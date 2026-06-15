// model.h — MT Framework ".mod" skeleton parser (DMC4, MOD version 0xD2).
// We only need the skeleton for the 3D viewer, NOT the mesh: bone hierarchy + local
// bind matrices. Verified against model\game\pl030\pl030 (Vergil body, 74 bones).
//
// MOD header (verified):
//   0x00 "MOD\0"; 0x04 u16 version(0xD2); 0x06 u16 numBones; 0x08 u16 numMeshes;
//   0x0A u16 numMaterials; ...; 0x24 u32 bonesOffset.
// Bone section @bonesOffset: numBones x { u8 func; u8 parent(0xFF=root); u8 sym; u8 pad;
//   f32 length; f32 x,y,z }  (24 bytes); then numBones x 4x4 local matrices (row-major,
//   translation in row 3); then numBones x 4x4 inverse-bind matrices.
// World bind pos = compose local matrices down the parent chain (row-vector convention:
//   world[i] = local[i] * world[parent[i]]).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mdl {

struct Mat4 { float m[16]; };           // row-major
struct Vec3 { float x, y, z; };

Mat4 Identity();
Mat4 Mul(const Mat4& a, const Mat4& b);  // a*b (row-vector: applies a then b)

class Model {
public:
    bool Parse(const std::vector<uint8_t>& data, std::string& err);
    bool valid() const { return valid_; }
    int  numBones() const { return (int)parents_.size(); }
    const std::vector<int>&  parents() const { return parents_; }
    // LMT tracks address bones by their hierarchy ID (not array index). Map ID->index.
    int  boneIndexForId(int id) const {
        return (id >= 0 && id < (int)idToIndex_.size()) ? idToIndex_[id] : -1;
    }
    const std::vector<Mat4>& localBind() const { return local_; }

    // World bind positions (one per bone), for the static skeleton.
    const std::vector<Vec3>& bindWorldPos() const { return bindPos_; }

    // Mesh vertex point cloud (decoded positions), for the solid/point model view.
    const std::vector<Vec3>& meshCloud() const { return cloud_; }
    // Solid triangles: 3 consecutive Vec3 = one triangle (bind pose).
    const std::vector<Vec3>& meshTris() const { return tris_; }

    // Export the mesh to a Wavefront .obj for Blender, per-mesh (`o mesh_NN`) in EXACT
    // MOD vertex order (no welding) so an edited OBJ can be reimported 1:1. False if no mesh.
    bool ExportOBJ(const std::string& path, std::string& err) const;
    // Reimport an edited OBJ: rewrites each MOD vertex POSITION (topology must be unchanged
    // -- same vertex count/order). Returns the new MOD bytes in outMod. Expands + rewrites
    // the model bbox if any vertex moved outside it (re-quantising every vertex to suit).
    bool ImportOBJ(const std::string& path, std::vector<uint8_t>& outMod, std::string& err,
                   int* changed = nullptr, int* clamped = nullptr) const;
    // Total mesh vertices in faithful-export order (sum of each mesh's [vstart,vmax)).
    int meshVertTotal() const;

    // Bind-pose world matrices (one per bone).
    const std::vector<Mat4>& bindWorld() const { return bindWorld_; }
    // Deform the triangles for a given set of animated LOCAL matrices (rigid nearest-bone
    // skinning): each tri vertex follows its nearest bone. Returns deformed tri verts.
    std::vector<Vec3> SkinTris(const std::vector<Mat4>& animLocals) const;

    // Compose world positions given a set of per-bone LOCAL matrices (used for animation:
    // pass animated locals to get the posed skeleton). Falls back to bind local when a
    // bone has no animated local supplied.
    std::vector<Vec3> ComposeWorldPos(const std::vector<Mat4>& locals) const;

    // Per-mesh record kept for faithful export + position reimport.
    struct MeshRec {
        uint32_t blockBase = 0;   // byte offset of the mesh's vertex block (vertOff+voff)
        uint8_t  stride = 0;
        uint32_t vstart = 0, vmax = 0;  // this mesh owns verts [vstart, vmax)
        bool     isFloat = false; // position is 3xfloat32 (else 3xint16 dequantised via bbox)
        uint32_t faceByteOff = 0; // byte offset of this mesh's u16 index list
        uint32_t idxCount = 0;
        int      uvOff = -1;      // byte offset of the 2x float16 UV within the vertex (-1 = none)
    };

private:
    bool valid_ = false;
    std::vector<int>  parents_;
    std::vector<Mat4> local_;     // bind local matrices
    std::vector<Vec3> bindPos_;   // composed bind world positions
    std::vector<Vec3> cloud_;     // decoded mesh vertex positions
    std::vector<Vec3> tris_;      // solid triangles (3 Vec3 each)
    std::vector<Mat4> bindWorld_; // bind-pose world matrix per bone
    std::vector<Mat4> invBind_;   // inverse of bindWorld_ (world -> bone)
    std::vector<int>  triBone_;   // nearest bone per triangle vertex
    std::vector<int>  idToIndex_; // bone hierarchy ID -> array index (256 entries)
    std::vector<uint8_t> raw_;    // original MOD bytes (for export/reimport)
    std::vector<MeshRec> meshRecs_;
    float mn_[3] = {0,0,0}, mx_[3] = {0,0,0};  // model bbox (position dequant range)
    float scale_ = 1.0f;   // int16 position quant scale: pos[a] = mn_[a] + u[a]*scale_
                           // (uniform; derived per-model from the actual max int16, NOT 65535)

    std::vector<Mat4> ComposeWorld(const std::vector<Mat4>& locals) const; // full matrices
};

} // namespace mdl
