"""Local game-frame worker for the explicit host-staged CK3 experiment.

Consumes real color/depth/estimated-motion planes from ck3-fsr-measure.addon64.
Never generates synthetic inputs or substitutes a missing GPU.
"""
from __future__ import annotations
import argparse
import ctypes as C
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sys
import time
import numpy as np
from fsr_benchmark import Runtime, Session, RUNTIME_SHA256
from vk_native import Vulkan, Device

PLANE = 1280*720*4
OUTPUT = 1920*1080*4

class Header(C.Structure):
    _fields_ = [(name,C.c_uint32) for name in "magic version inputW inputH outputW outputH colorBytes depthBytes motionBytes outputBytes status workerReady workerPid sourceW sourceH reset".split()] + [
        ("frameId",C.c_uint64),("replyId",C.c_uint64),("workerMs",C.c_double),("gpuMs",C.c_double),
        ("deltaMs",C.c_double),("gpuName",C.c_char*128),("message",C.c_char*512),
        ("bgra",C.c_uint32),("reserved",C.c_char*276)]
assert C.sizeof(Header)==1024 and Header.frameId.offset==64 and Header.gpuName.offset==104
SIZE = C.sizeof(Header)+PLANE*3+OUTPUT

# SDR input has only 256 possible channel values; FSR output has 65,536
# possible half-float bit patterns. Build exact conversion tables once, rather
# than evaluating millions of powers and float casts on every game frame.
_srgb = np.arange(256, dtype=np.float32)/255
_INPUT_LUT = np.where(_srgb<=.04045, _srgb/12.92, ((_srgb+.055)/1.055)**2.4).astype(np.float16)
_half = np.arange(65536, dtype=np.uint16).view(np.float16).astype(np.float32)
_finite = np.isfinite(_half)
_linear = np.clip(np.where(_finite, _half, 0), 0, 1)
_OUTPUT_LUT = np.round(np.clip(np.where(_linear<=.0031308, _linear*12.92,
                            1.055*_linear**(1/2.4)-.055),0,1)*255).astype(np.uint8)
# Motion arrives in 1080p pixel units. Preserve the existing 2/3 conversion,
# including its float32 arithmetic, but apply it to half-float bit patterns.
_motion = _half.copy()
_motion[_finite] *= np.float32(2/3)
_MOTION_LUT = _motion.astype(np.float16)
del _srgb, _half, _linear, _motion

def choose_gpu(devices, mode):
    vendor, kind = (0x1002,"integrated") if mode=="Integrated" else (0x10de,"discrete")
    matches = [d for d in devices if d["vendor_id"]==vendor and d["device_type"]==kind]
    if len(matches)!=1:
        raise RuntimeError(f"Expected exactly one {mode} processing GPU; found {len(matches)}. No fallback selected.")
    return matches[0]

def convert_inputs(raw, bgra):
    color = np.frombuffer(raw, np.uint8, count=PLANE).reshape(720,1280,4)
    rgba = _INPUT_LUT[color]
    rgba[:,:,3] = 1
    if bgra: rgba[:,:,[0,2]] = rgba[:,:,[2,0]]
    depth=raw[PLANE:PLANE*2]
    motion_bits=np.frombuffer(raw, np.uint16, offset=PLANE*2)
    if not np.isfinite(np.frombuffer(depth,np.float32)).all() or not _finite[motion_bits].all():
        raise RuntimeError("Nonfinite game depth or motion guides.")
    return rgba.tobytes()+depth+_MOTION_LUT[motion_bits].tobytes()

def convert_output(raw,bgra):
    bits=np.frombuffer(raw,np.uint16).reshape(1080,1920,4)
    if not _finite[bits[:,:,:3]].all(): raise RuntimeError("Nonfinite actual FSR output.")
    rgba=_OUTPUT_LUT[bits]
    rgba[:,:,3]=255
    if bgra: rgba[:,:,[0,2]] = rgba[:,:,[2,0]]
    return rgba.tobytes()


