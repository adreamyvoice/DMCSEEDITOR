// sdl.h — MT Framework ".sdl" scene / stage definition reader (DMC4SE).
// SDL is a node graph: magic "SDL\0" + u16 version + nodes, each a typed node
// (Root, MotionCamera, ImagePlaneFilter, Orb00, ColorFog, ...) with named
// properties (mPos, mFov, mColor, mOrbType, ...). Used for stage scene setup
// (cameras, fog, orb/item placement). This decodes the node/property outline to
// readable text (the "converter" read direction) so stage scenes can be inspected.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sdl {

using Bytes = std::vector<uint8_t>;

bool IsSDL(const Bytes& b);
// Human-readable outline: each node type and the properties under it, in order.
std::string ToText(const Bytes& b);

} // namespace sdl
