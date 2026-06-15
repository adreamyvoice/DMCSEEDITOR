// viewport.h — a small DX9 render-to-texture 3D viewport for the skeleton viewer.
// Renders bone segments + a ground grid into an offscreen texture which the UI shows
// via ImGui::Image. No D3DX dependency (matrices built by hand). Left-handed, Y-up.
#pragma once
#include <d3d9.h>
#include <vector>
#include "model.h"
#include "imgui.h"

namespace view {

struct Camera {
    float yaw   = 0.7f;   // radians, orbit around target
    float pitch = 0.15f;
    float dist  = 320.0f;
    mdl::Vec3 target { 0, 90, 0 };
};

// Lifecycle tied to the D3D device (RT texture lives in DEFAULT pool).
void OnDeviceLost();                 // release RT before a device Reset
void Shutdown();

// Upload RGBA8 pixels to a reusable preview texture; returns it as an ImGui texture id.
ImTextureID MakePreview(IDirect3DDevice9* dev, const unsigned char* rgba, int w, int h);

// Render the skeleton (bone world positions + parent indices) to the offscreen target
// and return it as an ImGui texture id. `w`,`h` is the desired viewport pixel size.
ImTextureID Render(IDirect3DDevice9* dev, int w, int h,
                   const std::vector<mdl::Vec3>& bonePos,
                   const std::vector<int>& parents,
                   const Camera& cam, int highlightBone = -1,
                   const std::vector<mdl::Vec3>* cloud = nullptr,
                   bool showSkeleton = true,
                   const std::vector<mdl::Vec3>* solidTris = nullptr);

} // namespace view
