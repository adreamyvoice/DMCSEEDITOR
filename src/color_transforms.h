// color_transforms.h — the preset channel math from RGBlue16's color menu.
// Reconstructed from FILE_FORMAT.md §5 (preset names + 0-255 clamps). The exact
// rounding of the 50%/80% presets is inferred (truncating integer math), matching
// how the E-runtime integer helpers behave; correct here if a sample proves otherwise.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace efx {

struct RGB { uint8_t r, g, b; };

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

enum class Preset {
    Half,        // c = c / 2            (颜色值取50%)
    EightyPct,   // c = c * 4 / 5        (取80%)
    AddRed100,   // R += 100             (红色值增加100)
    DelRed, DelGreen, DelBlue,           // delete a channel (set 0)
    SwapRB, SwapRG, SwapGB,              // swap two channels
    WhiteToBlue, WhiteToRed, WhiteToGreen, WhiteToBlack, // White->X remap
    BlackToWhite, BlackToBlue,           // Black->X remap
};

struct PresetInfo { Preset p; const char* label; };

inline std::vector<PresetInfo> AllPresets() {
    return {
        {Preset::Half,        "50% value"},
        {Preset::EightyPct,   "80% value"},
        {Preset::AddRed100,   "Red +100"},
        {Preset::DelRed,      "Delete red"},
        {Preset::DelGreen,    "Delete green"},
        {Preset::DelBlue,     "Delete blue"},
        {Preset::SwapRB,      "Swap red/blue"},
        {Preset::SwapRG,      "Swap red/green"},
        {Preset::SwapGB,      "Swap green/blue"},
        {Preset::WhiteToBlue, "White -> blue"},
        {Preset::WhiteToRed,  "White -> red"},
        {Preset::WhiteToGreen,"White -> green"},
        {Preset::WhiteToBlack,"White -> black"},
        {Preset::BlackToWhite,"Black -> white"},
        {Preset::BlackToBlue, "Black -> blue"},
    };
}

inline bool isWhite(RGB c) { return c.r > 240 && c.g > 240 && c.b > 240; }
inline bool isBlack(RGB c) { return c.r < 16  && c.g < 16  && c.b < 16; }

inline RGB ApplyPreset(Preset p, RGB c) {
    switch (p) {
        case Preset::Half:      return {clamp8(c.r/2), clamp8(c.g/2), clamp8(c.b/2)};
        case Preset::EightyPct: return {clamp8(c.r*4/5), clamp8(c.g*4/5), clamp8(c.b*4/5)};
        case Preset::AddRed100: return {clamp8(c.r+100), c.g, c.b};
        case Preset::DelRed:    return {0, c.g, c.b};
        case Preset::DelGreen:  return {c.r, 0, c.b};
        case Preset::DelBlue:   return {c.r, c.g, 0};
        case Preset::SwapRB:    return {c.b, c.g, c.r};
        case Preset::SwapRG:    return {c.g, c.r, c.b};
        case Preset::SwapGB:    return {c.r, c.b, c.g};
        case Preset::WhiteToBlue:  return isWhite(c) ? RGB{0,0,255}   : c;
        case Preset::WhiteToRed:   return isWhite(c) ? RGB{255,0,0}   : c;
        case Preset::WhiteToGreen: return isWhite(c) ? RGB{0,255,0}   : c;
        case Preset::WhiteToBlack: return isWhite(c) ? RGB{0,0,0}     : c;
        case Preset::BlackToWhite: return isBlack(c) ? RGB{255,255,255} : c;
        case Preset::BlackToBlue:  return isBlack(c) ? RGB{0,0,255}   : c;
    }
    return c;
}

} // namespace efx
