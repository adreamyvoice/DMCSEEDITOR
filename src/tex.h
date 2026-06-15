// tex.h — MT Framework ".tex" texture: parse, decode (preview), recolor.
// Header (verified, DMC4SE): "TEX\0"; u32 versionFlags; u32 {mipCount:6,width:13,height:13};
//   u32 format (byte@0x0D: 0x13=BC1/DXT1 diffuse, 0x19=BC3, 0x1F=BC5); u32 mipOffset[mipCount].
// Recolor trick: BC1/BC3 store 2 RGB565 endpoints + 2-bit indices per 4x4 block. We apply the
// colour transform to the two ENDPOINT colours only and keep the indices -> detail preserved,
// no re-encoding. Targets the _BM (diffuse) BC1 textures.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace tex {

using Bytes = std::vector<uint8_t>;

struct TexInfo {
    bool     ok = false;
    int      width = 0, height = 0, mipCount = 0;
    int      format = 0;       // MTF enum (0x13 BC1, 0x19 BC3, 0x1F BC5)
    uint32_t mip0 = 0;         // byte offset of the base mip
    std::vector<uint32_t> mipOffsets;
    bool isBC1() const { return format == 0x13; }          // DXT1, 8-byte blocks
    bool isBC2() const { return format == 0x17; }          // DXT3 (effect _BM), 16-byte
    bool isBC3() const { return format == 0x19; }          // DXT5, 16-byte
    bool isBC5() const { return format == 0x1F; }          // normal map (RG) - bump, not colour
    bool isBC7() const { return format == 0x20; }          // DX10 BC7 (decode + re-encode mode 6)
    bool isRGBA32() const { return format == 0x21; }       // uncompressed B8G8R8A8 (DMC3 HD stages)
    bool has16ByteBlocks() const { return isBC2() || isBC3() || isBC5(); }
    bool recolorable() const { return isBC1() || isBC2() || isBC3() || isBC5() || isBC7() || isRGBA32(); }
};

TexInfo Parse(const Bytes& tex);

// Decode mip `mip` to RGBA8; returns its pixel size in outW/outH. Supports BC1 and BC3.
// Pick a small mip (e.g. ~256px) for a fast preview.
bool DecodeRGBA(const Bytes& tex, const TexInfo& ti, int mip,
                std::vector<uint8_t>& rgba, int& outW, int& outH);
// Index of the smallest mip with width >= minW (for previews).
int  PreviewMip(const TexInfo& ti, int minW = 256);

// Colour transform applied to each block endpoint.
struct Recolor {
    float hue = 0.0f;       // hue rotation in degrees (-180..180)
    float sat = 1.0f;       // saturation multiplier
    float val = 1.0f;       // value/brightness multiplier
    float tintR = 1, tintG = 1, tintB = 1; // multiply tint (1=none)
    // Colorize: replace the colour with `col*` (keeping luminance detail). For effect
    // textures / "just pick a colour" — set everything to one colour.
    bool  colorize = false;
    float colR = 1, colG = 0, colB = 0;    // target colour
    float colStrength = 1.0f;              // 0=original .. 1=fully target colour
};

// Apply the recolor to all blocks of all mips in place (BC1, and BC3 colour part).
void Apply(Bytes& tex, const TexInfo& ti, const Recolor& rc);

// --- DDS round-trip: edit a texture in any external tool ----------------------
// .tex already holds standard BCn blocks, so we can wrap them in a DDS header for
// export (Photoshop / GIMP / Blender all read DDS) and splice an edited DDS back in.
// ToDDS: wrap the BCn payload of `tex` in a DDS file -> `out`. False on unsupported fmt.
bool ToDDS(const Bytes& tex, const TexInfo& ti, Bytes& out);
// FromDDS: build a new .tex from an edited `dds`, using `origTex`/`ti` as the template.
// The DDS must keep the SAME BCn format and base width/height; mip count may differ.
// Returns false with `err` set if the DDS is incompatible.
bool FromDDS(const Bytes& dds, const Bytes& origTex, const TexInfo& ti,
             Bytes& out, std::string& err);

// Parse a raw DDS (BCn) at byte `base` in buffer `b` into a TexInfo whose mipOffsets
// point into `b` — so DecodeRGBA()/Apply() work on a DDS exactly like on a .tex.
// (DMC3 HD .pac textures are embedded DDS.) ok=false if not a recognised BCn DDS.
TexInfo ParseDDS(const Bytes& b, size_t base);

// Re-encode an edited full-resolution RGBA8 image (ti.width x ti.height) back into `b`
// in place, regenerating every mip by 2x2 box downsample. Used by the click-to-paint
// brush: paint into a decoded RGBA buffer, then bake it back. The encoded size is
// identical to the original (deterministic per format) so it splices into the .pac.
// Supports BC1/BC2/BC3/BC7 and uncompressed RGBA; false (with err) for BC5/unsupported.
bool EncodeFromRGBA(Bytes& b, const TexInfo& ti, const std::vector<uint8_t>& rgba, std::string& err);

// Recolor the RGBA float colour tuples inside a DMC4 .efl effect file in place.
// Scans for 4-aligned (r,g,b,a) runs with all components in [0,1] and recolors RGB
// (keeping alpha). Returns how many colours were changed.
int RecolorEfl(Bytes& efl, const Recolor& rc);

} // namespace tex
