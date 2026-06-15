#include "savedit.h"
#include <cstring>
#include <fstream>
#include <algorithm>

namespace sav {

// ---- DMC1 (HD Collection) — verified against the real dmc1.sav + the open-source
// dmc1 editor (joaovitorbf/dmcsaveeditor): 10 slots of 2416 bytes, header 01 00 00,
// NO checksum. Offsets below are relative to the slot base. ------------------------
static const Field kDMC1[] = {
    {"Red Orbs",       0x634, 4, K_UINT,   9999999, "+1588: main currency"},
    {"Yellow Orbs",    0x620, 2, K_UINT,   999,     "+1568"},
    {"Blue Orbs",      0x638, 1, K_UINT,   255,     "+1592: vitality upgrades"},
    {"Vitality",       0x624, 1, K_UINT,   255,     "+1572: current max health units"},
    {"Devil Trigger",  0x625, 1, K_UINT,   255,     "+1573: DT gauge units"},
    {"Current Mission",0x24,  1, K_UINT,   22,      "+36"},
    {"Difficulty",     0x26,  1, K_DIFF,   4,        "+38: 0=Easy 1=Normal 2=Hard 3=DMD 4=Heaven/Hell"},
    {"Times Beaten",   0x22,  2, K_UINT,   9999,    "+34"},
    {"Save Count",     0x20,  2, K_UINT,   65535,   "+32"},
    {"Playtime (sec)", 0x2C,  4, K_TIME_60,0,       "+44: stored as seconds*60"},
};

static std::string lower(std::string s){ for(char&c:s)c=(char)tolower((unsigned char)c); return s; }

void SaveFile::detect() {
    game_ = GAME_UNKNOWN; slotCount_ = 1; slotSize_ = 0; fields_.clear();
    std::string base = path_;
    size_t sl = base.find_last_of("/\\"); if (sl!=std::string::npos) base = base.substr(sl+1);
    std::string nm = lower(base);

    if (data_.size() >= 4 && memcmp(data_.data(), "DSSS", 4) == 0) {       // DMC5
        game_ = GAME_DMC5; slotCount_ = 1; return;                          // single big save, no fields mapped yet
    }
    if (nm.find("dmc1")!=std::string::npos ||
        (data_.size()==24420 && data_.size()>=3 && data_[0]==1 && data_[1]==0 && data_[2]==0)) {
        game_ = GAME_DMC1; slotSize_ = 2416; slotCount_ = 10;
        fields_.assign(kDMC1, kDMC1 + sizeof(kDMC1)/sizeof(kDMC1[0]));
        return;
    }
    if (nm.find("dmc2")!=std::string::npos) { game_ = GAME_DMC2; slotCount_ = 1; return; }
    if (nm.find("dmc3")!=std::string::npos) { game_ = GAME_DMC3; slotCount_ = 1; return; }
    if (nm.find("dmc4")!=std::string::npos || nm.find("specialedition")!=std::string::npos ||
        nm.find("savedata")!=std::string::npos) { game_ = GAME_DMC4SE; slotCount_ = 1; return; }
}

bool SaveFile::Load(const std::string& path, std::string& err) {
    loaded_ = false; dirty_ = false; data_.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open file"; return false; }
    data_.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data_.empty()) { err = "empty file"; return false; }
    path_ = path; detect();
    loaded_ = true;
    return true;
}

const char* SaveFile::gameName() const {
    switch (game_) {
        case GAME_DMC1: return "Devil May Cry 1 (HD)";
        case GAME_DMC2: return "Devil May Cry 2 (HD)";
        case GAME_DMC3: return "Devil May Cry 3 (HD)";
        case GAME_DMC4SE: return "Devil May Cry 4 SE";
        case GAME_DMC5: return "Devil May Cry 5";
        default: return "Unknown save";
    }
}

bool SaveFile::slotUsed(int slot) const {
    if (game_ != GAME_DMC1) return true;
    size_t b = (size_t)slot * slotSize_;
    if (b + 3 > data_.size()) return false;
    return data_[b]==1 && data_[b+1]==0 && data_[b+2]==0;     // header signature
}

uint32_t SaveFile::ReadAt(size_t off, int width) const {
    uint32_t v = 0;
    for (int i = 0; i < width && off+i < data_.size(); ++i) v |= (uint32_t)data_[off+i] << (8*i);
    return v;
}
bool SaveFile::WriteAt(size_t off, int width, uint32_t v) {
    if (off + width > data_.size()) return false;
    for (int i = 0; i < width; ++i) data_[off+i] = (uint8_t)((v >> (8*i)) & 0xFF);
    dirty_ = true; return true;
}

uint32_t SaveFile::ReadField(const Field& f, int slot) const {
    size_t base = slotted() ? (size_t)slot * slotSize_ : 0;
    uint32_t raw = ReadAt(base + f.off, f.bytes);
    if (f.kind == K_DIFF)    return raw >= 1 ? (raw - 1) / 2 : 0;
    if (f.kind == K_TIME_60) return raw / 60;
    return raw;
}
void SaveFile::WriteField(const Field& f, int slot, uint32_t v) {
    if (f.maxv && v > f.maxv) v = f.maxv;
    size_t base = slotted() ? (size_t)slot * slotSize_ : 0;
    uint32_t raw = v;
    if (f.kind == K_DIFF)    raw = v * 2 + 1;
    if (f.kind == K_TIME_60) raw = v * 60;
    WriteAt(base + f.off, f.bytes, raw);
}

bool SaveFile::Save(std::string& err) {
    if (!loaded_) { err = "no save loaded"; return false; }
    // one-time backup of the original
    std::string bak = path_ + ".bak";
    { std::ifstream chk(bak, std::ios::binary);
      if (!chk.good()) { std::ifstream src(path_, std::ios::binary); std::ofstream dst(bak, std::ios::binary);
                         if (src.good() && dst.good()) dst << src.rdbuf(); } }
    // Resign hook: DMC1/2/3 HD carry no checksum; DMC5 DSSS hash is not locally
    // validated for direct edits — so all current games are a straight write-back.
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) { err = "cannot open for writing"; return false; }
    out.write((const char*)data_.data(), (std::streamsize)data_.size());
    if (!out) { err = "write failed"; return false; }
    dirty_ = false;
    return true;
}

} // namespace sav
