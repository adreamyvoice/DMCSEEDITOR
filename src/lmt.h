// lmt.h — inspector for MT Framework ".lmt" (motion list) files, DMC4 version 67.
// Layout (verified against motion\pl030\* unpacked from uPlayerVergil.arc):
//   header: char magic[4]="LMT\0"; u16 version(=67); u16 numMotions;
//   table:  numMotions x u32 absolute offsets (0 = empty slot);
//   each motion header (v67): u32 tracksPtr; u32 numTracks; u32 numFrames; u32 loopFrame; ...
//
// This is an INSPECTOR + safe scalar editor, NOT a full keyframe/pose editor. It lets
// you see every motion (frame count, track count, loop frame) and tweak those scalars;
// rewriting the compressed bone keyframe buffers is a much deeper effort and is out of
// scope here — use the Hex/Manual tools for raw buffer pokes.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace lmt {

using Bytes = std::vector<uint8_t>;

struct Motion {
    int      index   = 0;
    uint32_t offset  = 0;   // header offset in the file (0 => empty slot)
    uint32_t tracksPtr = 0;
    uint32_t numTracks = 0;
    uint32_t numFrames = 0;
    uint32_t loopFrame = 0;
    bool     empty   = true;
};

class LmtFile {
public:
    // Parse already-decompressed bytes (from arc::Extract or a loose .lmt). Pass
    // fresh=true on the first parse after loading a file to snapshot the original
    // offset table (so remaps can always refer back to the original slot data).
    bool Parse(const Bytes& data, std::string& err, bool fresh = false);
    bool valid() const { return valid_; }
    uint16_t version() const { return version_; }
    const std::vector<Motion>& motions() const { return motions_; }
    const std::vector<uint32_t>& originalOffsets() const { return origOffsets_; }
    int liveCount() const;  // non-empty motion slots

    // Which ORIGINAL slot's animation a slot currently plays (-1 if it points nowhere
    // recognisable). After RemapSlot(5,12), playsSlot(5) == 12.
    int playsSlot(int slot) const;

    // Remap: make `slot` play `srcSlot`'s original animation (table[slot] = origOffset[srcSlot]).
    bool RemapSlot(Bytes& out, int slot, int srcSlot, std::string& err) const;
    // Swap two slots' table entries (each plays the other's animation).
    bool SwapSlots(Bytes& out, int a, int b, std::string& err) const;
    // Restore a slot to play its own original animation.
    bool ResetSlot(Bytes& out, int slot, std::string& err) const { return RemapSlot(out, slot, slot, err); }

    // Edit a motion's scalar header fields (numFrames / loopFrame) in `out`, which must
    // be the same buffer Parse() saw. Returns false on bad index/offset.
    enum Field { NumFrames, LoopFrame };
    bool SetField(Bytes& out, int motionIndex, Field f, uint32_t value, std::string& err) const;

private:
    bool valid_ = false;
    uint16_t version_ = 0;
    std::vector<Motion> motions_;
    std::vector<uint32_t> origOffsets_;  // offset-table snapshot from the fresh load
};

} // namespace lmt
