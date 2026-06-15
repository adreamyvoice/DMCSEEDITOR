#include "viewport.h"
#include <cmath>

namespace view {

static IDirect3DTexture9* g_rt   = nullptr;
static IDirect3DSurface9* g_rtS  = nullptr;
static IDirect3DSurface9* g_ds   = nullptr;
static int g_w = 0, g_h = 0;

struct Vtx { float x, y, z; D3DCOLOR c; };
static const DWORD FVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;

static IDirect3DTexture9* g_prev = nullptr;
static int g_pw = 0, g_ph = 0;

void OnDeviceLost() {
    if (g_rtS) { g_rtS->Release(); g_rtS = nullptr; }
    if (g_rt)  { g_rt->Release();  g_rt  = nullptr; }
    if (g_ds)  { g_ds->Release();  g_ds  = nullptr; }
    if (g_prev){ g_prev->Release(); g_prev = nullptr; }
    g_w = g_h = 0; g_pw = g_ph = 0;
}
void Shutdown() { OnDeviceLost(); }

ImTextureID MakePreview(IDirect3DDevice9* dev, const unsigned char* rgba, int w, int h) {
    if (w < 1 || h < 1) return (ImTextureID)0;
    if (!g_prev || w != g_pw || h != g_ph) {
        if (g_prev) { g_prev->Release(); g_prev = nullptr; }
        if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_prev, nullptr)))
            return (ImTextureID)0;
        g_pw = w; g_ph = h;
    }
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(g_prev->LockRect(0, &lr, nullptr, 0))) {
        for (int y = 0; y < h; ++y) {
            uint8_t* dst = (uint8_t*)lr.pBits + y * lr.Pitch;
            const uint8_t* src = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) { // RGBA -> BGRA (D3DFMT_A8R8G8B8)
                dst[x*4+0] = src[x*4+2]; dst[x*4+1] = src[x*4+1];
                dst[x*4+2] = src[x*4+0]; dst[x*4+3] = src[x*4+3];
            }
        }
        g_prev->UnlockRect(0);
    }
    return (ImTextureID)g_prev;
}

static bool ensureTarget(IDirect3DDevice9* dev, int w, int h) {
    if (w < 16) w = 16; if (h < 16) h = 16;
    if (g_rt && w == g_w && h == g_h) return true;
    OnDeviceLost();
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8,
                                  D3DPOOL_DEFAULT, &g_rt, nullptr))) return false;
    g_rt->GetSurfaceLevel(0, &g_rtS);
    if (FAILED(dev->CreateDepthStencilSurface(w, h, D3DFMT_D16, D3DMULTISAMPLE_NONE, 0,
                                              TRUE, &g_ds, nullptr))) { OnDeviceLost(); return false; }
    g_w = w; g_h = h;
    return true;
}

// --- hand-built left-handed matrices (row-major, row-vector) ---
struct V3 { float x, y, z; };
static V3 sub(V3 a, V3 b){ return {a.x-b.x,a.y-b.y,a.z-b.z}; }
static V3 cross(V3 a, V3 b){ return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x}; }
static float dot(V3 a, V3 b){ return a.x*b.x+a.y*b.y+a.z*b.z; }
static V3 norm(V3 a){ float l=std::sqrt(dot(a,a)); if(l<1e-6f)l=1; return {a.x/l,a.y/l,a.z/l}; }

static void lookAtLH(D3DMATRIX& m, V3 eye, V3 at, V3 up) {
    V3 z = norm(sub(at, eye));
    V3 x = norm(cross(up, z));
    V3 y = cross(z, x);
    m._11=x.x; m._12=y.x; m._13=z.x; m._14=0;
    m._21=x.y; m._22=y.y; m._23=z.y; m._24=0;
    m._31=x.z; m._32=y.z; m._33=z.z; m._34=0;
    m._41=-dot(x,eye); m._42=-dot(y,eye); m._43=-dot(z,eye); m._44=1;
}
static void perspLH(D3DMATRIX& m, float fovY, float aspect, float zn, float zf) {
    float ys = 1.0f / std::tan(fovY * 0.5f);
    float xs = ys / aspect;
    memset(&m, 0, sizeof m);
    m._11 = xs; m._22 = ys;
    m._33 = zf / (zf - zn); m._34 = 1.0f;
    m._43 = -zn * zf / (zf - zn);
}

