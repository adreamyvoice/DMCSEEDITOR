#include "model.h"
#include <cstring>
#include <cstdio>
#include <cmath>

namespace mdl {

Mat4 Identity() {
    Mat4 r; memset(r.m, 0, sizeof r.m);
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

static Vec3 Translation(const Mat4& m) { return { m.m[12], m.m[13], m.m[14] }; }
static Mat4 AffineInverse(const Mat4& m);   // forward decl (defined below)
static Vec3 DecodeVtx(const std::vector<uint8_t>& b, size_t p, const Model::MeshRec& r,
                      const float mn[3], float scale);   // forward decl (defined below)

// IEEE half (float16) -> float32.
static float Half2Float(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF, f;
    if (e == 0) {
        if (m == 0) f = s << 31;
        else { while (!(m & 0x400)) { m <<= 1; e--; } e++; m &= ~0x400u; f = (s<<31) | ((e+112)<<23) | (m<<13); }
    } else if (e == 31) f = (s<<31) | 0x7F800000u | (m<<13);
    else f = (s<<31) | ((e+112)<<23) | (m<<13);
    float r; memcpy(&r, &f, 4); return r;
}

// Find the UV (2x float16) inside a vertex by scanning candidate offsets: pick the one whose
// two components both vary and stay in a tex-coord range. DMC4 UV offset varies by format, so
// detect per-mesh rather than hardcode a (large, incomplete) format table. -1 if none.
static int DetectUV(const std::vector<uint8_t>& b, size_t vbase, int stride, int vc) {
    int best = -1; float bestScore = -1.0f;
    int N = vc < 60 ? vc : 60;
    for (int so = 6; so + 4 <= stride; so += 2) {
        int inr = 0, cnt = 0;
        float umin=1e9f,umax=-1e9f,vmin=1e9f,vmax=-1e9f;
        for (int k = 0; k < N; ++k) {
            size_t p = vbase + (size_t)k*stride + so;
            if (p + 4 > b.size()) break;
            uint16_t hu,hv; memcpy(&hu,&b[p],2); memcpy(&hv,&b[p+2],2);
            float u = Half2Float(hu), v = Half2Float(hv);
            ++cnt;
            if (!std::isfinite(u) || !std::isfinite(v)) continue;
            if (u>=-0.2f&&u<=8.0f&&v>=-0.2f&&v<=8.0f) ++inr;
            if(u<umin)umin=u; if(u>umax)umax=u; if(v<vmin)vmin=v; if(v>vmax)vmax=v;
        }
        if (cnt == 0) continue;
        float frac = (float)inr/cnt, vu = umax-umin, vv = vmax-vmin;
        if (frac < 0.9f || vu < 0.03f || vv < 0.03f) continue;     // both axes must vary + be in range
        float score = frac + (vu<1?vu:1) + (vv<1?vv:1);
        if (score > bestScore) { bestScore = score; best = so; }
    }
    return best;
}

bool Model::Parse(const std::vector<uint8_t>& b, std::string& err) {
    valid_ = false; parents_.clear(); local_.clear(); bindPos_.clear();
    if (b.size() < 0x28 || memcmp(b.data(), "MOD\0", 4) != 0) { err = "not a MOD file"; return false; }
    uint16_t numBones; memcpy(&numBones, &b[6], 2);
    uint32_t bonesOff;  memcpy(&bonesOff, &b[0x24], 4);
    idToIndex_.assign(256, -1);
    // Skinned character models have a bone section; static stage meshes have 0 bones.
    // Parse bones when present, but still decode the mesh either way (for view/export).
    if (numBones > 0) {
        uint32_t localOff = bonesOff + (uint32_t)numBones * 24;
        uint32_t absOff   = localOff + (uint32_t)numBones * 64;
        if (absOff > b.size()) { err = "bone section past end of file"; return false; }
        parents_.resize(numBones);
        local_.resize(numBones);
        for (int i = 0; i < numBones; ++i) {
            uint8_t id = b[bonesOff + i * 24 + 0];            // hierarchy ID (what LMT references)
            parents_[i] = (int8_t)b[bonesOff + i * 24 + 1];   // 0xFF -> -1 (root)
            idToIndex_[id] = i;
            memcpy(local_[i].m, &b[localOff + (size_t)i * 64], 64);
        }
        bindPos_ = ComposeWorldPos(local_);
    }

    // --- mesh vertex point cloud --------------------------------------------
    // MOD mesh table: u16 numMeshes@8; u32 meshOffset@0x30; u32 vertexOffset@0x34.
    // bbox min@0x50 (3f), max@0x60 (3f). Each 48-byte mesh entry:
    //   +8  u8 format; +10 u8 vertexStride; +12 u32 vertexStart(in-block);
    //   +16 u32 vertexBlockByteOffset (into the vertex buffer); +42 u16 vertexCount.
    // Position = first 3 int16, unsigned-normalised across the model bbox.
    cloud_.clear();
    tris_.clear();
    raw_ = b;            // keep original bytes for faithful export / position reimport
    meshRecs_.clear();
    scale_ = 1.0f;
    if (b.size() > 0x70) {
        uint16_t numMeshes; memcpy(&numMeshes, &b[8], 2);
        uint32_t meshOff, vertOff, faceOff;
        memcpy(&meshOff, &b[0x30], 4);
        memcpy(&vertOff, &b[0x34], 4);
        memcpy(&faceOff, &b[0x38], 4);
        float mn[3], mx[3];
        memcpy(mn, &b[0x50], 12); memcpy(mx, &b[0x60], 12);
        memcpy(mn_, mn, 12); memcpy(mx_, mx, 12);
        auto inBox = [&](float x, float y, float z) {
            return x>mn[0]-20 && x<mx[0]+20 && y>mn[1]-20 && y<mx[1]+20 && z>mn[2]-20 && z<mx[2]+20;
        };
        // PASS A: build per-mesh records, classify float vs int16, and find the max int16
        // value per axis (over int16 meshes) to derive the true quantisation scale.
        uint32_t umax[3] = {1,1,1};
        for (int mi = 0; mi < numMeshes; ++mi) {
            size_t o = meshOff + (size_t)mi * 48;
            if (o + 48 > b.size()) break;
            uint8_t  stride = b[o + 10];
            uint16_t vmax;   memcpy(&vmax, &b[o + 42], 2);   // max vertex index (+1)
            uint32_t vstart; memcpy(&vstart, &b[o + 12], 4); // first vertex index in block
            uint32_t voff;   memcpy(&voff,   &b[o + 16], 4);
            uint32_t idxStart; memcpy(&idxStart, &b[o + 24], 4);
            uint32_t idxCount; memcpy(&idxCount, &b[o + 28], 4);
            if (stride < 6) continue;
            int vc = (int)vmax - (int)vstart; if (vc < 0) vc = 0;
            size_t blockBase = (size_t)vertOff + voff;
            size_t vbase = blockBase + (size_t)vstart * stride;
            // float ONLY if every vertex is a finite, in-bbox float AND the float-space extent
            // is non-degenerate (a real model-space mesh, not int16 bytes that merely look like
            // tiny in-box floats). Otherwise int16.
            bool isFloat = false;
            if (vc > 0) {
                bool allFloat = true; float flo[3]={1e30f,1e30f,1e30f}, fhi[3]={-1e30f,-1e30f,-1e30f};
                for (int s = 0; s < vc && allFloat; ++s) {
                    size_t sp = vbase + (size_t)s * stride;
                    if (sp + 12 > b.size()) { allFloat = false; break; }
                    float f[3]; memcpy(f, &b[sp], 12);
                    if (!inBox(f[0],f[1],f[2])) { allFloat = false; break; }
                    for (int a=0;a<3;++a){ if(f[a]<flo[a])flo[a]=f[a]; if(f[a]>fhi[a])fhi[a]=f[a]; }
                }
                float ext = 0; for (int a=0;a<3;++a){ float e=fhi[a]-flo[a]; if(e>ext)ext=e; }
                isFloat = allFloat && ext > 1.0f;
            }
            if (!isFloat) {
                for (int v = 0; v < vc; ++v) {
                    size_t p = vbase + (size_t)v * stride; if (p + 6 > b.size()) break;
                    uint16_t u0,u1,u2; memcpy(&u0,&b[p],2); memcpy(&u1,&b[p+2],2); memcpy(&u2,&b[p+4],2);
                    if (u0>umax[0])umax[0]=u0; if (u1>umax[1])umax[1]=u1; if (u2>umax[2])umax[2]=u2;
                }
            }
            MeshRec r; r.blockBase=(uint32_t)blockBase; r.stride=stride; r.vstart=vstart; r.vmax=vmax;
            r.isFloat=isFloat; r.faceByteOff=(uint32_t)((size_t)faceOff + (size_t)idxStart*2); r.idxCount=idxCount;
            r.uvOff = DetectUV(b, vbase, stride, vc);
            meshRecs_.push_back(r);
        }
        // Uniform quant scale: pos[a] = mn[a] + u[a]*scale. Each axis's max int16 maps to the
        // bbox max (the bbox is the vert extent), so scale = (mx-mn)/umax; the three axes agree.
        // Use the axis with the largest umax (best precision, surely hits the extreme). Dividing
        // by a fixed 65535 squashed models whose verts don't span the full range -> "skinny".
        { int ba = 0; for (int a=1;a<3;++a) if (umax[a]>umax[ba]) ba=a;
          float ext = mx[ba]-mn[ba];
          scale_ = (umax[ba] > 0 && ext > 0) ? ext / (float)umax[ba] : 1.0f; }
        // PASS B: decode positions for the point cloud + solid triangles using the real scale.
        for (const MeshRec& r : meshRecs_) {
            for (uint32_t v = r.vstart; v < r.vmax; ++v) {
                size_t p = r.blockBase + (size_t)v * r.stride; if (p + 6 > b.size()) break;
                cloud_.push_back(DecodeVtx(b, p, r, mn_, scale_));
            }
            for (uint32_t k = 0; k + 3 <= r.idxCount; k += 3) {
                size_t ip = r.faceByteOff + (size_t)k * 2;
                if (ip + 6 > b.size()) break;
                uint16_t a,c,d; memcpy(&a,&b[ip],2); memcpy(&c,&b[ip+2],2); memcpy(&d,&b[ip+4],2);
                if (a==0xFFFF||c==0xFFFF||d==0xFFFF) continue;
                size_t pa=r.blockBase+(size_t)a*r.stride, pc=r.blockBase+(size_t)c*r.stride, pd=r.blockBase+(size_t)d*r.stride;
                if (pa+6>b.size()||pc+6>b.size()||pd+6>b.size()) continue;
                tris_.push_back(DecodeVtx(b,pa,r,mn_,scale_));
                tris_.push_back(DecodeVtx(b,pc,r,mn_,scale_));
                tris_.push_back(DecodeVtx(b,pd,r,mn_,scale_));
            }
        }
    }

    // --- skinning prep: bind world matrices, inverse-bind, nearest-bone per tri vert ---
    // (skinned models only; static stage meshes keep their bind-pose geometry as-is)
    if (numBones > 0) {
        bindWorld_ = ComposeWorld(local_);
        invBind_.resize(numBones);
        for (int i = 0; i < numBones; ++i) invBind_[i] = AffineInverse(bindWorld_[i]);
        triBone_.assign(tris_.size(), 0);
        for (size_t v = 0; v < tris_.size(); ++v) {
            const Vec3& p = tris_[v];
            int best = 0; float bd = 1e30f;
            for (int bi = 0; bi < numBones; ++bi) {
                const Vec3& bp = bindPos_[bi];
                float dx=p.x-bp.x, dy=p.y-bp.y, dz=p.z-bp.z, d=dx*dx+dy*dy+dz*dz;
                if (d < bd) { bd = d; best = bi; }
            }
            triBone_[v] = best;
        }
    }

    // Valid if we have a skeleton or at least some decoded geometry.
    valid_ = (numBones > 0) || !tris_.empty() || !cloud_.empty();
    if (!valid_) { err = "model has neither bones nor a decodable mesh"; return false; }
    return true;
}

// Decode one vertex position from the raw MOD at byte `p` for mesh record `r`.
// int16 positions use the uniform per-model quant scale: pos[a] = mn[a] + u[a]*scale.
static Vec3 DecodeVtx(const std::vector<uint8_t>& b, size_t p, const Model::MeshRec& r,
                      const float mn[3], float scale) {
    if (r.isFloat) {
        float f0,f1,f2; memcpy(&f0,&b[p],4); memcpy(&f1,&b[p+4],4); memcpy(&f2,&b[p+8],4);
        return { f0, f1, f2 };
    }
    uint16_t u0,u1,u2; memcpy(&u0,&b[p],2); memcpy(&u1,&b[p+2],2); memcpy(&u2,&b[p+4],2);
    return { mn[0]+u0*scale, mn[1]+u1*scale, mn[2]+u2*scale };
}

int Model::meshVertTotal() const {
    int n = 0; for (const MeshRec& r : meshRecs_) n += (int)(r.vmax - r.vstart); return n;
}

bool Model::ExportOBJ(const std::string& path, std::string& err) const {
    if (meshRecs_.empty()) { err = "this model has no decodable mesh to export"; return false; }
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { err = "cannot open file for writing"; return false; }

    // sidecar .mtl (one material per mesh, so Blender separates the parts)
    std::string mtlPath = path; size_t dot = mtlPath.find_last_of('.');
    if (dot != std::string::npos) mtlPath = mtlPath.substr(0, dot);
    mtlPath += ".mtl";
    std::string mtlName = mtlPath; size_t sl = mtlName.find_last_of("/\\");
    if (sl != std::string::npos) mtlName = mtlName.substr(sl + 1);

    fprintf(f, "# DMC4 MOD mesh exported by DMCSEEDITOR (per-mesh, exact vertex order, UVs)\n");
    fprintf(f, "# Reimport with DMCSEEDITOR: edit vertex POSITIONS only, keep vertex count/order.\n");
    fprintf(f, "mtllib %s\n", mtlName.c_str());

    uint32_t base = 0;                       // running global vertex / UV index (OBJ is 1-based)
    int withUV = 0;
    for (size_t mi = 0; mi < meshRecs_.size(); ++mi) {
        const MeshRec& r = meshRecs_[mi];
        bool uv = r.uvOff >= 0;
        if (uv) ++withUV;
        fprintf(f, "o mesh_%02zu\nusemtl mesh_%02zu\n", mi, mi);
        for (uint32_t v = r.vstart; v < r.vmax; ++v) {
            size_t p = r.blockBase + (size_t)v * r.stride;
            if (p + 6 > raw_.size()) break;
            Vec3 q = DecodeVtx(raw_, p, r, mn_, scale_);
            fprintf(f, "v %.6f %.6f %.6f\n", q.x, q.y, q.z);
        }
        if (uv) {
            for (uint32_t v = r.vstart; v < r.vmax; ++v) {
                size_t p = r.blockBase + (size_t)v * r.stride + (size_t)r.uvOff;
                float u = 0, w = 0;
                if (p + 4 <= raw_.size()) {
                    uint16_t hu,hv; memcpy(&hu,&raw_[p],2); memcpy(&hv,&raw_[p+2],2);
                    u = Half2Float(hu); w = Half2Float(hv);
                }
                fprintf(f, "vt %.6f %.6f\n", u, 1.0f - w);   // OBJ V is bottom-up; game V is top-down
            }
        }
        for (uint32_t k = 0; k + 3 <= r.idxCount; k += 3) {
            size_t ip = r.faceByteOff + (size_t)k * 2;
            if (ip + 6 > raw_.size()) break;
            uint16_t a,c,d; memcpy(&a,&raw_[ip],2); memcpy(&c,&raw_[ip+2],2); memcpy(&d,&raw_[ip+4],2);
            if (a==0xFFFF||c==0xFFFF||d==0xFFFF) continue;
            if (a<r.vstart||a>=r.vmax||c<r.vstart||c>=r.vmax||d<r.vstart||d>=r.vmax) continue;
            uint32_t ia=base+(a-r.vstart)+1, ic=base+(c-r.vstart)+1, id=base+(d-r.vstart)+1;
            if (uv) fprintf(f, "f %u/%u %u/%u %u/%u\n", ia,ia, ic,ic, id,id);
            else    fprintf(f, "f %u %u %u\n", ia, ic, id);
        }
        base += (r.vmax - r.vstart);
    }
    fclose(f);

    FILE* mf = fopen(mtlPath.c_str(), "wb");
    if (mf) {
        fprintf(mf, "# DMCSEEDITOR materials (one per mesh)\n");
        for (size_t mi = 0; mi < meshRecs_.size(); ++mi)
            fprintf(mf, "newmtl mesh_%02zu\nKd 0.8 0.8 0.8\n", mi);
        fclose(mf);
    }
    (void)withUV;
    return true;
}

bool Model::ImportOBJ(const std::string& path, std::vector<uint8_t>& outMod, std::string& err,
                      int* changed, int* clamped) const {
    if (meshRecs_.empty() || raw_.empty()) { err = "no model loaded to reimport into"; return false; }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open OBJ"; return false; }
    std::vector<Vec3> verts;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] != 'v' || line[1] != ' ') continue;     // only "v x y z" (skip vt/vn/f/o)
        Vec3 q; if (sscanf(line + 2, "%f %f %f", &q.x, &q.y, &q.z) == 3) verts.push_back(q);
    }
    fclose(f);
    int total = meshVertTotal();
    if ((int)verts.size() != total) {
        char m[160]; snprintf(m, sizeof m, "vertex count mismatch: OBJ has %zu, model expects %d - keep the same vertex count/order (positions only)", verts.size(), total);
        err = m; return false;
    }
    // Start from the original bytes. Only REWRITE vertices the user actually moved (so
    // untouched verts -- and any mesh that decodes imperfectly -- keep their exact bytes,
    // and a no-op reimport is byte-identical). bbox is NOT changed; int16 edits outside the
    // model bounds are clamped (can't be represented) and counted.
    outMod = raw_;
    int nChanged = 0, nClamped = 0;
    size_t gi = 0;
    float invScale = scale_ > 1e-12f ? 1.0f/scale_ : 0.0f;
    for (const MeshRec& r : meshRecs_) {
        for (uint32_t v = r.vstart; v < r.vmax; ++v, ++gi) {
            size_t p = r.blockBase + (size_t)v * r.stride;
            if (p + (r.isFloat ? 12u : 6u) > outMod.size()) continue;
            Vec3 ref = DecodeVtx(raw_, p, r, mn_, scale_);
            const float q[3] = { verts[gi].x, verts[gi].y, verts[gi].z };
            const float rf[3] = { ref.x, ref.y, ref.z };
            bool moved = false;
            for (int a = 0; a < 3; ++a) {
                float eps = r.isFloat ? (fabsf(rf[a])*1e-5f + 1e-4f)
                                      : scale_;              // one int16 quant step
                if (fabsf(q[a]-rf[a]) > eps) moved = true;
            }
            if (!moved) continue;                            // keep original bytes exactly
            ++nChanged;
            if (r.isFloat) {
                float f0=q[0],f1=q[1],f2=q[2];
                memcpy(&outMod[p],&f0,4); memcpy(&outMod[p+4],&f1,4); memcpy(&outMod[p+8],&f2,4);
            } else {
                bool clampedHere = false;
                for (int a = 0; a < 3; ++a) {
                    float fu = (q[a]-mn_[a]) * invScale;
                    if (fu < 0) { fu = 0; clampedHere = true; }
                    if (fu > 65535.0f) { fu = 65535.0f; clampedHere = true; }
                    uint16_t u = (uint16_t)(fu + 0.5f);
                    memcpy(&outMod[p + a*2], &u, 2);
                }
                if (clampedHere) ++nClamped;
            }
        }
    }
    if (changed) *changed = nChanged;
    if (clamped) *clamped = nClamped;
    return true;
}

