// Frontier 3 render-dump probe helpers: SPIR-V binding parse, texel decode, alpha statistics,
// UV-to-texel mapping and the PNG writer. No game, ReShade or GPU needed.
#include "../src/feed_dump_probe.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static void Require(bool ok, const char *message)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

// ---------------------------------------------------------------------------
// Hand-assembled SPIR-V (only the instructions the parser reads)
// ---------------------------------------------------------------------------

struct Spv
{
    std::vector<uint32_t> w = { 0x07230203u, 0x00010000u, 0, 1000, 0 };
    void op(uint32_t code, std::initializer_list<uint32_t> operands)
    {
        w.push_back((uint32_t(operands.size() + 1) << 16) | code);
        w.insert(w.end(), operands.begin(), operands.end());
    }
    void op_str(uint32_t code, std::initializer_list<uint32_t> operands, const char *text)
    {
        std::vector<uint32_t> s((strlen(text) + 4) / 4, 0);
        memcpy(s.data(), text, strlen(text));
        w.push_back((uint32_t(operands.size() + s.size() + 1) << 16) | code);
        w.insert(w.end(), operands.begin(), operands.end());
        w.insert(w.end(), s.begin(), s.end());
    }
    void name(uint32_t id, const char *n) { op_str(5, { id }, n); }
    void member_name(uint32_t id, uint32_t m, const char *n) { op_str(6, { id, m }, n); }
    void decorate_set(uint32_t id, uint32_t set) { op(71, { id, 34, set }); }
    void decorate_binding(uint32_t id, uint32_t b) { op(71, { id, 33, b }); }
    void member_offset(uint32_t id, uint32_t m, uint32_t off) { op(72, { id, m, 35, off }); }
};

// Vertex-like stage: the cbuffer with deliberately non-default offsets (+64), so the test
// proves offsets come from the module rather than the built-in defaults.
static Spv VertexStage()
{
    Spv s;
    const uint32_t f32 = 1, v2 = 2, block = 3, ptr = 4, var = 5;
    static const char *members[8] = { "WidgetPos", "WidgetSize", "HighlightColor", "PopOutThreshold",
                                      "PortraitUVOffset", "PortraitUVScale", "PortraitTextureSize", "IsGrayscale" };
    static const uint32_t offsets[8] = { 0, 8, 16, 28, 32, 40, 48, 56 };
    s.name(block, "type.pdx_hlsl_cb38");
    for (uint32_t m = 0; m < 8; ++m) s.member_name(block, m, members[m]);
    s.name(var, "pdx_hlsl_cb38");
    s.decorate_set(var, 0);
    s.decorate_binding(var, 2);
    for (uint32_t m = 0; m < 8; ++m) s.member_offset(block, m, offsets[m] + 64);
    s.op(22, { f32, 32 });
    s.op(23, { v2, f32, 2 });
    s.op(30, { block, v2, v2, v2, f32, v2, v2, v2, f32 });
    s.op(32, { ptr, 2, block });     // Uniform
    s.op(59, { ptr, var, 2 });
    return s;
}

// Pixel-like stage: four textures plus a sampler, optionally named, with one texture
// declared as an array to exercise array unwrapping.
static Spv PixelStage(bool names, bool combined)
{
    Spv s;
    const uint32_t f32 = 1, img = 2, sampled = 3, arr = 4, len = 5, u32 = 6, ptr_img = 7, ptr_arr = 8, smp = 9, ptr_smp = 10;
    const uint32_t vars[4] = { 20, 21, 22, 23 }, sampler_var = 24;
    static const char *tex_names[4] = { "Frame", "Mask", "Portrait", "Background" };
    if (names)
    {
        for (int i = 0; i < 4; ++i) s.name(vars[i], tex_names[i]);
        s.name(sampler_var, "PortraitSampler");
    }
    // Shuffle bindings so binding order still maps to Frame, Mask, Portrait, Background.
    const uint32_t bindings[4] = { 3, 5, 7, 9 };
    for (int i = 0; i < 4; ++i) { s.decorate_set(vars[i], 1); s.decorate_binding(vars[i], bindings[i]); }
    s.decorate_set(sampler_var, 1); s.decorate_binding(sampler_var, 8);
    s.op(22, { f32, 32 });
    s.op(21, { u32, 32, 0 });
    s.op(25, { img, f32, 1, 0, 0, 0, 1, 0 });
    s.op(27, { sampled, img });
    s.op(43, { u32, len, 1 });       // OpConstant (ignored by the parser)
    s.op(28, { arr, combined ? sampled : img, len });
    s.op(26, { smp });
    s.op(32, { ptr_img, 0, combined ? sampled : img });   // UniformConstant
    s.op(32, { ptr_arr, 0, arr });
    s.op(32, { ptr_smp, 0, smp });
    s.op(59, { ptr_img, vars[0], 0 });
    s.op(59, { ptr_img, vars[1], 0 });
    s.op(59, { ptr_arr, vars[2], 0 });   // Portrait declared as a one-element array
    s.op(59, { ptr_img, vars[3], 0 });
    s.op(59, { ptr_smp, sampler_var, 0 });
    return s;
}