ImTextureID Render(IDirect3DDevice9* dev, int w, int h,
                   const std::vector<mdl::Vec3>& bonePos,
                   const std::vector<int>& parents,
                   const Camera& cam, int highlightBone,
                   const std::vector<mdl::Vec3>* cloud, bool showSkeleton,
                   const std::vector<mdl::Vec3>* solidTris) {
    if (!ensureTarget(dev, w, h)) return (ImTextureID)0;

    IDirect3DSurface9* prevRT = nullptr; IDirect3DSurface9* prevDS = nullptr;
    dev->GetRenderTarget(0, &prevRT);
    dev->GetDepthStencilSurface(&prevDS);
    dev->SetRenderTarget(0, g_rtS);
    dev->SetDepthStencilSurface(g_ds);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(16, 14, 18), 1.0f, 0);

    // camera
    float cp = std::cos(cam.pitch), sp = std::sin(cam.pitch);
    float cy = std::cos(cam.yaw),   sy = std::sin(cam.yaw);
    V3 tgt { cam.target.x, cam.target.y, cam.target.z };
    V3 eye { tgt.x + cam.dist * cp * sy, tgt.y + cam.dist * sp, tgt.z + cam.dist * cp * cy };
    D3DMATRIX view, proj, world;
    lookAtLH(view, eye, tgt, V3{0,1,0});
    perspLH(proj, 1.0f, g_h ? (float)g_w / g_h : 1.0f, 1.0f, 5000.0f);
    memset(&world, 0, sizeof world); world._11=world._22=world._33=world._44=1;
    dev->SetTransform(D3DTS_WORLD, &world);
    dev->SetTransform(D3DTS_VIEW, &view);
    dev->SetTransform(D3DTS_PROJECTION, &proj);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetFVF(FVF);

    std::vector<Vtx> lines;

    // ground grid (XZ plane at y=0)
    const D3DCOLOR grid = D3DCOLOR_XRGB(55, 40, 45);
    int ext = 200, step = 25;
    for (int g = -ext; g <= ext; g += step) {
        lines.push_back({(float)-ext,0,(float)g,grid}); lines.push_back({(float)ext,0,(float)g,grid});
        lines.push_back({(float)g,0,(float)-ext,grid}); lines.push_back({(float)g,0,(float)ext,grid});
    }

    // bones: segment from each bone to its parent
    const D3DCOLOR bone = D3DCOLOR_XRGB(220, 40, 50);
    const D3DCOLOR hot  = D3DCOLOR_XRGB(255, 230, 80);
    if (showSkeleton) {
        for (size_t i = 0; i < bonePos.size(); ++i) {
            int p = (i < parents.size()) ? parents[i] : -1;
            if (p < 0 || p >= (int)bonePos.size()) continue;
            D3DCOLOR col = ((int)i == highlightBone || p == highlightBone) ? hot : bone;
            lines.push_back({bonePos[i].x, bonePos[i].y, bonePos[i].z, col});
            lines.push_back({bonePos[p].x, bonePos[p].y, bonePos[p].z, col});
        }
    }

    if (dev->BeginScene() >= 0) {
        // solid mesh: flat-shaded triangles (face normal . light)
        if (solidTris && solidTris->size() >= 3) {
            const std::vector<mdl::Vec3>& t = *solidTris;
            std::vector<Vtx> sv; sv.reserve(t.size());
            V3 L = norm(V3{0.4f, 0.7f, 0.5f});   // light direction
            for (size_t i = 0; i + 2 < t.size(); i += 3) {
                V3 a{t[i].x,t[i].y,t[i].z}, b{t[i+1].x,t[i+1].y,t[i+1].z}, c{t[i+2].x,t[i+2].y,t[i+2].z};
                V3 nrm = norm(cross(sub(b,a), sub(c,a)));
                float d = nrm.x*L.x + nrm.y*L.y + nrm.z*L.z; if (d < 0) d = -d; // two-sided
                int sh = 40 + (int)(d * 175.0f); if (sh > 235) sh = 235;
                D3DCOLOR col = D3DCOLOR_XRGB(sh, sh, (int)(sh*0.96f));
                sv.push_back({a.x,a.y,a.z,col}); sv.push_back({b.x,b.y,b.z,col}); sv.push_back({c.x,c.y,c.z,col});
            }
            dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(sv.size()/3), sv.data(), sizeof(Vtx));
        }
        if (!lines.empty())
            dev->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(lines.size() / 2), lines.data(), sizeof(Vtx));
        // mesh vertex point cloud
        if (cloud && !cloud->empty()) {
            std::vector<Vtx> pc; pc.reserve(cloud->size());
            const D3DCOLOR cc = D3DCOLOR_XRGB(190, 190, 200);
            for (const mdl::Vec3& v : *cloud) pc.push_back({v.x, v.y, v.z, cc});
            float ps = 2.0f; DWORD psd; memcpy(&psd, &ps, 4);
            dev->SetRenderState(D3DRS_POINTSIZE, psd);
            dev->DrawPrimitiveUP(D3DPT_POINTLIST, (UINT)pc.size(), pc.data(), sizeof(Vtx));
        }
        // joints as small crosses (skeleton only)
        if (showSkeleton) {
            std::vector<Vtx> j;
            const D3DCOLOR jc = D3DCOLOR_XRGB(255, 120, 130);
            float s = 1.6f;
            for (size_t i = 0; i < bonePos.size(); ++i) {
                const mdl::Vec3& b = bonePos[i];
                j.push_back({b.x-s,b.y,b.z,jc}); j.push_back({b.x+s,b.y,b.z,jc});
                j.push_back({b.x,b.y-s,b.z,jc}); j.push_back({b.x,b.y+s,b.z,jc});
                j.push_back({b.x,b.y,b.z-s,jc}); j.push_back({b.x,b.y,b.z+s,jc});
            }
            if (!j.empty())
                dev->DrawPrimitiveUP(D3DPT_LINELIST, (UINT)(j.size() / 2), j.data(), sizeof(Vtx));
        }
        dev->EndScene();
    }

    dev->SetRenderTarget(0, prevRT);
    dev->SetDepthStencilSurface(prevDS);
    if (prevRT) prevRT->Release();
    if (prevDS) prevDS->Release();
    return (ImTextureID)g_rt;
}

} // namespace view
