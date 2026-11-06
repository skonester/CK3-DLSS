// Local whole-frame FSR measurement bridge. Independent of the portrait feeder.
// Serial host staging is deliberate and included in timing; not a production SR integration.
#include <windows.h>
#include <reshade.hpp>
#include <cstdio>
#include <cstring>
#include <string>
#include <share.h>

extern "C" __declspec(dllexport) const char *NAME = "CK3 FSR Measurement";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "Experimental whole-frame 720p to 1080p FSR measurement bridge";

namespace ck3fsr {
using namespace reshade::api;
constexpr uint32_t iw=1280, ih=720, ow=1920, oh=1080;
constexpr uint32_t plane=iw*ih*4, output=ow*oh*4, magic=0x31525346;
struct Header {
    uint32_t magic,version,inputW,inputH,outputW,outputH,colorBytes,depthBytes,motionBytes,outputBytes;
    uint32_t status,workerReady,workerPid,sourceW,sourceH,reset;
    uint64_t frameId,replyId;
    double workerMs,gpuMs,deltaMs;
    char gpuName[128], message[512];
    uint32_t bgra;
    char reserved[276];
};
static_assert(sizeof(Header)==1024);
constexpr uint32_t mapBytes=sizeof(Header)+plane*3+output;
static HANDLE mapping=nullptr, requestEvent=nullptr, replyEvent=nullptr;
static Header *header=nullptr;
static effect_runtime *owner=nullptr;
static device *dev=nullptr;
static command_queue *queue=nullptr;
static resource scaled[3]={}, result={}, readback={}, upload={};
static FILE *csv=nullptr, *log=nullptr;
static bool off=false, disabled=false;
static uint64_t frame=0;
static LARGE_INTEGER frequency={}, start={}, previous={};
static double lastCpu=0, frameDelta=16.6667;
static double lastReadback=0,lastInputCopy=0,lastWorkerWait=0,lastOutputCopy=0,lastUpload=0;
static uint32_t lastSuccess=0;
static std::string lastMessage;

static void Message(const char *text) {
    if(log && lastMessage!=text){ fprintf(log,"%s\n",text); fflush(log); }
    lastMessage=text;
    if(header){ strncpy_s(header->message,text,_TRUNCATE); }
}
static void ReleaseResources() {
    if(queue){ queue->flush_immediate_command_list(); queue->wait_idle(); }
    if(dev){ for(auto &r:scaled) if(r.handle){ dev->destroy_resource(r); r={}; }
        for(auto r:{result,readback,upload}) if(r.handle) dev->destroy_resource(r); }
    result={}; readback={}; upload={};
}
static void Destroy(effect_runtime *rt) {
    if(rt!=owner) return;
    ReleaseResources();
    if(csv){ fclose(csv); csv=nullptr; } if(log){ fclose(log); log=nullptr; }
    if(header){ UnmapViewOfFile(header); header=nullptr; }
    for(auto h:{mapping,requestEvent,replyEvent}) if(h) CloseHandle(h);
    mapping=requestEvent=replyEvent=nullptr; owner=nullptr; dev=nullptr; queue=nullptr;
}
static void Init(effect_runtime *rt) {
    if(owner || rt->get_device()->get_api()!=device_api::vulkan) return;
    wchar_t token[65]={}, dir[MAX_PATH]={}, mode[32]={};
    if(!GetEnvironmentVariableW(L"CK3_FSR_SESSION",token,65) ||
       !GetEnvironmentVariableW(L"CK3_FSR_OUTPUT",dir,MAX_PATH)) return;
    for(auto c:std::wstring(token)) if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f'))) return;
    GetEnvironmentVariableW(L"CK3_FSR_MODE",mode,32); off=wcscmp(mode,L"Off")==0;
    owner=rt; dev=rt->get_device(); queue=rt->get_command_queue();
    const std::wstring base=std::wstring(L"Local\\CK3FSR-")+token;
    mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,mapBytes,base.c_str());
    if(mapping) header=(Header*)MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,mapBytes);
    requestEvent=CreateEventW(nullptr,FALSE,FALSE,(base+L"-request").c_str());
    replyEvent=CreateEventW(nullptr,FALSE,FALSE,(base+L"-reply").c_str());
    log=_wfsopen((std::wstring(dir)+L"\\bridge.log").c_str(),L"w",_SH_DENYNO);
    csv=_wfsopen((std::wstring(dir)+L"\\frames.csv").c_str(),L"w",_SH_DENYNO);
    if(!header||!requestEvent||!replyEvent||!csv){ Message("ERROR: bridge initialization failed"); disabled=true; return; }
    memset(header,0,sizeof(Header));
    header->magic=magic; header->version=1; header->inputW=iw; header->inputH=ih;
    header->outputW=ow; header->outputH=oh;
    header->colorBytes=header->depthBytes=header->motionBytes=plane; header->outputBytes=output;
    fprintf(csv,"t_ms,frame,dt_ms,fsr_enabled,fsr_success,fsr_cpu_ms,worker_ms,fsr_gpu_ms,input_w,input_h,output_w,output_h,gpu,readback_wait_ms,input_copy_ms,worker_wait_ms,output_copy_ms,upload_wait_ms\n"); fflush(csv);
    QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&start); previous={};
    Message(off?"FSR OFF baseline; expected composite output=1920x1080":"Awaiting FSR worker; input=1280x720 output=1920x1080; host-staged prototype");
}
static bool Resources(format colorFormat) {
    const auto usage=resource_usage::copy_source|resource_usage::copy_dest;
    const format formats[]={colorFormat,format::r32_float,format::r16g16_float};
    for(int i=0;i<3;++i) if(!dev->create_resource(resource_desc(iw,ih,1,1,formats[i],1,memory_heap::default_,usage),nullptr,resource_usage::copy_dest,&scaled[i])) return false;
    return dev->create_resource(resource_desc(ow,oh,1,1,colorFormat,1,memory_heap::default_,usage),nullptr,resource_usage::copy_dest,&result) &&
        dev->create_resource(resource_desc(uint64_t(plane)*3,memory_heap::readback,resource_usage::copy_dest),nullptr,resource_usage::copy_dest,&readback) &&
        dev->create_resource(resource_desc(uint64_t(output),memory_heap::upload,resource_usage::copy_source),nullptr,resource_usage::copy_source,&upload);
}
static void Finish(effect_runtime *rt,command_list *cl,resource_view rtv,resource_view) {
    if(rt!=owner) return;
    lastSuccess=0; lastCpu=0;
    lastReadback=lastInputCopy=lastWorkerWait=lastOutputCopy=lastUpload=0;
    if(!header || disabled) return;
    const auto bb=dev->get_resource_from_view(rtv);
    const auto desc=dev->get_resource_desc(bb);
    header->sourceW=desc.texture.width; header->sourceH=desc.texture.height;
    if(off || !header->workerReady) return;
    if(desc.texture.width!=ow || desc.texture.height!=oh || desc.texture.samples!=1){ Message("ERROR: composite must be 1920x1080 single-sample"); return; }
    // ReShade may expose a typeless resource even when the Vulkan view is UNORM.
    // The staging images need the concrete byte-compatible UNORM format.
    const auto fmt=format_to_default_typed(desc.texture.format,0);
    const bool bgra=fmt==format::b8g8r8a8_unorm||fmt==format::b8g8r8a8_unorm_srgb;
    if(!bgra && fmt!=format::r8g8b8a8_unorm && fmt!=format::r8g8b8a8_unorm_srgb){
        char message[160]; sprintf_s(message,"ERROR: SDR RGBA8/BGRA8 required; resource format=%u view format=%u",unsigned(desc.texture.format),unsigned(dev->get_resource_view_desc(rtv).format)); Message(message); return; }
    header->bgra=bgra;
    if(cl!=queue->get_immediate_command_list()){ Message("ERROR: effect command list is not the immediate queue list"); disabled=true; return; }
    auto mv=rt->find_texture_variable("DLSS5_Feed.fx","DLSS5_MV");
    auto dp=rt->find_texture_variable("DLSS5_Feed.fx","DLSS5_Depth");
    resource_view mvView={},dpView={},unused={};
    if(mv.handle) rt->get_texture_binding(mv,&mvView,&unused);
    if(dp.handle) rt->get_texture_binding(dp,&dpView,&unused);
    if(!mvView.handle||!dpView.handle){ Message("ERROR: DLSS5_Feed depth/motion guides unavailable"); return; }
    const resource sources[]={bb,dev->get_resource_from_view(dpView),dev->get_resource_from_view(mvView)};
    if(format_to_default_typed(dev->get_resource_desc(sources[1]).texture.format)!=format::r32_float ||
       format_to_default_typed(dev->get_resource_desc(sources[2]).texture.format)!=format::r16g16_float){ Message("ERROR: unexpected guide texture format"); return; }
    if(!scaled[0].handle && !Resources(fmt)){ Message("ERROR: staging resource allocation failed"); disabled=true; return; }
    LARGE_INTEGER begin={},end={},afterReadback={},afterInput={},afterWorker={},afterOutput={}; QueryPerformanceCounter(&begin);
    for(int i=0;i<3;++i){
        const auto original=i==0?resource_usage::render_target:resource_usage::shader_resource;
        cl->barrier(sources[i],original,resource_usage::copy_source);
        cl->copy_texture_region(sources[i],0,nullptr,scaled[i],0,nullptr,filter_mode::min_mag_mip_linear);
        cl->barrier(scaled[i],resource_usage::copy_dest,resource_usage::copy_source);
        cl->copy_texture_to_buffer(scaled[i],0,nullptr,readback,uint64_t(i)*plane);
        cl->barrier(scaled[i],resource_usage::copy_source,resource_usage::copy_dest);
        cl->barrier(sources[i],resource_usage::copy_source,original);
    }
    queue->flush_immediate_command_list(); queue->wait_idle();
    QueryPerformanceCounter(&afterReadback);
    void *mapped=nullptr;
    if(!dev->map_buffer_region(readback,0,uint64_t(plane)*3,map_access::read_only,&mapped)){ Message("ERROR: readback map failed"); disabled=true; return; }
    memcpy((char*)header+sizeof(Header),mapped,size_t(plane)*3); dev->unmap_buffer_region(readback);
    QueryPerformanceCounter(&afterInput);
    header->frameId=++frame; header->reset=frame==1;
    header->deltaMs=frameDelta;
    header->status=1; ResetEvent(replyEvent); MemoryBarrier(); SetEvent(requestEvent);
    if(WaitForSingleObject(replyEvent,2000)!=WAIT_OBJECT_0 || header->status!=2 || header->replyId!=frame){ Message("ERROR: FSR worker failed or timed out; FSR disabled"); disabled=true; return; }
    QueryPerformanceCounter(&afterWorker);
    if(!dev->map_buffer_region(upload,0,output,map_access::write_only,&mapped)){ Message("ERROR: output map failed"); disabled=true; return; }
    memcpy(mapped,(char*)header+sizeof(Header)+size_t(plane)*3,output); dev->unmap_buffer_region(upload);
    QueryPerformanceCounter(&afterOutput);
    cl->copy_buffer_to_texture(upload,0,0,0,result,0);
    cl->barrier(result,resource_usage::copy_dest,resource_usage::copy_source);
    cl->barrier(bb,resource_usage::render_target,resource_usage::copy_dest);
    cl->copy_resource(result,bb);
    cl->barrier(bb,resource_usage::copy_dest,resource_usage::render_target);
    cl->barrier(result,resource_usage::copy_source,resource_usage::copy_dest);
    // Include completion of the returned upload in serial prototype timing.
    queue->flush_immediate_command_list(); queue->wait_idle();
    QueryPerformanceCounter(&end); lastCpu=1000.0*double(end.QuadPart-begin.QuadPart)/frequency.QuadPart;
    auto ms=[](LARGE_INTEGER a,LARGE_INTEGER b){ return 1000.0*double(b.QuadPart-a.QuadPart)/frequency.QuadPart; };
    lastReadback=ms(begin,afterReadback); lastInputCopy=ms(afterReadback,afterInput);
    lastWorkerWait=ms(afterInput,afterWorker); lastOutputCopy=ms(afterWorker,afterOutput); lastUpload=ms(afterOutput,end);
    lastSuccess=1;
    if(frame==1 || frame%600==0){ char message[256]; sprintf_s(message,"FSR delivered frame=%llu input=1280x720 output=1920x1080 gpu=%s",frame,header->gpuName); Message(message); }
}
static void Present(effect_runtime *rt) {
    if(rt!=owner || !csv) return;
    LARGE_INTEGER now={}; QueryPerformanceCounter(&now);
    const double dt=previous.QuadPart?1000.0*double(now.QuadPart-previous.QuadPart)/frequency.QuadPart:0;
    const double t=1000.0*double(now.QuadPart-start.QuadPart)/frequency.QuadPart;
    fprintf(csv,"%.4f,%llu,%.4f,%d,%u,%.4f,%.4f,%.4f,%u,%u,%u,%u,%s,%.4f,%.4f,%.4f,%.4f,%.4f\n",t,frame,dt,!off,lastSuccess,lastCpu,
        header?header->workerMs:0,header?header->gpuMs:0,iw,ih,header?header->sourceW:0,header?header->sourceH:0,
        off?"Off":header?header->gpuName:"",lastReadback,lastInputCopy,lastWorkerWait,lastOutputCopy,lastUpload);
    fflush(csv); previous=now; if(dt>0) frameDelta=dt;
}
}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH){
        if(!reshade::register_addon(module)) return FALSE;
        reshade::register_event<reshade::addon_event::init_effect_runtime>(ck3fsr::Init);
        reshade::register_event<reshade::addon_event::destroy_effect_runtime>(ck3fsr::Destroy);
        reshade::register_event<reshade::addon_event::reshade_finish_effects>(ck3fsr::Finish);
        reshade::register_event<reshade::addon_event::reshade_present>(ck3fsr::Present);
    } else if(reason==DLL_PROCESS_DETACH){
        reshade::unregister_event<reshade::addon_event::init_effect_runtime>(ck3fsr::Init);
        reshade::unregister_event<reshade::addon_event::destroy_effect_runtime>(ck3fsr::Destroy);
        reshade::unregister_event<reshade::addon_event::reshade_finish_effects>(ck3fsr::Finish);
        reshade::unregister_event<reshade::addon_event::reshade_present>(ck3fsr::Present);
        reshade::unregister_addon(module);
    }
    return TRUE;
}