static void SpirvTests()
{
    const Spv vs = VertexStage();
    std::string log;
    const dprobe::CbInfo v = dprobe::SpvParsePortrait(vs.w.data(), vs.w.size(), &log);
    Require(v.found && !v.push_constant && v.set == 0 && v.binding == 2, "cbuffer set/binding");
    Require(v.off_pos == 64 && v.off_size == 72 && v.off_popout == 92 && v.off_uvoff == 96 && v.off_uvscale == 104 &&
            v.off_tex == 112 && v.off_gray == 120, "member offsets come from the module");
    Require(log.find("pdx_hlsl_cb38") != std::string::npos, "bindings log names the cbuffer");
    Require(!v.img[dprobe::IMG_PORTRAIT].found, "vertex stage has no textures");

    for (int combined = 0; combined < 2; ++combined)
    {
        const Spv ps = PixelStage(true, combined != 0);
        const dprobe::CbInfo p = dprobe::SpvParsePortrait(ps.w.data(), ps.w.size());
        Require(!p.found && p.img_by_name, "pixel stage: textures by name");
        Require(p.img[dprobe::IMG_FRAME].binding == 3 && p.img[dprobe::IMG_MASK].binding == 5 &&
                p.img[dprobe::IMG_PORTRAIT].binding == 7 && p.img[dprobe::IMG_BACKGROUND].binding == 9 &&
                p.img[dprobe::IMG_PORTRAIT].set == 1, "named textures map to their bindings (sampler ignored, array unwrapped)");
        dprobe::CbInfo merged = v;
        dprobe::MergeCb(merged, p);
        Require(merged.found && merged.off_uvoff == 96 && merged.img[dprobe::IMG_PORTRAIT].found && merged.img_by_name, "vertex + pixel merge");
        dprobe::CbInfo reverse = p;
        dprobe::MergeCb(reverse, v);
        Require(reverse.found && reverse.off_uvoff == 96 && reverse.img[dprobe::IMG_PORTRAIT].binding == 7, "merge is order independent");
    }

    const Spv stripped = PixelStage(false, true);
    const dprobe::CbInfo s = dprobe::SpvParsePortrait(stripped.w.data(), stripped.w.size());
    Require(!s.img_by_name && s.img[dprobe::IMG_PORTRAIT].found && s.img[dprobe::IMG_PORTRAIT].binding == 7 &&
            s.img[dprobe::IMG_FRAME].binding == 3, "stripped names fall back to the shader's sampler index order");

    const uint32_t junk[6] = { 0xDEADBEEFu, 1, 2, 3, 4, 5 };
    Require(!dprobe::SpvParsePortrait(junk, 6).found, "non-SPIR-V input is rejected");
    std::vector<uint32_t> truncated = vs.w;
    truncated.resize(12);
    (void)dprobe::SpvParsePortrait(truncated.data(), truncated.size());   // must not read past the end
    std::puts("PASS SPIR-V: cbuffer offsets, named/stripped texture bindings, arrays, samplers ignored, stage merge");
}

// ---------------------------------------------------------------------------

static void DecodeTests()
{
    Require(dprobe::HalfToFloat(0x3C00) == 1.0f && dprobe::HalfToFloat(0xC000) == -2.0f &&
            dprobe::HalfToFloat(0x7BFF) == 65504.0f && dprobe::HalfToFloat(0x0001) == std::ldexp(1.0f, -24) &&
            dprobe::HalfToFloat(0x0000) == 0.0f && std::isinf(dprobe::HalfToFloat(0x7C00)), "half to float");
    float c[4];
    const uint8_t rgba8[4] = { 10, 20, 30, 255 };
    Require(dprobe::DecodeTexel(28, rgba8, c) && c[0] == 10 / 255.0f && c[3] == 1.0f, "RGBA8");
    Require(dprobe::DecodeTexel(87, rgba8, c) && c[0] == 30 / 255.0f && c[2] == 10 / 255.0f, "BGRA8 swizzle");
    const uint32_t r10 = 1023u | (512u << 10) | (0u << 20) | (2u << 30);
    Require(dprobe::DecodeTexel(24, reinterpret_cast<const uint8_t *>(&r10), c) && c[0] == 1.0f && std::fabs(c[3] - 2.0f / 3.0f) < 1e-6f, "R10G10B10A2");
    const uint16_t h4[4] = { 0x3800, 0x0000, 0x3C00, 0x3C00 };
    Require(dprobe::DecodeTexel(10, reinterpret_cast<const uint8_t *>(h4), c) && c[0] == 0.5f && c[3] == 1.0f, "RGBA16F");
    const uint8_t a8 = 128;
    Require(dprobe::DecodeTexel(65, &a8, c) && c[0] == 0.0f && c[3] == 128 / 255.0f, "A8 reads as alpha");
    Require(dprobe::DecodeTexel(61, &a8, c) && c[3] == 128 / 255.0f, "R8 reads as coverage");
    Require(!dprobe::DecodeTexel(71, rgba8, c) && dprobe::BytesPerTexel(71) == 0, "BC1 is refused");
    std::puts("PASS texel decode: RGBA8, BGRA8, R10G10B10A2, RGBA16F, R8, A8; compressed refused");
}

