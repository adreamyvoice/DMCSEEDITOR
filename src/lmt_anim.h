// lmt_anim.h — DMC4 LMT v67 keyframe decoder + skeleton poser.
// Implements the MT Framework motion format per the RevilLib spec:
//   motion header (64B x86): u32 tracksPtr, numTracks, numFrames, i32 loopFrame, ...
//   track (36B x86): u8 compression, u8 trackType, u8 boneType, u8 boneID(0xFF=none),
//                    f32 weight, u32 bufferSize, u32 buffer(abs), Vec4 reference, u32 extremes(abs).
//   trackType: 0=LocalRot 1=LocalPos 2=LocalScale 3=AbsRot 4=AbsPos.
//   extremes -> TrackMinMax {Vec4 min; Vec4 max;}; quantised value = max + min*decoded.
//   Codecs (compression, TrackV2 table): 1 SingleVec3, 2 StepQuat3, 3 LinearVec3,
//     4 BiLinearVec3_16, 5 BiLinearVec3_8, 6 LinearQuat4_14, 7 BiLinearQuat4_7,
//     11/12/13 QuatXW/YW/ZW_14, 14 Quat4_11, 15 Quat4_9.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "model.h"   // mdl::Mat4, mdl::Vec3

namespace anim {

// Number of motions and their frame counts come from lmt::LmtFile; here we work on the
// raw decompressed LMT bytes plus a motion header offset.

struct MotionInfo { int numFrames = 0; bool valid = false; };

// Read a motion's frame count etc. from the raw bytes at motionOffset.
MotionInfo GetMotion(const std::vector<uint8_t>& lmt, uint32_t motionOffset);

// How well a motion fits a skeleton: fraction of its bone-targeted tracks whose bone ID
// maps to a real bone in `model` (1.0 = perfect match, low = wrong character).
float MatchScore(const std::vector<uint8_t>& lmt, uint32_t motionOffset, const mdl::Model& model);

// Produce per-bone LOCAL matrices for `motionOffset` at `frame` (0..numFrames-1),
// starting from the model's bind locals and overriding animated channels. Then the
// caller runs model.ComposeWorldPos(out) for world positions.
std::vector<mdl::Mat4> PoseLocals(const std::vector<uint8_t>& lmt, uint32_t motionOffset,
                                  float frame, const mdl::Model& model);

} // namespace anim
