#include "lmt_anim.h"
#include <cstring>
#include <cmath>

namespace anim {

struct V4 { float x, y, z, w; };

static uint32_t rdU32(const std::vector<uint8_t>& b, size_t o) {
    uint32_t v = 0; if (o + 4 <= b.size()) memcpy(&v, &b[o], 4); return v;
}
static float rdF32(const std::vector<uint8_t>& b, size_t o) {
    float v = 0; if (o + 4 <= b.size()) memcpy(&v, &b[o], 4); return v;
}
static V4 rdV4(const std::vector<uint8_t>& b, size_t o) {
    return { rdF32(b,o), rdF32(b,o+4), rdF32(b,o+8), rdF32(b,o+12) };
}
static uint64_t rdU64(const std::vector<uint8_t>& b, size_t o) {
    uint64_t v = 0; for (int i = 0; i < 8 && o + i < b.size(); ++i) v |= (uint64_t)b[o+i] << (8*i); return v;
}

struct Key { int frame; V4 v; };

// One track's decoded keyframes plus metadata.
struct Track {
    int   boneID;     // -1 = none
    int   type;       // 0 LocalRot,1 LocalPos,2 LocalScale,3 AbsRot,4 AbsPos
    V4    reference;
    std::vector<Key> keys;
    bool  isRot() const { return type == 0 || type == 3; }
};

// element byte size per codec (variable ones handled inline)
static int elemSize(int comp) {
    switch (comp) {
        case 1: return 12;   // SingleVector3
        case 2: return 12;   // StepRotationQuat3 (when buffered)
        case 3: return 16;   // LinearVector3
        case 4: return 8;    // BiLinearVector3_16
        case 5: return 4;    // BiLinearVector3_8
        case 6: return 8;    // LinearRotationQuat4_14
        case 7: return 4;    // BiLinearRotationQuat4_7
        case 11: case 12: case 13: return 4;  // QuatXW/YW/ZW_14
        case 14: return 6;   // Quat4_11
        case 15: return 5;   // Quat4_9
        default: return 0;
    }
}
static bool usesMinMax(int comp) {
    return comp==4||comp==5||comp==7||comp==11||comp==12||comp==13||comp==14||comp==15;
}

// decode one element -> normalized/raw V4 + frame delta. `data` points at the element.
static V4 decodeElem(int comp, const std::vector<uint8_t>& b, size_t p, int& frameDelta) {
    frameDelta = 1;
    V4 out{0,0,0,0};
    switch (comp) {
        case 1: out = { rdF32(b,p), rdF32(b,p+4), rdF32(b,p+8), 1 }; frameDelta=1; break;
        case 2: { float x=rdF32(b,p),y=rdF32(b,p+4),z=rdF32(b,p+8);
                  float w2=1-x*x-y*y-z*z; out={x,y,z, w2>0?std::sqrt(w2):0}; frameDelta=1; } break;
        case 3: out = { rdF32(b,p), rdF32(b,p+4), rdF32(b,p+8), 1 }; frameDelta=(int)rdU32(b,p+12); break;
        case 4: { uint16_t u[4]; for(int i=0;i<4;i++){uint16_t t=0; if(p+2+i*2<=b.size())memcpy(&t,&b[p+i*2],2); u[i]=t;}
                  out={u[0]/65535.0f,u[1]/65535.0f,u[2]/65535.0f,1}; frameDelta=u[3]; } break;
        case 5: { out={b[p]/255.0f,b[p+1]/255.0f,b[p+2]/255.0f,1}; frameDelta=b[p+3]; } break;
        case 6: { uint64_t d=rdU64(b,p); const int M=0x3FFF; const float SMAX=0x3FFF*0.5f; const float MUL=1.0f/(0x3FFF/4.0f);
                  float c[4]={(float)((d>>42)&M),(float)((d>>28)&M),(float)((d>>14)&M),(float)(d&M)};
                  for(int i=0;i<4;i++){ if(c[i]>SMAX) c[i]=-(float)(M-(int)c[i]); c[i]*=MUL; }
                  out={c[0],c[1],c[2],c[3]}; frameDelta=(int)(d>>56); if(frameDelta==0)frameDelta=1; } break;
        case 7: { uint32_t d=rdU32(b,p); const int M=0x7F;
                  out={ ((d>>21)&M)/127.0f, ((d>>14)&M)/127.0f, ((d>>7)&M)/127.0f, (d&M)/127.0f };
                  frameDelta=(int)(d>>28); if(frameDelta==0)frameDelta=1; } break;
        case 11: case 12: case 13: { uint32_t d=rdU32(b,p); const int M=0x3FFF; float a=(d&M)/16383.0f, wv=((d>>14)&M)/16383.0f;
                  if(comp==11) out={a,0,0,wv}; else if(comp==12) out={0,a,0,wv}; else out={0,0,a,wv};
                  frameDelta=(int)(d>>28); if(frameDelta==0)frameDelta=1; } break;
        case 14: { uint64_t d=rdU64(b,p); const int M=0x7FF;
                  float X=(float)(d&M), Y=(float)((d>>11)&M), Z=(float)((d>>22)&M), W=(float)((d>>33)&M);
                  out={X/2047.0f,Y/2047.0f,Z/2047.0f,W/2047.0f}; int fd=(int)((d>>44)&0xFFF); frameDelta=fd?fd:1; } break;
        case 15: { uint64_t d=rdU64(b,p); const int M=0x1FF;
                  float X=(float)(d&M), Y=(float)((d>>9)&M), Z=(float)((d>>18)&M), W=(float)((d>>27)&M);
                  out={X/511.0f,Y/511.0f,Z/511.0f,W/511.0f}; frameDelta=(int)(b[p+4]>>4); if(frameDelta==0)frameDelta=1; } break;
    }
    return out;
}

static void decodeTrack(const std::vector<uint8_t>& b, size_t trackOff, Track& tk) {
    int comp = b[trackOff];
    tk.type   = b[trackOff+1];
    tk.boneID = (b[trackOff+3]==0xFF) ? -1 : b[trackOff+3];
    uint32_t bufSize = rdU32(b, trackOff+8);
    uint32_t bufOff  = rdU32(b, trackOff+12);
    tk.reference = rdV4(b, trackOff+16);
    uint32_t extOff = rdU32(b, trackOff+32);

    // constant / empty -> single keyframe = reference
    if (bufSize == 0 || bufOff == 0 || comp == 0) {
        tk.keys.push_back({0, tk.reference});
        return;
    }
    V4 mn{0,0,0,0}, mx{0,0,0,0};
    if (usesMinMax(comp) && extOff) { mn = rdV4(b, extOff); mx = rdV4(b, extOff+16); }

    int es = elemSize(comp);
    if (es <= 0) { tk.keys.push_back({0, tk.reference}); return; }
    int cur = 0;
    for (uint32_t off = 0; off + es <= bufSize; off += es) {
        int fd = 1;
        V4 d = decodeElem(comp, b, bufOff + off, fd);
        if (usesMinMax(comp) && extOff) {
            d.x = mx.x + mn.x * d.x; d.y = mx.y + mn.y * d.y;
            d.z = mx.z + mn.z * d.z; d.w = mx.w + mn.w * d.w;
        }
        tk.keys.push_back({cur, d});
        cur += fd;
    }
    if (tk.keys.empty()) tk.keys.push_back({0, tk.reference});
    // prepend reference as rest pose if the first key isn't at 0
    if (tk.keys.front().frame > 0) tk.keys.insert(tk.keys.begin(), {0, tk.reference});
}

static V4 nlerp(V4 a, V4 b, float t) {
    // shortest-path
    float dot = a.x*b.x+a.y*b.y+a.z*b.z+a.w*b.w;
    if (dot < 0) { b.x=-b.x; b.y=-b.y; b.z=-b.z; b.w=-b.w; }
    V4 r{ a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t };
    float l = std::sqrt(r.x*r.x+r.y*r.y+r.z*r.z+r.w*r.w); if (l<1e-8f) l=1;
    return { r.x/l, r.y/l, r.z/l, r.w/l };
}
static V4 lerp(V4 a, V4 b, float t) {
    return { a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t, a.w+(b.w-a.w)*t };
}

static V4 evalTrack(const Track& tk, float f) {
    const auto& k = tk.keys;
    if (k.empty()) return tk.reference;
    if (f <= k.front().frame) return k.front().v;
    if (f >= k.back().frame)  return k.back().v;
    for (size_t i = 0; i + 1 < k.size(); ++i) {
        if (f >= k[i].frame && f <= k[i+1].frame) {
            float span = (float)(k[i+1].frame - k[i].frame); if (span < 1e-6f) return k[i].v;
            float t = (f - k[i].frame) / span;
            return tk.isRot() ? nlerp(k[i].v, k[i+1].v, t) : lerp(k[i].v, k[i+1].v, t);
        }
    }
    return k.back().v;
}

// quaternion(x,y,z,w) -> row-major rotation matrix (row-vector convention)
static void quatToMat(const V4& q, mdl::Mat4& m) {
    float x=q.x,y=q.y,z=q.z,w=q.w;
    m.m[0]=1-2*(y*y+z*z); m.m[1]=2*(x*y+w*z);   m.m[2]=2*(x*z-w*y);   m.m[3]=0;
    m.m[4]=2*(x*y-w*z);   m.m[5]=1-2*(x*x+z*z); m.m[6]=2*(y*z+w*x);   m.m[7]=0;
    m.m[8]=2*(x*z+w*y);   m.m[9]=2*(y*z-w*x);   m.m[10]=1-2*(x*x+y*y);m.m[11]=0;
    m.m[12]=0; m.m[13]=0; m.m[14]=0; m.m[15]=1;
}

MotionInfo GetMotion(const std::vector<uint8_t>& lmt, uint32_t mo) {
    MotionInfo mi;
    if (mo == 0 || mo + 16 > lmt.size()) return mi;
    mi.numFrames = (int)rdU32(lmt, mo + 8);
    mi.valid = true;
    return mi;
}

float MatchScore(const std::vector<uint8_t>& lmt, uint32_t mo, const mdl::Model& model) {
    if (mo == 0 || mo + 16 > lmt.size()) return 0.0f;
    uint32_t tracksPtr = rdU32(lmt, mo);
    uint32_t numTracks = rdU32(lmt, mo + 4);
    int total = 0, mapped = 0;
    for (uint32_t t = 0; t < numTracks; ++t) {
        size_t to = tracksPtr + (size_t)t * 36;
        if (to + 36 > lmt.size()) break;
        int boneID = (lmt[to + 3] == 0xFF) ? -1 : lmt[to + 3];
        if (boneID < 0) continue;                      // scene/abs track, not bone-bound
        ++total;
        if (model.boneIndexForId(boneID) >= 0) ++mapped;
    }
    return total ? (float)mapped / total : 0.0f;
}

std::vector<mdl::Mat4> PoseLocals(const std::vector<uint8_t>& lmt, uint32_t mo,
                                  float frame, const mdl::Model& model) {
    std::vector<mdl::Mat4> locals = model.localBind();   // start from bind pose
    if (mo == 0 || mo + 16 > lmt.size()) return locals;
    uint32_t tracksPtr = rdU32(lmt, mo);
    uint32_t numTracks = rdU32(lmt, mo + 4);
    int nb = model.numBones();

    for (uint32_t t = 0; t < numTracks; ++t) {
        size_t to = tracksPtr + (size_t)t * 36;
        if (to + 36 > lmt.size()) break;
        Track tk; decodeTrack(lmt, to, tk);
        if (tk.boneID < 0) continue;                      // skip scene/abs (boneID none)
        int bi = model.boneIndexForId(tk.boneID);         // LMT ID -> bone array index
        if (bi < 0 || bi >= nb) continue;
        V4 val = evalTrack(tk, frame);
        mdl::Mat4& L = locals[bi];
        if (tk.type == 0) {            // LocalRotation: replace 3x3, keep translation
            float tx=L.m[12], ty=L.m[13], tz=L.m[14];
            quatToMat(val, L);
            L.m[12]=tx; L.m[13]=ty; L.m[14]=tz;
        } else if (tk.type == 1) {     // LocalPosition: replace translation
            L.m[12]=val.x; L.m[13]=val.y; L.m[14]=val.z;
        }
        // LocalScale (2) and Absolute (3,4) ignored for the skeleton view
    }
    return locals;
}

} // namespace anim
