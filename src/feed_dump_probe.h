// feed_dump_probe.h -- Frontier 3: game-independent helpers for the render dumper's portrait
// probe. No ReShade, Vulkan or Windows dependencies, so tests/render-dump-probe.cpp can
// check every piece offline.
//
//   * SPIR-V scan of the GUI portrait shaders: the WidgetPos/WidgetSize cbuffer and its UV
//     members, plus the set/binding of the four textures gui_portrait.shader samples
//     (Frame, Mask, Portrait, Background -- TextureSampler indices 0..3).
//   * Texel decode for the formats a portrait texture can plausibly use.
//   * Alpha statistics over the texel rectangle a draw actually samples.
//   * A minimal PNG writer (stored deflate blocks) for visual inspection of probed textures.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace dprobe
{
// gui_portrait.shader: TextureSampler Frame=0, Mask=1, Portrait=2, Background=3.
enum { IMG_FRAME = 0, IMG_MASK, IMG_PORTRAIT, IMG_BACKGROUND, IMG_COUNT };
static const char *kImgName[IMG_COUNT] = { "Frame", "Mask", "Portrait", "Background" };
static const char kImgLetter[IMG_COUNT] = { 'F', 'M', 'P', 'B' };

struct ImgBinding { bool found = false; uint32_t set = 0, binding = 0; };

struct CbInfo
{
    bool     found = false;
    bool     push_constant = false;   // the cbuffer is a push-constant block, not a descriptor
    uint32_t set = 0, binding = 0;
    // HLSL packing of jomini gui_portrait.shader's ConstantBuffer(2); parsed when names exist.
    uint32_t off_pos = 0, off_size = 8, off_popout = 28, off_uvoff = 32, off_uvscale = 40, off_tex = 48, off_gray = 56;
    ImgBinding img[IMG_COUNT];
    bool     img_by_name = false;     // false: binding-order fallback (or nothing found)
};

static const char *SpvString(const uint32_t *w, uint32_t words)
{
    const char *s = reinterpret_cast<const char *>(w);
    return strnlen(s, words * 4u) < words * 4u ? s : "";
}

static bool ContainsNoCase(const std::string &hay, const char *needle)
{
    std::string a = hay, b = needle;
    for (char &c : a) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    for (char &c : b) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}

// Parses one shader stage. `bindings_log` (optional) receives one line per named descriptor.
static CbInfo SpvParsePortrait(const uint32_t *w, size_t count, std::string *bindings_log = nullptr)
{
    CbInfo out;
    if (w == nullptr || count < 5 || w[0] != 0x07230203u) return out;

    struct Member { uint32_t index; std::string name; };
    std::unordered_map<uint32_t, std::vector<Member>> member_names;   // struct id -> names
    std::unordered_map<uint64_t, uint32_t> member_offsets;            // (struct<<32)|member -> offset
    std::unordered_map<uint32_t, std::string> names;                  // id -> OpName
    std::unordered_map<uint32_t, uint32_t> sets, bindings;            // id -> decoration value
    std::unordered_map<uint32_t, uint32_t> ptr_pointee;               // pointer type -> pointee
    std::unordered_map<uint32_t, uint8_t>  kind;                      // type id -> 1 image, 2 sampler, 3 sampled image
    std::unordered_map<uint32_t, uint32_t> array_elem;                // array type -> element type
    struct Var { uint32_t type, id, storage; };
    std::vector<Var> vars;

    for (size_t i = 5; i < count;)
    {
        const uint32_t op = w[i] & 0xFFFFu, wc = w[i] >> 16;
        if (wc == 0 || i + wc > count) break;
        const uint32_t *o = w + i;
        switch (op)
        {
        case 5:  if (wc >= 3) names[o[1]] = SpvString(o + 2, wc - 2); break;                          // OpName
        case 6:  if (wc >= 4) member_names[o[1]].push_back({ o[2], SpvString(o + 3, wc - 3) }); break; // OpMemberName
        case 71: if (wc >= 4) { if (o[2] == 34) sets[o[1]] = o[3]; else if (o[2] == 33) bindings[o[1]] = o[3]; } break; // OpDecorate
        case 72: if (wc >= 5 && o[3] == 35) member_offsets[(uint64_t(o[1]) << 32) | o[2]] = o[4]; break; // OpMemberDecorate Offset
        case 25: if (wc >= 2) kind[o[1]] = 1; break;                                                 // OpTypeImage
        case 26: if (wc >= 2) kind[o[1]] = 2; break;                                                 // OpTypeSampler
        case 27: if (wc >= 2) kind[o[1]] = 3; break;                                                 // OpTypeSampledImage
        case 28: case 29: if (wc >= 3) array_elem[o[1]] = o[2]; break;                               // OpType(Runtime)Array
        case 32: if (wc >= 4) ptr_pointee[o[1]] = o[3]; break;                                       // OpTypePointer
        case 59: if (wc >= 4) vars.push_back({ o[1], o[2], o[3] }); break;                           // OpVariable
        }
        i += wc;
    }

    auto pointee_kind = [&](const Var &v) -> uint8_t {
        const auto pp = ptr_pointee.find(v.type);
        if (pp == ptr_pointee.end()) return 0;
        uint32_t t = pp->second;
        for (int depth = 0; depth < 4; ++depth)
        {
            const auto ae = array_elem.find(t);
            if (ae == array_elem.end()) break;
            t = ae->second;
        }
        const auto k = kind.find(t);
        return k != kind.end() ? k->second : 0;
    };

    // Constant buffer: the struct with WidgetPos + WidgetSize members.
    uint32_t block = 0;
    uint32_t idx[7] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    static const char *kMembers[7] = { "WidgetPos", "WidgetSize", "PopOutThreshold", "PortraitUVOffset", "PortraitUVScale",
                                       "PortraitTextureSize", "IsGrayscale" };
    for (const auto &kv : member_names)
    {
        uint32_t found[7] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
        for (const Member &m : kv.second)
            for (int k = 0; k < 7; ++k)
                if (m.name == kMembers[k]) found[k] = m.index;
        if (found[0] != UINT32_MAX && found[1] != UINT32_MAX) { block = kv.first; memcpy(idx, found, sizeof(idx)); break; }
    }

    struct ImgVar { std::string name; uint32_t set, binding; };
    std::vector<ImgVar> images;
    for (const Var &v : vars)
    {
        if (!sets.count(v.id) && !bindings.count(v.id)) continue;
        const uint8_t k = pointee_kind(v);
        const auto nm = names.find(v.id);
        const std::string name = nm != names.end() ? nm->second : "";
        const uint32_t set = sets.count(v.id) ? sets[v.id] : 0, binding = bindings.count(v.id) ? bindings[v.id] : 0;
        if (bindings_log != nullptr)
        {
            char line[200];
            snprintf(line, sizeof(line), "set=%u binding=%u storage=%u kind=%s %s\n", set, binding, v.storage,
                     k == 1 ? "image" : k == 2 ? "sampler" : k == 3 ? "sampled-image" : "other", name.empty() ? "?" : name.c_str());
            *bindings_log += line;
        }
        if (k == 1 || k == 3) images.push_back({ name, set, binding });
    }

    if (block != 0)
        for (const Var &v : vars)
        {
            const auto pp = ptr_pointee.find(v.type);
            if (pp == ptr_pointee.end() || pp->second != block) continue;
            out.found = true;
            out.push_constant = v.storage == 9;   // StorageClass PushConstant
            out.set = sets.count(v.id) ? sets[v.id] : 0;
            out.binding = bindings.count(v.id) ? bindings[v.id] : 0;
            auto off = [&](uint32_t m, uint32_t def) {
                const auto it = member_offsets.find((uint64_t(block) << 32) | m);
                return m != UINT32_MAX && it != member_offsets.end() ? it->second : def;
            };
            out.off_pos     = off(idx[0], 0);
            out.off_size    = off(idx[1], 8);
            out.off_popout  = off(idx[2], 28);
            out.off_uvoff   = off(idx[3], 32);
            out.off_uvscale = off(idx[4], 40);
            out.off_tex     = off(idx[5], 48);
            out.off_gray    = off(idx[6], 56);
            break;
        }

    // Textures by name first ("Portrait" before the rest, so a name never matches twice).
    static const char *kNeedles[IMG_COUNT] = { "frame", "mask", "portrait", "background" };
    static const int kOrder[IMG_COUNT] = { IMG_PORTRAIT, IMG_BACKGROUND, IMG_FRAME, IMG_MASK };
    std::vector<bool> used(images.size(), false);
    int named = 0;
    for (int o = 0; o < IMG_COUNT; ++o)
    {
        const int slot = kOrder[o];
        for (size_t i = 0; i < images.size(); ++i)
            if (!used[i] && ContainsNoCase(images[i].name, kNeedles[slot]))
            {
                out.img[slot] = { true, images[i].set, images[i].binding };
                used[i] = true;
                ++named;
                break;
            }
    }
    out.img_by_name = named > 0;
    // Stripped names: exactly four textures map to the shader's sampler indices in binding order.
    if (named == 0 && images.size() == IMG_COUNT)
    {
        std::sort(images.begin(), images.end(), [](const ImgVar &a, const ImgVar &b) {
            return a.set != b.set ? a.set < b.set : a.binding < b.binding; });
        for (int k = 0; k < IMG_COUNT; ++k) out.img[k] = { true, images[k].set, images[k].binding };
    }
    return out;
}

// The vertex stage has the cbuffer, the pixel stage the textures; a pipeline needs both.
static void MergeCb(CbInfo &into, const CbInfo &from)
{
    if (!into.found && from.found)
    {
        ImgBinding keep[IMG_COUNT];
        memcpy(keep, into.img, sizeof(keep));
        const bool keep_named = into.img_by_name;
        into = from;
        for (int k = 0; k < IMG_COUNT; ++k) if (keep[k].found) into.img[k] = keep[k];
        into.img_by_name = into.img_by_name || keep_named;
        return;
    }
    for (int k = 0; k < IMG_COUNT; ++k) if (!into.img[k].found && from.img[k].found) into.img[k] = from.img[k];
    into.img_by_name = into.img_by_name || from.img_by_name;
}

// ---------------------------------------------------------------------------
// Texel decode. Format numbers are DXGI's (ReShade's api::format uses the same values).
// ---------------------------------------------------------------------------

static float HalfToFloat(uint16_t h)
{
    const uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    float v;
    if (exp == 0) v = std::ldexp(float(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else v = std::ldexp(float(man | 0x400u), int(exp) - 25);
    return sign ? -v : v;
}

// 0 = unsupported (compressed, depth, planar...).
static uint32_t BytesPerTexel(uint32_t fmt)
{
    switch (fmt)
    {
    case 27: case 28: case 29:      // R8G8B8A8 typeless/unorm/srgb
    case 87: case 90: case 91:      // B8G8R8A8 unorm/typeless/srgb
    case 23: case 24:               // R10G10B10A2 typeless/unorm
        return 4;
    case 9: case 10:                // R16G16B16A16 typeless/float
        return 8;
    case 2:                         // R32G32B32A32 float
        return 16;
    case 61: case 65:               // R8 unorm, A8 unorm
        return 1;
    default:
        return 0;
    }
}

// Returns false for unsupported formats. A8 decodes as (0,0,0,a); R8 as (r,r,r,r) so a
// single-channel mask reads as coverage either way.
static bool DecodeTexel(uint32_t fmt, const uint8_t *p, float rgba[4])
{
    switch (fmt)
    {
    case 27: case 28: case 29:
        for (int c = 0; c < 4; ++c) rgba[c] = p[c] / 255.0f;
        return true;
    case 87: case 90: case 91:
        rgba[0] = p[2] / 255.0f; rgba[1] = p[1] / 255.0f; rgba[2] = p[0] / 255.0f; rgba[3] = p[3] / 255.0f;
        return true;
    case 23: case 24:
    {
        uint32_t v; memcpy(&v, p, 4);
        rgba[0] = (v & 1023u) / 1023.0f; rgba[1] = ((v >> 10) & 1023u) / 1023.0f;
        rgba[2] = ((v >> 20) & 1023u) / 1023.0f; rgba[3] = (v >> 30) / 3.0f;
        return true;
    }
    case 9: case 10:
        for (int c = 0; c < 4; ++c) { uint16_t h; memcpy(&h, p + 2 * c, 2); rgba[c] = HalfToFloat(h); }
        return true;
    case 2:
        memcpy(rgba, p, 16);
        return true;
    case 61:
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = p[0] / 255.0f;
        return true;
    case 65:
        rgba[0] = rgba[1] = rgba[2] = 0.0f; rgba[3] = p[0] / 255.0f;
        return true;
    default:
        return false;
    }
}

// ---------------------------------------------------------------------------
// Alpha statistics
// ---------------------------------------------------------------------------

struct AlphaStats
{
    uint64_t n = 0, zero = 0, full = 0, partial = 0;   // a < 1/255, a > 254/255, between
    uint64_t premult_violations = 0;                    // max(rgb) > a + tolerance (not premultiplied)
    uint64_t ring_n = 0, ring_zero = 0;                 // outer 2-texel ring of the rectangle
    double   sum_a = 0.0;
    float    min_a = 1e30f, max_a = -1e30f;
};

// Pixel rectangle [x0,x1) x [y0,y1) of a tightly packed image with the given row pitch.
static AlphaStats Measure(const uint8_t *data, uint32_t row_pitch, uint32_t fmt, int x0, int y0, int x1, int y1)
{
    AlphaStats s;
    const uint32_t bpp = BytesPerTexel(fmt);
    if (data == nullptr || bpp == 0 || x1 <= x0 || y1 <= y0) return s;
    const float eps = (fmt == 9 || fmt == 10 || fmt == 2) ? 0.01f : 2.5f / 255.0f;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
        {
            float c[4];
            DecodeTexel(fmt, data + size_t(y) * row_pitch + size_t(x) * bpp, c);
            const float a = c[3];
            ++s.n;
            s.sum_a += a;
            s.min_a = (std::min)(s.min_a, a);
            s.max_a = (std::max)(s.max_a, a);
            if (a < 0.5f / 255.0f) ++s.zero;
            else if (a > 254.5f / 255.0f) ++s.full;
            else ++s.partial;
            if ((std::max)(c[0], (std::max)(c[1], c[2])) > a + eps) ++s.premult_violations;
            const bool ring = x < x0 + 2 || y < y0 + 2 || x >= x1 - 2 || y >= y1 - 2;
            if (ring) { ++s.ring_n; if (a < 0.5f / 255.0f) ++s.ring_zero; }
        }
    return s;
}

// gui_portrait.shader: PortraitUV = (UV1 - PortraitUVOffset) / PortraitUVScale for UV1 in
// [0,1]^2 over the widget. Returns the texel rectangle of a w x h image it samples
// (clamped to the image), or false when the transform is degenerate.
static bool UvTexelRect(float off_x, float off_y, float scale_x, float scale_y, uint32_t w, uint32_t h,
                        int &x0, int &y0, int &x1, int &y1)
{
    if (!(std::fabs(scale_x) > 1e-6f) || !(std::fabs(scale_y) > 1e-6f) || w == 0 || h == 0) return false;
    float u0 = (0.0f - off_x) / scale_x, u1 = (1.0f - off_x) / scale_x;
    float v0 = (0.0f - off_y) / scale_y, v1 = (1.0f - off_y) / scale_y;
    if (u0 > u1) std::swap(u0, u1);
    if (v0 > v1) std::swap(v0, v1);
    if (!(u1 > 0.0f && v1 > 0.0f && u0 < 1.0f && v0 < 1.0f)) return false;   // also rejects NaN
    u0 = (std::max)(u0, 0.0f); v0 = (std::max)(v0, 0.0f);
    u1 = (std::min)(u1, 1.0f); v1 = (std::min)(v1, 1.0f);
    x0 = int(std::floor(u0 * w)); y0 = int(std::floor(v0 * h));
    x1 = int(std::ceil(u1 * w));  y1 = int(std::ceil(v1 * h));
    x0 = (std::clamp)(x0, 0, int(w)); x1 = (std::clamp)(x1, 0, int(w));
    y0 = (std::clamp)(y0, 0, int(h)); y1 = (std::clamp)(y1, 0, int(h));
    return x1 > x0 && y1 > y0;
}

// ---------------------------------------------------------------------------
// Minimal PNG writer: 8-bit gray (channels=1) or RGBA (channels=4), stored deflate blocks.
// ---------------------------------------------------------------------------

static uint32_t Crc32(const uint8_t *p, size_t n, uint32_t crc = 0)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void PutBE(std::vector<uint8_t> &v, uint32_t x)
{
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}

static void PngChunk(std::vector<uint8_t> &out, const char type[4], const std::vector<uint8_t> &data)
{
    PutBE(out, static_cast<uint32_t>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    PutBE(out, Crc32(out.data() + start, out.size() - start));
}

static std::vector<uint8_t> EncodePng(uint32_t w, uint32_t h, const uint8_t *pixels, int channels)
{
    std::vector<uint8_t> out = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    std::vector<uint8_t> ihdr;
    PutBE(ihdr, w); PutBE(ihdr, h);
    ihdr.push_back(8); ihdr.push_back(channels == 4 ? 6 : 0); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
    PngChunk(out, "IHDR", ihdr);

    std::vector<uint8_t> raw;
    const size_t row = size_t(w) * channels;
    raw.reserve((row + 1) * h);
    for (uint32_t y = 0; y < h; ++y)
    {
        raw.push_back(0);   // filter: none
        raw.insert(raw.end(), pixels + y * row, pixels + (y + 1) * row);
    }
    std::vector<uint8_t> z = { 0x78, 0x01 };
    size_t pos = 0;
    do
    {
        const size_t len = (std::min)(raw.size() - pos, size_t(65535));
        z.push_back(pos + len == raw.size() ? 1 : 0);
        z.push_back(uint8_t(len)); z.push_back(uint8_t(len >> 8));
        z.push_back(uint8_t(~len)); z.push_back(uint8_t(~len >> 8));
        z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + len);
        pos += len;
    } while (pos < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) { a = (a + c) % 65521u; b = (b + a) % 65521u; }
    PutBE(z, (b << 16) | a);
    PngChunk(out, "IDAT", z);
    PngChunk(out, "IEND", {});
    return out;
}

static bool WritePng(const char *path, uint32_t w, uint32_t h, const uint8_t *pixels, int channels)
{
    const std::vector<uint8_t> png = EncodePng(w, h, pixels, channels);
    FILE *f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || f == nullptr) return false;
    const bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}

// Converts a w x h region of a supported format to RGBA8 (clamped) and to an alpha plane.
static bool ToRgba8(const uint8_t *data, uint32_t row_pitch, uint32_t fmt, uint32_t w, uint32_t h,
                    std::vector<uint8_t> &rgba, std::vector<uint8_t> &alpha)
{
    const uint32_t bpp = BytesPerTexel(fmt);
    if (bpp == 0 || data == nullptr) return false;
    rgba.resize(size_t(w) * h * 4);
    alpha.resize(size_t(w) * h);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            float c[4];
            DecodeTexel(fmt, data + size_t(y) * row_pitch + size_t(x) * bpp, c);
            for (int k = 0; k < 4; ++k)
            {
                const float v = c[k] != c[k] ? 0.0f : (std::clamp)(c[k], 0.0f, 1.0f);
                rgba[(size_t(y) * w + x) * 4 + k] = uint8_t(v * 255.0f + 0.5f);
            }
            alpha[size_t(y) * w + x] = rgba[(size_t(y) * w + x) * 4 + 3];
        }
    return true;
}
}  // namespace dprobe
