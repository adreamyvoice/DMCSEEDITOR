// pac.h — DMC3 (HD Collection) ".pac" / "PNST" container reader.
// Both share one layout: magic("PAC\0" or "PNST") + u32 count + count×u32 offset
// table (0 = empty slot). A sub-file's size = (next non-zero offset, or EOF) - offset.
// There are NO stored filenames, so entries are named by their sub-magic + slot index
// (mirroring how the .arc browser gives entries friendly types).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace pac {

using Bytes = std::vector<uint8_t>;

struct Entry {
    int         slot = 0;        // index in the offset table (stable id)
    uint32_t    offset = 0;      // byte offset in the file
    uint32_t    size = 0;        // computed sub-file size
    std::string tag;             // raw 4-char sub-magic ("MOD ","SCM ",…) or hex
    std::string type;            // friendly type ("model","motion","script",…)
    std::string desc;            // friendly description (type + any detail)
    int         childCount = 0;  // if this sub-file is itself a PAC/PNST container
};

// An embedded DDS texture found anywhere inside the container (DMC3 HD textures).
struct TexRef {
    std::string name;            // "tex0 (256x256 DXT5)"
    uint32_t    off = 0;         // absolute byte offset of the "DDS " header in the file
    uint32_t    size = 0;        // exact DDS size (header + all mips)
    int         w = 0, h = 0, mips = 0, fmt = 0;
};

class PacFile {
public:
    bool Load(const std::string& path, std::string& err);
    bool loaded() const { return loaded_; }
    const std::string& path()  const { return path_; }
    const std::string& magic() const { return magic_; }    // "PAC" or "PNST"
    const std::vector<Entry>& entries() const { return entries_; }
    // Raw bytes of entry i (a copy of the sub-file slice).
    bool Extract(size_t i, Bytes& out, std::string& err) const;

    // Embedded DDS textures (DMC3 HD) and direct access for in-place recolor.
    const std::vector<TexRef>& textures() const { return texs_; }
    const Bytes& data() const { return file_; }
    bool dirty() const { return dirty_; }
    // Overwrite `bytes.size()` bytes at `off` in the in-memory file (same-size edits).
    bool Splice(uint32_t off, const Bytes& bytes, std::string& err);
    // Write the (edited) container back to disk; makes a one-time .bak on first save.
    bool SaveInPlace(std::string& err);

private:
    std::string path_, magic_;
    Bytes       file_;
    std::vector<Entry>  entries_;
    std::vector<TexRef> texs_;
    bool        loaded_ = false;
    bool        dirty_  = false;
};

// True if the buffer starts with a PAC/PNST container magic.
bool IsContainer(const Bytes& d);

} // namespace pac
