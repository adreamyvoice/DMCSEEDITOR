// effect_file.h — model for DMC4 ".efl" (Effect List) files, the real on-disk
// format edited by effect mods. Verified against real samples unpacked from
// uPlayerVergil.arc (see samples/). An EFL is:
//   magic "EFL\0" + version + a block/offset table + binary parameter blocks +
//   embedded path-reference strings ("@effect\tex\com\<name>_BM" textures and
//   "effect\ean\com\<name>" animations) sitting in zero-padded null-terminated slots.
//
// PROVEN editable here: the texture/animation path references (swap what texture an
// effect uses) — they live in fixed zero-padded slots, safe to overwrite in place.
// Numeric params (colors/coords/sizes) are IEEE floats in the binary blocks; exact
// semantic offsets still need mapping, so those are edited via the Manual/Hex tools.
//
// NOTE: this supersedes the earlier marker-based model. The "nativePC/positionColor/
// WENJIANJIESHU" markers the disasm surfaced were tool-internal strings — they do NOT
// appear in real EFL data (verified: 0 hits across 583 unpacked files).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace efx {

using Bytes = std::vector<uint8_t>;

// A path-reference string embedded in the EFL (editable in place).
struct PathRef {
    size_t      offset;    // byte offset of the first char in the file
    size_t      slot;      // bytes available before the next non-zero data (max length)
    std::string value;     // current text (e.g. "@effect\\tex\\com\\ec032_12_BM")
    enum Kind { Texture, Animation, Other } kind;
};

// A per-texture emitter COLOUR CODE (the community effect-editing method, infernalwarks
// "Effect Editing" tutorial): 4 bytes R,G,B,A stored DOUBLED at (offset_of_"BM" - 64),
// just before a "..._BM" texture-name string. The doubled copy is the validation. This is
// the clean way to recolour an effect (vs blind float scanning). Verified exact on real EFL.
struct ColorCode {
    size_t      offset;    // start of the first 4-byte copy (second copy is offset+4)
    uint8_t     r, g, b, a;
    uint8_t     palette;   // byte 128 above BM (00/01 = RGB additive, 05/06 = CMYK)
    std::string label;     // the texture name this colour drives
};

enum class ValType { F32, F64, U8, I32 };

class EffectFile {
public:
    bool Load(const std::string& path, std::string& err);
    // Load from an in-memory buffer (e.g. an entry extracted from a .arc). `name` is a
    // display label; there is no backing path, so save goes back through the archive.
    bool LoadFromMemory(const Bytes& data, const std::string& name, std::string& err);
    bool Save(const std::string& path, bool keepBackup, std::string& err) const;
    bool loaded() const { return loaded_; }
    bool dirty()  const { return dirty_; }

    const std::string& path() const { return path_; }
    const Bytes& raw() const { return buf_; }
    size_t size() const { return buf_.size(); }

    bool   isEFL() const { return isEFL_; }
    uint32_t version() const { return version_; }

    // The embedded path references, scanned on load.
    const std::vector<PathRef>& pathRefs() const { return refs_; }

    // Overwrite a path ref in place (null-terminated, bounded by its slot).
    bool SetPathRef(size_t index, const std::string& v, std::string& err);

    // Per-texture emitter colour codes (BM-anchored), scanned on load.
    const std::vector<ColorCode>& colorCodes() const { return colors_; }
    // Set R,G,B of a colour code (writes BOTH doubled copies; alpha/intensity preserved).
    bool SetColorCode(size_t index, uint8_t r, uint8_t g, uint8_t b, std::string& err);

    // Generic typed value access at an absolute offset (for the Manual editor / pinning
    // colors and coordinates with help from the Hex Inspector).
    bool ReadValue (size_t off, ValType t, std::string& out) const;
    bool WriteValue(size_t off, ValType t, const std::string& in, std::string& err);

    long Find(const std::string& needle, size_t from = 0) const;

private:
    void ScanPathRefs();
    void ScanColorCodes();

    std::string path_;
    Bytes       buf_;
    bool        loaded_  = false;
    bool        dirty_   = false;
    bool        isEFL_   = false;
    uint32_t    version_ = 0;
    std::vector<PathRef>  refs_;
    std::vector<ColorCode> colors_;
};

} // namespace efx
