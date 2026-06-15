#include "effect_file.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>

namespace efx {

long EffectFile::Find(const std::string& needle, size_t from) const {
    if (needle.empty() || from >= buf_.size()) return -1;
    size_t n = buf_.size(), m = needle.size();
    for (size_t i = from; i + m <= n; ++i)
        if (memcmp(buf_.data() + i, needle.data(), m) == 0) return (long)i;
    return -1;
}

bool EffectFile::Load(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "Cannot open file: " + path; return false; }
    buf_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (buf_.empty()) { err = "File is empty: " + path; return false; }
    path_ = path;
    loaded_ = true;
    dirty_ = false;
    isEFL_ = buf_.size() >= 8 && memcmp(buf_.data(), "EFL\0", 4) == 0;
    version_ = isEFL_ ? *reinterpret_cast<const uint32_t*>(buf_.data() + 4) : 0;
    ScanPathRefs();
    ScanColorCodes();
    return true;
}

bool EffectFile::LoadFromMemory(const Bytes& data, const std::string& name, std::string& err) {
    if (data.empty()) { err = "empty buffer"; return false; }
    buf_ = data;
    path_ = name;
    loaded_ = true;
    dirty_ = false;
    isEFL_ = buf_.size() >= 8 && memcmp(buf_.data(), "EFL\0", 4) == 0;
    version_ = isEFL_ ? *reinterpret_cast<const uint32_t*>(buf_.data() + 4) : 0;
    ScanPathRefs();
    ScanColorCodes();
    return true;
}

bool EffectFile::Save(const std::string& path, bool keepBackup, std::string& err) const {
    if (keepBackup) {
        std::ifstream src(path, std::ios::binary);
        if (src) { std::ofstream bak(path + ".bak", std::ios::binary); bak << src.rdbuf(); }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) { err = "Cannot write file: " + path; return false; }
    out.write(reinterpret_cast<const char*>(buf_.data()), (std::streamsize)buf_.size());
    if (!out) { err = "Write failed: " + path; return false; }
    return true; // size preserved; source never deleted.
}

// ---- path-ref scanning ------------------------------------------------------

static bool printable(uint8_t c) { return c >= 0x20 && c < 0x7f; }

void EffectFile::ScanPathRefs() {
    refs_.clear();
    size_t i = 0, n = buf_.size();
    while (i < n) {
        // a path ref looks like "@effect\tex\..." or "effect\ean\..." (EFL) or
        // "model\game\pl030\..._BM" (MRL material texture refs) — start of an ASCII run.
        if (printable(buf_[i]) && (buf_[i] == '@' || buf_[i] == 'e' || buf_[i] == 'm')) {
            size_t j = i;
            while (j < n && printable(buf_[j])) ++j;
            size_t len = j - i;
            std::string s((const char*)&buf_[i], len);
            bool isPath = s.find("effect\\") != std::string::npos ||
                          s.find("\\com\\")  != std::string::npos ||
                          s.find("model\\")  != std::string::npos;   // MRL texture references
            if (isPath && len >= 6) {
                // slot = chars until the next non-zero byte (how far we can grow it)
                size_t k = j;
                while (k < n && buf_[k] == 0) ++k;
                PathRef r;
                r.offset = i; r.slot = k - i; r.value = s;
                r.kind = (s.find("\\tex\\") != std::string::npos || s.find("model\\") != std::string::npos
                          || s.rfind("_BM") != std::string::npos)
                            ? PathRef::Texture
                       : (s.find("\\ean\\") != std::string::npos) ? PathRef::Animation
                       : PathRef::Other;
                refs_.push_back(r);
                i = j;
                continue;
            }
            i = j;
            continue;
        }
        ++i;
    }
}