std::vector<Mat4> Model::ComposeWorld(const std::vector<Mat4>& locals) const {
    int n = (int)parents_.size();
    std::vector<Mat4> world(n);
    for (int i = 0; i < n; ++i) {
        const Mat4& L = (i < (int)locals.size()) ? locals[i] : local_[i];
        int p = parents_[i];
        world[i] = (p < 0 || p >= n) ? L : Mul(L, world[p]); // child * parent (row-vector)
    }
    return world;
}

std::vector<Vec3> Model::ComposeWorldPos(const std::vector<Mat4>& locals) const {
    std::vector<Mat4> world = ComposeWorld(locals);
    std::vector<Vec3> pos(world.size());
    for (size_t i = 0; i < world.size(); ++i) pos[i] = Translation(world[i]);
    return pos;
}

// affine inverse of a rotation+translation matrix (row-major, row-vector)
static Mat4 AffineInverse(const Mat4& m) {
    Mat4 r = Identity();
    // transpose upper 3x3 (assumes orthonormal rotation)
    r.m[0]=m.m[0]; r.m[1]=m.m[4]; r.m[2]=m.m[8];
    r.m[4]=m.m[1]; r.m[5]=m.m[5]; r.m[6]=m.m[9];
    r.m[8]=m.m[2]; r.m[9]=m.m[6]; r.m[10]=m.m[10];
    float tx=m.m[12], ty=m.m[13], tz=m.m[14];
    r.m[12] = -(tx*r.m[0]+ty*r.m[4]+tz*r.m[8]);
    r.m[13] = -(tx*r.m[1]+ty*r.m[5]+tz*r.m[9]);
    r.m[14] = -(tx*r.m[2]+ty*r.m[6]+tz*r.m[10]);
    return r;
}
static Vec3 XformPoint(const Vec3& p, const Mat4& m) {
    return { p.x*m.m[0]+p.y*m.m[4]+p.z*m.m[8]+m.m[12],
             p.x*m.m[1]+p.y*m.m[5]+p.z*m.m[9]+m.m[13],
             p.x*m.m[2]+p.y*m.m[6]+p.z*m.m[10]+m.m[14] };
}

std::vector<Vec3> Model::SkinTris(const std::vector<Mat4>& animLocals) const {
    int nb = (int)parents_.size();
    std::vector<Mat4> animWorld = ComposeWorld(animLocals);
    // skin[b] = invBind[b] * animWorld[b]  (model -> bone-local -> animated world)
    std::vector<Mat4> skin(nb);
    for (int b = 0; b < nb; ++b) skin[b] = Mul(invBind_[b], animWorld[b]);
    std::vector<Vec3> out(tris_.size());
    for (size_t i = 0; i < tris_.size(); ++i) {
        int b = (i < triBone_.size()) ? triBone_[i] : -1;
        out[i] = (b >= 0 && b < nb) ? XformPoint(tris_[i], skin[b]) : tris_[i];
    }
    return out;
}

} // namespace mdl
