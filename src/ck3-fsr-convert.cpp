// Fixed-size SDR conversion for the local whole-frame measurement worker.
// Tables are generated once by Python and retained for the lifetime of the call.
// No per-frame allocations, powers, half-float casts or Python image copies.
#include <cstdint>
#include <cstring>

extern "C" __declspec(dllexport) int fsr_convert_input(
    const uint8_t *input, uint16_t *output, int bgra,
    const uint16_t *srgb, const uint16_t *motion) {
    constexpr size_t pixels=1280*720, plane=pixels*4;
    for(size_t p=0;p<pixels;++p) {
        output[4*p+0]=srgb[input[4*p+(bgra?2:0)]];
        output[4*p+1]=srgb[input[4*p+1]];
        output[4*p+2]=srgb[input[4*p+(bgra?0:2)]];
        output[4*p+3]=0x3c00;
    }
    const auto depth=reinterpret_cast<const uint32_t*>(input+plane);
    for(size_t p=0;p<pixels;++p)
        if((depth[p]&0x7f800000)==0x7f800000) return 0;
    memcpy(reinterpret_cast<uint8_t*>(output)+plane*2,depth,plane);
    const auto vectors=reinterpret_cast<const uint16_t*>(input+plane*2);
    auto target=reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(output)+plane*3);
    for(size_t p=0;p<pixels*2;++p) {
        const uint16_t bits=vectors[p];
        if((bits&0x7c00)==0x7c00) return 0;
        target[p]=motion[bits];
    }
    return 1;
}

extern "C" __declspec(dllexport) int fsr_convert_output(
    const uint16_t *input, uint8_t *output, int bgra, const uint8_t *srgb) {
    for(size_t p=0;p<size_t(1920)*1080;++p) {
        const uint16_t r=input[4*p],g=input[4*p+1],b=input[4*p+2];
        if((r&0x7c00)==0x7c00 || (g&0x7c00)==0x7c00 || (b&0x7c00)==0x7c00) return 0;
        output[4*p+(bgra?2:0)]=srgb[r];
        output[4*p+1]=srgb[g];
        output[4*p+(bgra?0:2)]=srgb[b];
        output[4*p+3]=255;
    }
    return 1;
}
