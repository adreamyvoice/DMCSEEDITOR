#include "pac.h"
#include <cstring>
#include <fstream>
#include <algorithm>

namespace pac {

static uint32_t u32(const Bytes& b, size_t o) {
    uint32_t v = 0; if (o + 4 <= b.size()) memcpy(&v, &b[o], 4); return v;
}
static bool tag4(const Bytes& b, size_t o, const char* t) {
    return o + 4 <= b.size() && memcmp(&b[o], t, 4) == 0;
}

bool IsContainer(const Bytes& d) {
    return d.size() >= 8 && (memcmp(d.data(), "PAC\0", 4) == 0 || memcmp(d.data(), "PNST", 4) == 0);
}

// Classify a sub-file by its leading bytes. Fills tag/type/childCount.
static void Classify(const Bytes& f, uint32_t off, uint32_t size, Entry& e) {
    auto at = [&](size_t k) -> uint8_t { return (off + k < f.size()) ? f[off + k] : 0; };
    char m[5] = {0}; for (int i = 0; i < 4; ++i) m[i] = (char)at(i);

    auto isMagic = [&](const char* t) { return memcmp(m, t, 4) == 0; };
    // size-prefixed motion: u32 size, then "MOT\0"
    bool motSized = (at(4)=='M' && at(5)=='O' && at(6)=='T' && at(7)==0);

    if (isMagic("MOD ")) { e.tag = "MOD";  e.type = "model"; }
    else if (isMagic("MOT") || motSized) { e.tag = "MOT"; e.type = "motion"; }
    else if (isMagic("SCM ")) { e.tag = "SCM"; e.type = "script"; }
    else if (isMagic("HITS")) { e.tag = "HITS"; e.type = "collision"; }
    else if (isMagic("EFM ")) { e.tag = "EFM"; e.type = "effect"; }
    else if (isMagic("SHW ")) { e.tag = "SHW"; e.type = "shadow"; }
    else if (isMagic("PNST")) { e.tag = "PNST"; e.type = "geometry pack"; e.childCount = (int)u32(f, off + 4); }
    else if (isMagic("PAC\0")) { e.tag = "PAC"; e.type = "container"; e.childCount = (int)u32(f, off + 4); }
    else {
        // printable text? (config / name lists)
        bool printable = true;
        for (int i = 0; i < 4; ++i) { uint8_t c = at(i); if (!(c == 9 || c == 10 || c == 13 || (c >= 32 && c < 127))) { printable = false; break; } }
        if (printable) {
            e.tag = "txt"; e.type = "text";
            std::string s; for (int i = 0; i < 24 && at(i) >= 32 && at(i) < 127; ++i) s += (char)at(i);
            if (!s.empty()) e.type = "text \"" + s + "\"";
        } else {
            uint32_t t0 = u32(f, off);              // many resource blobs lead with a small type-id u32
            if (t0 > 0 && t0 < 0x100) { e.tag = "res"; e.type = "texture/resource (type " + std::to_string(t0) + ")"; }
            else {
                char hx[12]; snprintf(hx, sizeof hx, "%02x%02x%02x%02x", at(0), at(1), at(2), at(3));
                e.tag = hx; e.type = "data";
            }
        }
    }
}

bool PacFile::Load(const std::string& path, std::string& err) {
    loaded_ = false; entries_.clear(); magic_.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    file_.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (!IsContainer(file_)) { err = "not a PAC/PNST container"; return false; }
    magic_ = (memcmp(file_.data(), "PNST", 4) == 0) ? "PNST" : "PAC";

    uint32_t count = u32(file_, 4);
    if (count == 0 || count > 100000) { err = "implausible entry count"; return false; }
    if (8 + (size_t)count * 4 > file_.size()) { err = "offset table past end of file"; return false; }

    // gather non-zero offsets (with their slot) and a sorted copy to compute sizes
    std::vector<std::pair<uint32_t,int>> live;     // (offset, slot)
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t o = u32(file_, 8 + (size_t)i * 4);
        if (o != 0 && o < file_.size()) live.push_back({o, (int)i});
    }
    std::vector<uint32_t> sorted; sorted.reserve(live.size());
    for (auto& p : live) sorted.push_back(p.first);
    std::sort(sorted.begin(), sorted.end());