class NativeConverter:
    """Convert directly between shared memory and Vulkan's mapped buffers."""
    def __init__(self, path=None):
        path=Path(path) if path else Path(__file__).with_name('ck3-fsr-convert.dll')
        self.dll=C.CDLL(str(path.resolve()))
        self.dll.fsr_convert_input.argtypes=[C.c_void_p,C.c_void_p,C.c_int,C.c_void_p,C.c_void_p]
        self.dll.fsr_convert_output.argtypes=[C.c_void_p,C.c_void_p,C.c_int,C.c_void_p]
        self.dll.fsr_convert_input.restype=self.dll.fsr_convert_output.restype=C.c_int

    def input(self, source, destination, bgra):
        if not self.dll.fsr_convert_input(source,destination,bgra,_INPUT_LUT.ctypes.data,_MOTION_LUT.ctypes.data):
            raise RuntimeError('Nonfinite game depth or motion guides.')

    def output(self, source, destination, bgra):
        if not self.dll.fsr_convert_output(source,destination,bgra,_OUTPUT_LUT.ctypes.data):
            raise RuntimeError('Nonfinite actual FSR output.')

def run(args):
    k=C.WinDLL("kernel32",use_last_error=True)
    for name,result,parameters in [
        ("OpenFileMappingW",C.c_void_p,[C.c_uint32,C.c_int,C.c_wchar_p]),
        ("MapViewOfFile",C.c_void_p,[C.c_void_p,C.c_uint32,C.c_uint32,C.c_uint32,C.c_size_t]),
        ("OpenEventW",C.c_void_p,[C.c_uint32,C.c_int,C.c_wchar_p]),
        ("OpenProcess",C.c_void_p,[C.c_uint32,C.c_int,C.c_uint32]),
        ("WaitForSingleObject",C.c_uint32,[C.c_void_p,C.c_uint32]),
        ("SetEvent",C.c_int,[C.c_void_p]),("CloseHandle",C.c_int,[C.c_void_p]),
        ("UnmapViewOfFile",C.c_int,[C.c_void_p])]:
        fn=getattr(k,name); fn.restype=result; fn.argtypes=parameters
    args.output.mkdir(parents=True,exist_ok=True)
    report={"schema_version":1,"session":args.session,"mode":args.gpu,"status":"starting",
            "started_at":datetime.now(timezone.utc).isoformat(),"prototype":"serial_host_staged_whole_frame",
            "input":[1280,720],"output":[1920,1080],"engine_jitter":False,"motion":"estimated_DLSS5_Feed",
            "color_conversion":"native SDR lookup tables, directly between mapped buffers","frames":0}
    report_path=args.output/"worker.json"
    profile_file=(args.output/"worker-timings.csv").open("w",encoding="utf-8",buffering=1)
    profile_file.write("frame,input_convert_ms,upload_flush_ms,submit_wait_ms,fsr_gpu_ms,output_query_invalidate_ms,output_convert_ms,total_ms\n")
    def save():
        temp=report_path.with_suffix(".tmp"); temp.write_text(json.dumps(report,indent=2),encoding="utf-8"); temp.replace(report_path)
    save()
    process=k.OpenProcess(0x100000,False,args.game_pid)
    mapping=request=reply=view=None; header=None; session=None
    try:
        if not process: raise RuntimeError("Cannot monitor the requested CK3 process.")
        deadline=time.monotonic()+120
        while time.monotonic()<deadline:
            if k.WaitForSingleObject(process,0)==0: raise RuntimeError("CK3 exited before bridge initialization.")
            mapping=k.OpenFileMappingW(0xf001f,False,"Local\\CK3FSR-"+args.session)
            if mapping: break
            time.sleep(.25)
        if not mapping: raise RuntimeError("CK3 bridge did not create its session mapping within 120 seconds.")
        view=k.MapViewOfFile(mapping,0xf001f,0,0,SIZE)
        if not view: raise RuntimeError("Cannot map the CK3 bridge packet.")
        header=Header.from_address(view)
        for _ in range(100):
            if header.magic==0x31525346 and header.version==1: break
            time.sleep(.05)
        if header.magic!=0x31525346 or header.version!=1: raise RuntimeError("Bridge packet signature mismatch.")
        if (header.inputW,header.inputH,header.outputW,header.outputH)!=(1280,720,1920,1080):
            raise RuntimeError("Bridge dimensions do not match the 720p to 1080p experiment.")
        request=k.OpenEventW(0x100002,False,"Local\\CK3FSR-"+args.session+"-request")
        reply=k.OpenEventW(0x100002,False,"Local\\CK3FSR-"+args.session+"-reply")
        if not request or not reply: raise RuntimeError("Cannot open bridge synchronization events.")
        with Vulkan(debug_utils=True) as vk:
            gpu=choose_gpu(vk.inventory(),args.gpu)
            runtime=Runtime(); converter=NativeConverter()
            report.update(gpu_name=gpu["name"],gpu_uuid=gpu["uuid"],runtime=runtime.version,
                          runtime_sha256=RUNTIME_SHA256,status="ready")
            with Device(vk,gpu,fsr=True) as device:
                try:
                    session=Session(runtime,device,(1280,720,1920,1080))
                    header.gpuName=gpu["name"].encode()[:127]; header.workerPid=os.getpid(); header.workerReady=1
                    save()
                    while k.WaitForSingleObject(process,0)!=0:
                        if k.WaitForSingleObject(request,500)!=0: continue
                        begin=time.perf_counter(); sequence=header.frameId
                        converter.input(view+C.sizeof(Header),session.upload.mapped,bool(header.bgra)); after_input=time.perf_counter()
                        session.upload.flush(); after_upload=time.perf_counter()
                        reset=bool(header.reset or report["frames"]==0 or header.deltaMs>250)
                        def record(command):
                            session.record_upload(command)
                            session.record_dispatch(command,report["frames"],60,jitter_value=(0,0),
                                frame_delta=max(1,min(header.deltaMs,250)),reset=reset,motion_scale=(1,1))
                            session.images[3].copy(command,session.readback)
                            device.barrier(command,session.readback)
                        device.execute(record); session.completed+=1; after_submit=time.perf_counter()
                        gpu_ms=session.gpu_time(); session.readback.invalidate()
                        after_output_read=time.perf_counter()
                        converter.output(session.readback.mapped,view+C.sizeof(Header)+PLANE*3,bool(header.bgra)); end=time.perf_counter()
                        header.workerMs=(end-begin)*1000; header.gpuMs=gpu_ms
                        header.replyId=sequence; header.status=2; k.SetEvent(reply)
                        timings=[(b-a)*1000 for a,b in zip(
                            [begin,after_input,after_upload,after_submit,after_output_read],
                            [after_input,after_upload,after_submit,after_output_read,end])]
                        profile_file.write(f"{sequence},"+','.join(f"{v:.4f}" for v in
                            [*timings[:3],gpu_ms,*timings[3:],header.workerMs])+"\n")
                        report["frames"]+=1
                        if report["frames"]==1 or report["frames"]%60==0:
                            report.update(status="running",last_worker_ms=header.workerMs,last_fsr_gpu_ms=gpu_ms)
                            save()
                    report["status"]="game_exited"; save()
                finally:
                    if header: header.workerReady=0
                    if session: session.close()
    except Exception as exc:
        report.update(status="failed",error=str(exc)); save()
        if header:
            header.message=str(exc).encode()[:511]; header.status=3
            if reply: k.SetEvent(reply)
        raise
    finally:
        profile_file.close()
        if view: k.UnmapViewOfFile(view)
        for handle in (mapping,request,reply,process):
            if handle: k.CloseHandle(handle)

if __name__=="__main__":
    parser=argparse.ArgumentParser()
    parser.add_argument("--session",required=True)
    parser.add_argument("--gpu",choices=["Dedicated","Integrated"],required=True)
    parser.add_argument("--game-pid",type=int,required=True)
    parser.add_argument("--output",type=Path,required=True)
    args=parser.parse_args()
    if len(args.session)!=32 or any(c not in "0123456789abcdef" for c in args.session): parser.error("Invalid session token")
    run(args)