// A premultiplied 16x16 "character": opaque disc, soft one-texel rim, transparent border.
static std::vector<uint8_t> Character(bool premultiplied)
{
    std::vector<uint8_t> img(16 * 16 * 4, 0);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x)
        {
            const float d = std::sqrt((x - 7.5f) * (x - 7.5f) + (y - 7.5f) * (y - 7.5f));
            const uint8_t a = d < 4.5f ? 255 : d < 5.5f ? 128 : 0;
            uint8_t *p = &img[(y * 16 + x) * 4];
            const uint8_t color = 200;
            p[0] = p[1] = p[2] = premultiplied ? uint8_t(color * a / 255) : color;
            p[3] = a;
        }
    return img;
}

static void StatsTests()
{
    const std::vector<uint8_t> img = Character(true);
    const dprobe::AlphaStats s = dprobe::Measure(img.data(), 64, 28, 0, 0, 16, 16);
    Require(s.n == 256 && s.zero + s.full + s.partial == 256, "every texel classified once");
    Require(s.full > 40 && s.partial > 10 && s.zero > 100, "disc, rim and border all present");
    Require(s.ring_n == 16 * 16 - 12 * 12 && s.ring_zero == s.ring_n, "transparent edge ring detected");
    Require(s.premult_violations == 0, "premultiplied colour accepted");
    const std::vector<uint8_t> straight = Character(false);
    Require(dprobe::Measure(straight.data(), 64, 28, 0, 0, 16, 16).premult_violations > 100, "straight alpha flagged");
    const dprobe::AlphaStats inner = dprobe::Measure(img.data(), 64, 28, 6, 6, 10, 10);
    Require(inner.n == 16 && inner.full == 16, "sub-rectangle restricts the measurement");
    Require(dprobe::Measure(img.data(), 64, 28, 5, 5, 5, 9).n == 0, "empty rectangle");
    Require(dprobe::Measure(img.data(), 64, 71, 0, 0, 16, 16).n == 0, "unsupported format measures nothing");

    int x0, y0, x1, y1;
    Require(dprobe::UvTexelRect(0, 0, 1, 1, 380, 421, x0, y0, x1, y1) && x0 == 0 && y0 == 0 && x1 == 380 && y1 == 421, "identity UV covers the texture");
    Require(dprobe::UvTexelRect(0, 0, 2, 2, 400, 400, x0, y0, x1, y1) && x1 == 200 && y1 == 200, "scale 2 samples the top-left quarter");
    Require(dprobe::UvTexelRect(-0.5f, 0, 2, 1, 400, 400, x0, y0, x1, y1) && x0 == 100 && x1 == 300, "offset shifts the sampled window");
    Require(dprobe::UvTexelRect(0.25f, 0, 0.5f, 1, 400, 400, x0, y0, x1, y1) && x0 == 0 && x1 == 400, "zoomed-out window is clamped");
    Require(!dprobe::UvTexelRect(0, 0, 0, 1, 400, 400, x0, y0, x1, y1), "zero scale is degenerate");
    Require(!dprobe::UvTexelRect(NAN, 0, 1, 1, 400, 400, x0, y0, x1, y1), "NaN is degenerate");
    Require(!dprobe::UvTexelRect(2.0f, 0, 1, 1, 400, 400, x0, y0, x1, y1), "window entirely outside the texture");
    std::puts("PASS alpha statistics and UV mapping: classes, edge ring, premultiplication, sub-rectangles, clamps, degenerate input");
}

// ---------------------------------------------------------------------------