    for (auto& p : live) {
        uint32_t off = p.first;
        // size = next larger offset (in file order) - off, else to EOF
        auto it = std::upper_bound(sorted.begin(), sorted.end(), off);
        uint32_t end = (it != sorted.end()) ? *it : (uint32_t)file_.size();
        Entry e; e.slot = p.second; e.offset = off; e.size = end - off;
        Classify(file_, off, e.size, e);
        char d[96];
        if (e.childCount > 0) snprintf(d, sizeof d, "%s - %d items", e.type.c_str(), e.childCount);
        else snprintf(d, sizeof d, "%s", e.type.c_str());
        e.desc = d;
        entries_.push_back(e);
    }
    // present in slot order for a stable, readable list
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b){ return a.slot < b.slot; });

    // Scan for embedded DDS textures (DMC3 HD textures are DDS/BCn) so they can be
    // previewed/recoloured directly, no extraction needed.
    texs_.clear();
    for (size_t o = 0; o + 128 <= file_.size(); ) {
        if (memcmp(&file_[o], "DDS ", 4) != 0) { ++o; continue; }
        auto R = [&](size_t k) { uint32_t v = 0; if (o+k+4<=file_.size()) memcpy(&v, &file_[o + k], 4); return v; };
        int h = (int)R(12), w = (int)R(16), mips = (int)R(28);
        char fcc[5] = {0}; memcpy(fcc, &file_[o + 84], 4);
        uint32_t pfFlags = R(80);
        // Detect format. DX10-header textures (BC7 / typed RGBA) carry their format in the DXGI enum.
        int fmt = 0; uint32_t header = 128; bool compressed = true; const char* fn = "?";
        if (!memcmp(fcc, "DX10", 4) && o + 148 <= file_.size()) {
            uint32_t dxgi = R(128); header = 148;
            if      (dxgi==70||dxgi==71||dxgi==72) { fmt=0x13; fn="BC1"; }
            else if (dxgi==73||dxgi==74||dxgi==75) { fmt=0x17; fn="BC2"; }
            else if (dxgi==76||dxgi==77||dxgi==78) { fmt=0x19; fn="BC3"; }
            else if (dxgi==82||dxgi==83)           { fmt=0x1F; fn="BC5"; }
            else if (dxgi==97||dxgi==98||dxgi==99) { fmt=0x20; fn="BC7"; }
            else if (dxgi==28||dxgi==29||dxgi==87||dxgi==88) { fmt=0x21; fn="RGBA"; compressed=false; }
        } else if (!memcmp(fcc,"DXT1",4)) { fmt=0x13; fn="BC1"; }
        else if (!memcmp(fcc,"DXT3",4)) { fmt=0x17; fn="BC2"; }
        else if (!memcmp(fcc,"DXT5",4)) { fmt=0x19; fn="BC3"; }
        else if (!memcmp(fcc,"ATI2",4)||!memcmp(fcc,"BC5U",4)) { fmt=0x1F; fn="BC5"; }
        else if ((pfFlags & 0x40) && R(88)==32) { fmt=0x21; fn="RGBA"; compressed=false; }
        if (!fmt || w <= 0 || h <= 0 || w > 8192 || h > 8192) { ++o; continue; }
        if (mips <= 0) mips = 1;
        uint32_t bb = (fmt == 0x13) ? 8u : 16u, total = header;
        for (int m = 0; m < mips; ++m) { int mw=w>>m, mh=h>>m; if(mw<1)mw=1; if(mh<1)mh=1;
            total += compressed ? ((mw+3)/4)*((mh+3)/4)*bb : (uint32_t)mw*mh*4; }
        if (o + total > file_.size()) { ++o; continue; }            // sanity: fits in file
        char nm[64]; snprintf(nm, sizeof nm, "tex%zu (%dx%d %s)", texs_.size(), w, h, fn);
        TexRef t; t.name = nm; t.off = (uint32_t)o; t.size = total; t.w=w; t.h=h; t.mips=mips; t.fmt=fmt;
        texs_.push_back(t);
        o += total;                                                 // skip past this texture
    }

    path_ = path; loaded_ = true; dirty_ = false;
    return true;
}

bool PacFile::Splice(uint32_t off, const Bytes& bytes, std::string& err) {
    if ((size_t)off + bytes.size() > file_.size()) { err = "splice out of range"; return false; }
    memcpy(&file_[off], bytes.data(), bytes.size());
    dirty_ = true;
    return true;
}

bool PacFile::SaveInPlace(std::string& err) {
    if (!loaded_) { err = "no file loaded"; return false; }
    std::string bak = path_ + ".bak";
    std::ifstream chk(bak, std::ios::binary);
    if (!chk.good()) {                                              // one-time backup of the original
        std::ifstream src(path_, std::ios::binary);
        std::ofstream dst(bak, std::ios::binary);
        if (src.good() && dst.good()) dst << src.rdbuf();
    }
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot open file for writing"; return false; }
    out.write((const char*)file_.data(), (std::streamsize)file_.size());
    if (!out) { err = "write failed"; return false; }
    dirty_ = false;
    return true;
}

bool PacFile::Extract(size_t i, Bytes& out, std::string& err) const {
    if (i >= entries_.size()) { err = "bad index"; return false; }
    const Entry& e = entries_[i];
    if ((size_t)e.offset + e.size > file_.size()) { err = "entry out of range"; return false; }
    out.assign(file_.begin() + e.offset, file_.begin() + e.offset + e.size);
    return true;
}

} // namespace pac
