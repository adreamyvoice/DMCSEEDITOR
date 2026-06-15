// arc.h — MT Framework ".arc" archive (DMC4SE v7) reader/writer.
// Ported from the project's arc.py / inject.py. Layout:
//   header: "ARC\0" + u16 version + u16 count
//   TOC:    count x 80-byte entries { char name[64]; u32 extHash; u32 comp;
//                                     u32 decompField; u32 offset }
//           decompField low 29 bits = decompressed size, top 3 bits = flags.
//   data:   zlib-compressed blobs, starting at 0x8000, contiguous.
// Extract = zlib inflate an entry; repack = recompress changed entries, copy the
// rest verbatim, rebuild the TOC padded to 0x8000.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace arc {

using Bytes = std::vector<uint8_t>;

struct Entry {
    std::string name;        // internal path, '\\' separators (no extension)
    uint32_t    extHash = 0; // FNV-style extension hash
    uint32_t    comp = 0;    // compressed size in the source file
    uint32_t    decompField = 0; // raw field (flags|size)
    uint32_t    offset = 0;  // data offset in the source file
    // edit state:
    bool        replaced = false; // true if user swapped in new decompressed bytes
    Bytes       newData;          // replacement (decompressed) bytes when replaced

    uint32_t decompSize() const { return decompField & 0x1FFFFFFF; }
    std::string ext() const;     // resolved extension ("efl","tex","lmt",hash-hex…)
};

class ArcFile {
public:
    bool Load(const std::string& path, std::string& err);
    bool Save(const std::string& path, std::string& err) const; // rebuild + write
    bool loaded() const { return loaded_; }
    bool dirty()  const { return dirty_; }

    uint16_t version() const { return version_; }
    const std::string& path() const { return path_; }
    const std::vector<Entry>& entries() const { return entries_; }

    // Decompressed bytes of entry i (uses newData if replaced, unless original=true).
    bool Extract(size_t i, Bytes& out, std::string& err, bool original = false) const;
    // Replace entry i's contents with decompressed bytes (recompressed on Save).
    bool Replace(size_t i, const Bytes& decompressed, std::string& err);

private:
    std::string path_;
    Bytes       file_;         // original file bytes (for verbatim copy of untouched entries)
    std::vector<Entry> entries_;
    uint16_t    version_ = 0;
    uint32_t    dataStart_ = 0; // byte offset where compressed blobs begin
    bool        loaded_ = false;
    bool        dirty_  = false;
};

const char* ExtForHash(uint32_t h); // KNOWN table, or nullptr

} // namespace arc
