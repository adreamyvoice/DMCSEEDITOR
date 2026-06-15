#include "arc.h"
#include "miniz.h"
#include <cstdio>
#include <cstring>
#include <fstream>

namespace arc {

static const int ENTRY = 80;

struct Known { uint32_t h; const char* ext; };
static const Known kKnown[] = {
    {0x241F5DEB,"tex"}, {0x2749C8A8,"mrl"}, {0x10C460E6,"msg"}, {0x232E228C,"rtex"},
    {0x046D7AAC,"sdl"}, {0x4C0DB839,"sdl"},  // scene / stage definition (DMC4SE build)
    {0x2CE309AB,"gui"}, {0x07F768AF,"gii"}, {0x12191BA1,"efl"},
    {0x6D5AE854,"efl"},  // observed in player arcs (effect list)
    {0x76820D81,"lmt"},  // motion list (best-known)
    {0x58A15856,"mod"},  // model / skeleton
};
const char* ExtForHash(uint32_t h) {
    for (auto& k : kKnown) if (k.h == h) return k.ext;
    return nullptr;
}
std::string Entry::ext() const {
    if (const char* e = ExtForHash(extHash)) return e;
    char b[16]; snprintf(b, sizeof b, "%08X", extHash); return b;
}

bool ArcFile::Load(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "Cannot open: " + path; return false; }
    file_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (file_.size() < 8 || memcmp(file_.data(), "ARC\0", 4) != 0) {
        err = "Not an ARC archive (missing 'ARC\\0' magic)"; return false;
    }
    uint16_t count;
    memcpy(&version_, &file_[4], 2);
    memcpy(&count,    &file_[6], 2);
    entries_.clear();
    size_t off = 8;
    for (uint16_t i = 0; i < count; ++i) {
        if (off + ENTRY > file_.size()) { err = "Truncated TOC"; return false; }
        Entry e;
        char nm[65]; memcpy(nm, &file_[off], 64); nm[64] = 0;
        e.name = nm;
        memcpy(&e.extHash,     &file_[off + 64], 4);
        memcpy(&e.comp,        &file_[off + 68], 4);
        memcpy(&e.decompField, &file_[off + 72], 4);
        memcpy(&e.offset,      &file_[off + 76], 4);
        entries_.push_back(std::move(e));
        off += ENTRY;
    }
    // Data start = lowest entry offset in the original file (TOC is fixed-size since
    // the entry count never changes, so reusing it keeps byte-exact alignment).
    dataStart_ = file_.size();
    for (const Entry& e : entries_) if (e.offset && e.offset < dataStart_) dataStart_ = e.offset;
    if (entries_.empty() || dataStart_ < 8 + entries_.size() * ENTRY) dataStart_ = (uint32_t)(8 + entries_.size() * ENTRY);
    path_ = path;
    loaded_ = true;
    dirty_ = false;
    return true;
}

bool ArcFile::Extract(size_t i, Bytes& out, std::string& err, bool original) const {
    if (i >= entries_.size()) { err = "bad index"; return false; }
    const Entry& e = entries_[i];
    if (e.replaced && !original) { out = e.newData; return true; }
    if ((size_t)e.offset + e.comp > file_.size()) { err = "entry data past EOF"; return false; }
    mz_ulong dsize = e.decompSize();
    out.resize(dsize);
    int r = mz_uncompress(out.data(), &dsize, &file_[e.offset], e.comp);
    if (r != MZ_OK) {
        // some entries are stored raw (comp == decomp) — copy through
        if (e.comp == e.decompSize()) { out.assign(&file_[e.offset], &file_[e.offset] + e.comp); return true; }
        char m[64]; snprintf(m, sizeof m, "inflate failed (mz=%d)", r); err = m; return false;
    }
    out.resize(dsize);
    return true;
}

bool ArcFile::Replace(size_t i, const Bytes& decompressed, std::string& err) {
    if (i >= entries_.size()) { err = "bad index"; return false; }
    entries_[i].replaced = true;
    entries_[i].newData = decompressed;
    dirty_ = true;
    return true;
}

bool ArcFile::Save(const std::string& path, std::string& err) const {
    if (8 + entries_.size() * ENTRY > dataStart_) { err = "TOC larger than data-start region"; return false; }
    // Build data blob + new TOC offsets/sizes.
    Bytes blob;
    struct Out { uint32_t comp, decompField, offset; };
    std::vector<Out> outs(entries_.size());
    uint32_t cur = dataStart_;
    for (size_t i = 0; i < entries_.size(); ++i) {
        const Entry& e = entries_[i];
        if (e.replaced) {
            mz_ulong bound = mz_compressBound((mz_ulong)e.newData.size());
            Bytes c(bound);
            mz_ulong clen = bound;
            int r = mz_compress2(c.data(), &clen, e.newData.data(), (mz_ulong)e.newData.size(), 9);
            if (r != MZ_OK) { char m[48]; snprintf(m, sizeof m, "deflate failed (mz=%d)", r); err = m; return false; }
            outs[i].comp = (uint32_t)clen;
            outs[i].decompField = (e.decompField & 0xE0000000) | ((uint32_t)e.newData.size() & 0x1FFFFFFF);
            outs[i].offset = cur;
            blob.insert(blob.end(), c.begin(), c.begin() + clen);
            cur += (uint32_t)clen;
        } else {
            outs[i].comp = e.comp;
            outs[i].decompField = e.decompField;
            outs[i].offset = cur;
            blob.insert(blob.end(), &file_[e.offset], &file_[e.offset] + e.comp);
            cur += e.comp;
        }
    }
    // Assemble: header + TOC + pad to DATA_START + blob.
    Bytes buf;
    buf.resize(8);
    memcpy(&buf[0], "ARC\0", 4);
    uint16_t count = (uint16_t)entries_.size();
    memcpy(&buf[4], &version_, 2);
    memcpy(&buf[6], &count, 2);
    for (size_t i = 0; i < entries_.size(); ++i) {
        uint8_t row[ENTRY]; memset(row, 0, ENTRY);
        std::string nm = entries_[i].name;
        if (nm.size() > 63) nm.resize(63);
        memcpy(row, nm.data(), nm.size());
        memcpy(row + 64, &entries_[i].extHash, 4);
        memcpy(row + 68, &outs[i].comp, 4);
        memcpy(row + 72, &outs[i].decompField, 4);
        memcpy(row + 76, &outs[i].offset, 4);
        buf.insert(buf.end(), row, row + ENTRY);
    }
    if (buf.size() < dataStart_) buf.resize(dataStart_, 0);
    buf.insert(buf.end(), blob.begin(), blob.end());

    // one-time backup of the ORIGINAL archive (only if no .bak exists yet) so edits are
    // always recoverable — the first .bak captures the pristine file.
    {
        std::string bak = path + ".bak";
        std::ifstream test(bak, std::ios::binary);
        if (!test.good()) {
            std::ifstream src(path, std::ios::binary);
            if (src.good()) { std::ofstream dst(bak, std::ios::binary); dst << src.rdbuf(); }
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { err = "Cannot write: " + path; return false; }
    out.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
    if (!out) { err = "Write failed: " + path; return false; }
    return true;
}

} // namespace arc