// ---- effect colour codes (infernalwarks "Effect Editing" method) -----------
// A real colour code is 4 bytes (R,G,B,A) sitting at (offset_of_"BM" - 64), DOUBLED:
// bytes[off..off+4] == bytes[off+4..off+8]. The doubling is the validation rule. The
// palette byte (00/01 RGB additive, 05/06 CMYK) sits 128 bytes above the same "BM".
void EffectFile::ScanColorCodes() {
    colors_.clear();
    size_t n = buf_.size();
    for (size_t i = 0; i + 1 < n; ++i) {
        if (buf_[i] != 'B' || buf_[i + 1] != 'M') continue;          // "BM" of a _BM texture name
        if (i < 64) continue;
        size_t off = i - 64;
        if (off + 8 > n) continue;
        // doubled-4-byte validation
        if (buf_[off] != buf_[off+4] || buf_[off+1] != buf_[off+5] ||
            buf_[off+2] != buf_[off+6] || buf_[off+3] != buf_[off+7]) continue;
        // skip all-zero (not a colour)
        if ((buf_[off] | buf_[off+1] | buf_[off+2]) == 0) continue;
        ColorCode c;
        c.offset = off;
        c.r = buf_[off]; c.g = buf_[off+1]; c.b = buf_[off+2]; c.a = buf_[off+3];
        c.palette = (i >= 128) ? buf_[i - 128] : 0;
        // label: the texture-name string this BM ends (walk back over printable chars)
        size_t s = i; while (s > 0 && buf_[s-1] >= 0x20 && buf_[s-1] < 0x7f) --s;
        size_t e = i + 2; while (e < n && buf_[e] >= 0x20 && buf_[e] < 0x7f) ++e;
        c.label.assign((const char*)&buf_[s], e - s);
        colors_.push_back(c);
    }
}

bool EffectFile::SetColorCode(size_t index, uint8_t r, uint8_t g, uint8_t b, std::string& err) {
    if (index >= colors_.size()) { err = "bad colour index"; return false; }
    ColorCode& c = colors_[index];
    if (c.offset + 8 > buf_.size()) { err = "colour offset past end"; return false; }
    // write both doubled copies (RGB only; preserve the alpha/intensity byte)
    buf_[c.offset+0] = buf_[c.offset+4] = r;
    buf_[c.offset+1] = buf_[c.offset+5] = g;
    buf_[c.offset+2] = buf_[c.offset+6] = b;
    c.r = r; c.g = g; c.b = b;
    dirty_ = true;
    return true;
}

bool EffectFile::SetPathRef(size_t index, const std::string& v, std::string& err) {
    if (index >= refs_.size()) { err = "bad ref index"; return false; }
    PathRef& r = refs_[index];
    if (v.size() + 1 > r.slot) {
        char m[96]; snprintf(m, sizeof m, "Too long: max %zu chars for this slot", r.slot - 1);
        err = m; return false;
    }
    for (size_t k = 0; k < r.slot; ++k)
        buf_[r.offset + k] = (k < v.size()) ? (uint8_t)v[k] : 0; // copy + zero-fill rest
    r.value = v;
    dirty_ = true;
    return true;
}

// ---- typed value access -----------------------------------------------------

bool EffectFile::ReadValue(size_t off, ValType t, std::string& out) const {
    char tmp[64];
    switch (t) {
        case ValType::F32: if (off+4 > buf_.size()) return false;
            { float v; memcpy(&v, &buf_[off], 4); snprintf(tmp, sizeof tmp, "%g", v); } break;
        case ValType::F64: if (off+8 > buf_.size()) return false;
            { double v; memcpy(&v, &buf_[off], 8); snprintf(tmp, sizeof tmp, "%g", v); } break;
        case ValType::I32: if (off+4 > buf_.size()) return false;
            { int32_t v; memcpy(&v, &buf_[off], 4); snprintf(tmp, sizeof tmp, "%d", v); } break;
        case ValType::U8:  if (off+1 > buf_.size()) return false;
            snprintf(tmp, sizeof tmp, "%u", buf_[off]); break;
    }
    out = tmp;
    return true;
}

bool EffectFile::WriteValue(size_t off, ValType t, const std::string& in, std::string& err) {
    switch (t) {
        case ValType::F32: if (off+4 > buf_.size()) { err="offset past end"; return false; }
            { float v = strtof(in.c_str(), nullptr); memcpy(&buf_[off], &v, 4); } break;
        case ValType::F64: if (off+8 > buf_.size()) { err="offset past end"; return false; }
            { double v = strtod(in.c_str(), nullptr); memcpy(&buf_[off], &v, 8); } break;
        case ValType::I32: if (off+4 > buf_.size()) { err="offset past end"; return false; }
            { int32_t v = (int32_t)strtol(in.c_str(), nullptr, 0); memcpy(&buf_[off], &v, 4); } break;
        case ValType::U8:  if (off+1 > buf_.size()) { err="offset past end"; return false; }
            { long v = strtol(in.c_str(), nullptr, 0); if (v<0||v>255){err="0-255 only";return false;}
              buf_[off] = (uint8_t)v; } break;
    }
    dirty_ = true;
    return true;
}

} // namespace efx
