// savedit.h — Steam save editor for the DMC games (DMC1/2/3 HD Collection, DMC4SE, DMC5).
// Loads a save file, auto-detects which game it is, exposes friendly value fields + an
// arbitrary offset/value poke, and writes back with a one-time .bak. "Resign" = per-game
// integrity fix on save: the DMC HD saves carry NO checksum (verified against the
// open-source dmc1 editor + the raw files), so save is a straight write; DMC5's DSSS hash
// is not locally validated for direct edits, so it's a straight write too (with a warning).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sav {

using Bytes = std::vector<uint8_t>;

enum Game { GAME_UNKNOWN=0, GAME_DMC1, GAME_DMC2, GAME_DMC3, GAME_DMC4SE, GAME_DMC5 };

enum Kind {
    K_UINT = 0,      // plain little-endian unsigned of `bytes` width
    K_DIFF,          // DMC1 difficulty: stored (index*2)+1, shown as index
    K_TIME_60,       // playtime stored as seconds*60 (u32), shown as seconds
};

struct Field {
    const char* name;
    int      off;     // offset within a slot (slotted games) or absolute (single-save games)
    int      bytes;   // 1, 2 or 4
    int      kind;    // Kind
    uint32_t maxv;    // UI clamp (0 = no clamp beyond width)
    const char* help; // tooltip / note
};

// One editable save (whole file in memory).
class SaveFile {
public:
    bool Load(const std::string& path, std::string& err);
    bool loaded() const { return loaded_; }
    Game game() const { return game_; }
    const char* gameName() const;
    const std::string& path() const { return path_; }
    bool dirty() const { return dirty_; }

    int slotCount() const { return slotCount_; }
    int slotSize()  const { return slotSize_; }
    bool slotted()  const { return slotCount_ > 1; }
    // Is this slot in use? (header signature present) — slotted games only.
    bool slotUsed(int slot) const;

    // Friendly fields for this game (empty if none mapped yet).
    const std::vector<Field>& fields() const { return fields_; }

    // Read/write a friendly field for `slot` (slot ignored for single-save games).
    uint32_t ReadField(const Field& f, int slot) const;
    void     WriteField(const Field& f, int slot, uint32_t v);

    // Arbitrary poke: read/write `width` bytes (1/2/4) little-endian at absolute `off`.
    uint32_t ReadAt(size_t off, int width) const;
    bool     WriteAt(size_t off, int width, uint32_t v);

    const Bytes& data() const { return data_; }
    size_t size() const { return data_.size(); }

    // Write back to disk; makes a one-time `<path>.bak` first. Applies per-game resign.
    bool Save(std::string& err);

private:
    void detect();
    std::string path_;
    Bytes  data_;
    Game   game_ = GAME_UNKNOWN;
    int    slotCount_ = 1, slotSize_ = 0;
    std::vector<Field> fields_;
    bool   loaded_ = false, dirty_ = false;
};

} // namespace sav
