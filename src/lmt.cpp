#include "lmt.h"
#include <cstring>

namespace lmt {

static uint32_t rd32(const Bytes& b, size_t o) {
    uint32_t v = 0; if (o + 4 <= b.size()) memcpy(&v, &b[o], 4); return v;
}

bool LmtFile::Parse(const Bytes& data, std::string& err, bool fresh) {
    valid_ = false; motions_.clear();
    if (data.size() < 8 || memcmp(data.data(), "LMT\0", 4) != 0) {
        err = "Not an LMT file (missing 'LMT\\0' magic)"; return false;
    }
    uint16_t ver, count;
    memcpy(&ver,   &data[4], 2);
    memcpy(&count, &data[6], 2);
    version_ = ver;
    size_t table = 8;
    if (fresh) {
        origOffsets_.clear();
        for (uint16_t i = 0; i < count; ++i) origOffsets_.push_back(rd32(data, table + (size_t)i * 4));
    }
    for (uint16_t i = 0; i < count; ++i) {
        Motion m; m.index = i;
        m.offset = rd32(data, table + (size_t)i * 4);
        if (m.offset == 0 || m.offset + 16 > data.size()) { m.empty = true; }
        else {
            m.empty = false;
            m.tracksPtr = rd32(data, m.offset + 0x00);
            m.numTracks = rd32(data, m.offset + 0x04);
            m.numFrames = rd32(data, m.offset + 0x08);
            m.loopFrame = rd32(data, m.offset + 0x0C);
        }
        motions_.push_back(m);
    }
    valid_ = true;
    return true;
}

int LmtFile::liveCount() const {
    int n = 0; for (auto& m : motions_) if (!m.empty) ++n; return n;
}

int LmtFile::playsSlot(int slot) const {
    if (slot < 0 || slot >= (int)motions_.size()) return -1;
    uint32_t cur = motions_[slot].offset;
    for (size_t i = 0; i < origOffsets_.size(); ++i) if (origOffsets_[i] == cur) return (int)i;
    return -1;
}

bool LmtFile::RemapSlot(Bytes& out, int slot, int srcSlot, std::string& err) const {
    int n = (int)origOffsets_.size();
    if (slot < 0 || slot >= n || srcSlot < 0 || srcSlot >= n) { err = "slot out of range"; return false; }
    size_t pos = 8 + (size_t)slot * 4;
    if (pos + 4 > out.size()) { err = "table past end"; return false; }
    uint32_t v = origOffsets_[srcSlot];
    memcpy(&out[pos], &v, 4);
    return true;
}

bool LmtFile::SwapSlots(Bytes& out, int a, int b, std::string& err) const {
    int n = (int)origOffsets_.size();
    if (a < 0 || a >= n || b < 0 || b >= n) { err = "slot out of range"; return false; }
    size_t pa = 8 + (size_t)a * 4, pb = 8 + (size_t)b * 4;
    if (pa + 4 > out.size() || pb + 4 > out.size()) { err = "table past end"; return false; }
    uint32_t va, vb; memcpy(&va, &out[pa], 4); memcpy(&vb, &out[pb], 4);
    memcpy(&out[pa], &vb, 4); memcpy(&out[pb], &va, 4);
    return true;
}

bool LmtFile::SetField(Bytes& out, int motionIndex, Field f, uint32_t value, std::string& err) const {
    if (motionIndex < 0 || motionIndex >= (int)motions_.size()) { err = "bad motion index"; return false; }
    const Motion& m = motions_[motionIndex];
    if (m.empty) { err = "empty motion slot"; return false; }
    size_t fo = m.offset + (f == NumFrames ? 0x08 : 0x0C);
    if (fo + 4 > out.size()) { err = "field past end"; return false; }
    memcpy(&out[fo], &value, 4);
    return true;
}

} // namespace lmt