static uint32_t BE(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

// Verifies chunk CRCs and inflates the stored blocks; returns the image bytes without filter bytes.
static std::vector<uint8_t> DecodeStoredPng(const std::vector<uint8_t> &png, uint32_t &w, uint32_t &h, int &channels)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    Require(png.size() > 8 && memcmp(png.data(), sig, 8) == 0, "PNG signature");
    std::vector<uint8_t> z;
    bool end = false;
    for (size_t pos = 8; pos < png.size();)
    {
        Require(pos + 12 <= png.size(), "chunk header in bounds");
        const uint32_t len = BE(&png[pos]);
        Require(pos + 12 + len <= png.size(), "chunk body in bounds");
        const char *type = reinterpret_cast<const char *>(&png[pos + 4]);
        Require(dprobe::Crc32(&png[pos + 4], 4 + len) == BE(&png[pos + 8 + len]), "chunk CRC");
        if (memcmp(type, "IHDR", 4) == 0)
        {
            w = BE(&png[pos + 8]); h = BE(&png[pos + 12]);
            Require(png[pos + 16] == 8, "8-bit");
            channels = png[pos + 17] == 6 ? 4 : png[pos + 17] == 0 ? 1 : 0;
        }
        else if (memcmp(type, "IDAT", 4) == 0) z.insert(z.end(), png.begin() + pos + 8, png.begin() + pos + 8 + len);
        else if (memcmp(type, "IEND", 4) == 0) end = true;
        pos += 12 + len;
    }
    Require(end && z.size() > 6 && z[0] == 0x78 && ((z[0] << 8) | z[1]) % 31 == 0, "zlib header");
    std::vector<uint8_t> raw;
    size_t pos = 2;
    for (bool final = false; !final;)
    {
        Require(pos + 5 <= z.size() && (z[pos] & 6) == 0, "stored block");
        final = (z[pos] & 1) != 0;
        const uint16_t len = uint16_t(z[pos + 1] | (z[pos + 2] << 8)), nlen = uint16_t(z[pos + 3] | (z[pos + 4] << 8));
        Require(uint16_t(~len) == nlen && pos + 5 + len <= z.size(), "stored length");
        raw.insert(raw.end(), z.begin() + pos + 5, z.begin() + pos + 5 + len);
        pos += 5 + len;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    Require(pos + 4 == z.size() && BE(&z[pos]) == ((b << 16) | a), "Adler-32");
    std::vector<uint8_t> pixels;
    const size_t row = size_t(w) * channels;
    Require(raw.size() == (row + 1) * h, "raw size");
    for (uint32_t y = 0; y < h; ++y)
    {
        Require(raw[y * (row + 1)] == 0, "filter none");
        pixels.insert(pixels.end(), raw.begin() + y * (row + 1) + 1, raw.begin() + (y + 1) * (row + 1));
    }
    return pixels;
}

static void PngTests()
{
    Require(dprobe::Crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xCBF43926u, "CRC-32 check value");
    for (int channels : { 4, 1 })
        for (uint32_t size : { 3u, 211u })   // 211x211x4 crosses several 64 KiB stored blocks
        {
            std::vector<uint8_t> px(size_t(size) * size * channels);
            for (size_t i = 0; i < px.size(); ++i) px[i] = uint8_t(i * 7 + 3);
            const std::vector<uint8_t> png = dprobe::EncodePng(size, size, px.data(), channels);
            uint32_t w = 0, h = 0; int c = 0;
            Require(DecodeStoredPng(png, w, h, c) == px && w == size && h == size && c == channels, "PNG round trip");
        }
    const std::vector<uint8_t> img = Character(true);
    std::vector<uint8_t> rgba, alpha;
    Require(dprobe::ToRgba8(img.data(), 64, 28, 16, 16, rgba, alpha) && rgba == img && alpha[7 * 16 + 7] == 255 && alpha[0] == 0, "RGBA8 conversion");
    const uint16_t hdr[4] = { 0x4400 /* 4.0 */, 0xBC00 /* -1 */, 0x7E00 /* NaN */, 0x3800 };
    Require(dprobe::ToRgba8(reinterpret_cast<const uint8_t *>(hdr), 8, 10, 1, 1, rgba, alpha) &&
            rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 128, "float conversion clamps and drops NaN");
    const char *path = "build\\compat-tests\\render-dump-probe.png";
    Require(dprobe::WritePng(path, 16, 16, img.data(), 4), "PNG file written");
    FILE *f = nullptr;
    Require(fopen_s(&f, path, "rb") == 0 && f != nullptr, "PNG file readable");
    std::vector<uint8_t> file;
    for (int ch; (ch = fgetc(f)) != EOF;) file.push_back(uint8_t(ch));
    fclose(f);
    uint32_t fw = 0, fh = 0; int fc = 0;
    Require(DecodeStoredPng(file, fw, fh, fc) == img, "PNG file round trip");
    std::puts("PASS PNG writer: CRC, multi-block stored deflate, Adler-32, RGBA and gray, float clamping, file output");
}

int main()
{
    SpirvTests();
    DecodeTests();
    StatsTests();
    PngTests();
    std::puts("PASS render-dump probe helpers");
}
