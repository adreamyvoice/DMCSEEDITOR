// bc7.h — BC7 (DX10 / DXGI_FORMAT_BC7) block decode + a simple mode-6 re-encoder.
// DMC3 HD stores some stage/effect textures as BC7 (via the DDS DX10 header). BC7 has no
// trivial endpoint-recolour trick like DXT (8 modes, partitions, variable endpoints), so to
// edit one we DECODE the block to RGBA, recolour the pixels, then RE-ENCODE as mode 6
// (single subset, RGBA 7+pbit endpoints, 4-bit indices) — which can represent any block and
// keeps the 16-byte block size, so it splices back into the .pac in place.
#pragma once
#include <cstdint>

namespace bc7 {

// Decode one 16-byte BC7 block into 16 RGBA pixels (row-major 4x4). Always fills out.
void DecodeBlock(const uint8_t in[16], uint8_t out[16][4]);

// Encode 16 RGBA pixels (row-major 4x4) into one 16-byte BC7 mode-6 block.
void EncodeBlockMode6(const uint8_t in[16][4], uint8_t out[16]);

} // namespace bc7
