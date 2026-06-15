// main.cpp — RGBlue (English) : a black/red DMC effect-file editor.
// Clean-room reimplementation of the RGBlue16 E-language tool. Win32 + DirectX9 +
// Dear ImGui, built as a Windows exe to run under CrossOver/Wine. English throughout.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <d3d9.h>
#include <string>
#include <vector>
#include <fstream>
#include <cstring>
#include <cmath>
#include <set>
#include <filesystem>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"

#include "theme.h"
#include "effect_file.h"
#include "color_transforms.h"
#include "arc.h"
#include "pac.h"
#include "sdl.h"
#include "lmt.h"
#include "model.h"
#include "viewport.h"
#include "lmt_anim.h"
#include "tex.h"
#include "savedit.h"

// ---- D3D9 boilerplate (from the ImGui example) ------------------------------
static LPDIRECT3D9          g_pD3D = nullptr;
static LPDIRECT3DDEVICE9    g_pd3dDevice = nullptr;
static bool                 g_DeviceLost = false;
static UINT                 g_ResizeWidth = 0, g_ResizeHeight = 0;
static D3DPRESENT_PARAMETERS g_d3dpp = {};

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static bool CreateDeviceD3D(HWND hWnd) {
    if ((g_pD3D = Direct3DCreate9(D3D_SDK_VERSION)) == nullptr) return false;
    ZeroMemory(&g_d3dpp, sizeof(g_d3dpp));
    g_d3dpp.Windowed = TRUE;
    g_d3dpp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval = D3DPRESENT_INTERVAL_ONE; // vsync
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) < 0)
        return false;
    return true;
}
static void CleanupDeviceD3D() {
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D) { g_pD3D->Release(); g_pD3D = nullptr; }
}
static void ResetDevice() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    view::OnDeviceLost();   // release the viewport's DEFAULT-pool render target
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    // Under Wine/d3dmetal a resize can momentarily refuse the reset; don't assert/blast
    // ahead with a half-reset device (that's the resize "glitch") — retry next frame.
    if (hr == D3DERR_DEVICELOST || hr == D3DERR_INVALIDCALL) { g_DeviceLost = true; return; }
    ImGui_ImplDX9_CreateDeviceObjects();
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_ResizeWidth = (UINT)LOWORD(lParam);
            g_ResizeHeight = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---- app state --------------------------------------------------------------
static efx::EffectFile           g_file;     // current .efl being edited
static arc::ArcFile              g_arc;      // current .arc archive (if any)
static pac::PacFile             g_pac;      // current .pac/PNST container (DMC3 HD)
static sav::SaveFile            g_save;     // current Steam save being edited (Save Editor tab)
static int                      g_saveSlot = 0;
static std::string             g_sdlText;  // decoded outline of the current .sdl scene/stage def
static std::string             g_sdlName;
static lmt::LmtFile              g_lmt;      // parsed motion list (if any)
static lmt::Bytes                g_lmtBuf;   // backing bytes for the LMT
static int   g_srcEntry  = -1;   // arc entry index the efl/lmt came from (-1 = loose file)
static bool  g_lmtDirty  = false; // a motion remap/swap/field edit happened
enum ActiveKind { AK_None, AK_Efl, AK_Lmt };
static ActiveKind g_active = AK_None;
static char  g_status[512] = "Open a .arc archive, or a loose .efl / .lmt file.";
static bool  g_keepBackup = true;
static int   g_hexScroll  = 0;
// 3D viewer state
static mdl::Model   g_model;      // loaded skeleton (from a .mod entry)
static bool         g_modelOK = false;
static std::string  g_modelName;
static int          g_modelEntry = -1;   // arc entry the MOD came from (-1 = loose), for reimport write-back
static view::Camera g_cam;
static int          g_motionSel = -1;   // which LMT motion slot to preview (-1 = bind pose)
static bool         g_playing = false;
static float        g_frame = 0.0f;
static float        g_fps = 30.0f;
// texture recolor state
static int          g_texEntry = -1;          // arc entry index of the loaded texture
static efx::Bytes   g_texOrig;                // original .tex bytes
static tex::TexInfo g_texInfo;
static tex::Recolor g_texRC;
static std::string  g_texName;
static bool         g_texDirtyPreview = true;
static int          g_texGen = 0;             // bumped each time a new texture loads
static bool         g_texChanged = false;     // recolor differs from the loaded original
static std::set<int> g_texSel;                // arc entries checked for batch recolor
static std::string  g_gameRoot;               // "…/Special Edition/" derived from an opened arc

// click-to-paint brush state (DMC3 .pac textures)
static float        g_brushColor[3] = {1.0f, 0.0f, 0.0f};   // shared with the colour wheel
static bool         g_paintMode = false;
static std::vector<uint8_t> g_paintRGBA;      // full-res mip0 RGBA of the focused texture
static int          g_paintW = 0, g_paintH = 0, g_paintFor = -1;
static float        g_brushRadius = 10.0f, g_brushStrength = 1.0f;
static bool         g_brushSoft = true;
static bool         g_paintDirty = true;      // working buffer changed -> re-upload preview

// Active raw buffer for the Hex Inspector (efl edits g_file; lmt edits g_lmtBuf).
static const efx::Bytes& ActiveBuf() {
    static efx::Bytes empty;
    if (g_active == AK_Efl) return g_file.raw();
    if (g_active == AK_Lmt) return g_lmtBuf;
    return empty;
}

static std::string OpenFileDialog() {
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "Effect files\0*.*\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrTitle = "Open DMC effect file";
    if (GetOpenFileNameA(&ofn)) return path;
    return "";
}

// Generic open/save dialogs for asset round-trips (DDS textures, OBJ meshes).
static std::string OpenFileDialogF(const char* filter, const char* title) {
    char path[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn); ofn.lpstrFilter = filter;
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST; ofn.lpstrTitle = title;
    if (GetOpenFileNameA(&ofn)) return path;
    return "";
}
static std::string SaveFileDialogF(const char* filter, const char* defExt, const char* defName) {
    char path[MAX_PATH] = {0};
    if (defName) { strncpy(path, defName, MAX_PATH - 1); path[MAX_PATH-1] = 0; }
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn); ofn.lpstrFilter = filter;
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH; ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST; ofn.lpstrTitle = "Export";
    if (GetSaveFileNameA(&ofn)) return path;
    return "";
}
// Turn an arc entry path ("scr\\st705\\...") into a safe loose filename stem.
static std::string SafeStem(const std::string& n) {
    std::string s = n; for (char& c : s) if (c=='\\' || c=='/' || c==':') c = '_'; return s;
}
static bool ReadFileBytes(const std::string& path, efx::Bytes& out) {
    std::ifstream in(path, std::ios::binary); if (!in) return false;
    in.seekg(0, std::ios::end); std::streamoff n = in.tellg(); in.seekg(0);
    if (n <= 0) return false; out.resize((size_t)n);
    in.read((char*)out.data(), n); return (bool)in;
}
static bool WriteFileBytes(const std::string& path, const efx::Bytes& data) {
    std::ofstream o(path, std::ios::binary); if (!o) return false;
    o.write((const char*)data.data(), (std::streamsize)data.size()); return (bool)o;
}

// ---- UI: tabs (real DMC4 .efl format) ---------------------------------------
static const char* RefKind(efx::PathRef::Kind k) {
    switch (k) { case efx::PathRef::Texture: return "texture";
                 case efx::PathRef::Animation: return "anim";
                 default: return "other"; }
}

// Stage whatever is currently being edited back into the open archive (without writing
// to disk) so edits accumulate. Called when switching entries and before Save Archive.
static void StageCurrentEdits() {
    if (!g_arc.loaded()) return;
    std::string err;
    if (g_texChanged && g_texEntry >= 0 && g_texInfo.ok) {
        efx::Bytes out = g_texOrig; tex::Apply(out, g_texInfo, g_texRC);
        g_arc.Replace((size_t)g_texEntry, out, err);
    }
    if (g_active == AK_Efl && g_srcEntry >= 0 && g_file.dirty())
        g_arc.Replace((size_t)g_srcEntry, g_file.raw(), err);
    if (g_active == AK_Lmt && g_srcEntry >= 0 && g_lmtDirty)
        g_arc.Replace((size_t)g_srcEntry, g_lmtBuf, err);
}

static int PendingEdits() {
    if (!g_arc.loaded()) return 0;
    int n = 0; for (const arc::Entry& e : g_arc.entries()) if (e.replaced) ++n;
    return n;
}
static void SaveAll() {
    StageCurrentEdits();
    std::string err;
    if (g_arc.loaded() && g_arc.Save(g_arc.path(), err))
        snprintf(g_status, sizeof g_status, "Saved all %d change(s) to %s", PendingEdits(), g_arc.path().c_str());
    else snprintf(g_status, sizeof g_status, "Error: %s", err.empty() ? "no archive open" : err.c_str());
}
static bool HasUnstagedEdit() {
    return (g_texChanged && g_texEntry >= 0) ||
           (g_active == AK_Efl && g_srcEntry >= 0 && g_file.dirty()) ||
           (g_active == AK_Lmt && g_srcEntry >= 0 && g_lmtDirty);
}
// A reusable "save everything" button; shows the pending-edit count (no per-frame staging).
static void SaveAllButton() {
    int pend = PendingEdits() + (HasUnstagedEdit() ? 1 : 0);
    if (pend == 0) ImGui::BeginDisabled();
    char lbl[64]; snprintf(lbl, sizeof lbl, "Save ALL changes (%d)", pend);
    if (ImGui::Button(lbl)) SaveAll();   // stages current + writes everything
    if (pend == 0) ImGui::EndDisabled();
}

// Load an extracted/parsed buffer into the right editor based on its magic.
static void AdoptBuffer(const efx::Bytes& data, const std::string& name, int srcEntry) {
    StageCurrentEdits();   // keep edits from the previously-open entry
    std::string err;
    if (data.size() >= 4 && memcmp(data.data(), "TEX\0", 4) == 0) {  // texture for recoloring
        tex::TexInfo ti = tex::Parse(data);
        if (ti.ok && ti.recolorable()) {
            g_texOrig = data; g_texInfo = ti; g_texName = name; g_texRC = tex::Recolor();
            g_texEntry = srcEntry; g_texDirtyPreview = true; g_texGen++; g_texChanged = false;
            const char* fn = ti.isBC1() ? "BC1" : ti.isBC2() ? "BC2" : "BC3";
            snprintf(g_status, sizeof g_status, "Texture %s: %dx%d %s (Textures tab)", name.c_str(), ti.width, ti.height, fn);
        } else
            snprintf(g_status, sizeof g_status, "%s is format 0x%X (normal/mask map) - recolor only works on _BM (BC1) / _MM (BC3) color maps", name.c_str(), ti.format);
        return;
    }
    if (sdl::IsSDL(data)) {                                          // scene / stage definition
        g_sdlText = sdl::ToText(data); g_sdlName = name;
        snprintf(g_status, sizeof g_status, "SDL scene %s (open the SDL tab)", name.c_str());
        return;
    }
    if (data.size() >= 4 && memcmp(data.data(), "MOD\0", 4) == 0) {  // skeleton for the 3D viewer
        if (g_model.Parse(data, err)) { g_modelOK = true; g_modelName = name; g_modelEntry = srcEntry;
            // auto-frame: center the camera on the skeleton's centroid and fit its height
            const auto& bp = g_model.bindWorldPos();
            mdl::Vec3 lo{1e9f,1e9f,1e9f}, hi{-1e9f,-1e9f,-1e9f};
            for (auto& p : bp) {
                lo.x = p.x<lo.x?p.x:lo.x; lo.y = p.y<lo.y?p.y:lo.y; lo.z = p.z<lo.z?p.z:lo.z;
                hi.x = p.x>hi.x?p.x:hi.x; hi.y = p.y>hi.y?p.y:hi.y; hi.z = p.z>hi.z?p.z:hi.z;
            }
            g_cam.target = { (lo.x+hi.x)*0.5f, (lo.y+hi.y)*0.5f, (lo.z+hi.z)*0.5f };
            float ext = hi.y - lo.y; if (ext < 50) ext = 50;
            g_cam.dist = ext * 1.6f;
            g_cam.yaw = 0.4f; g_cam.pitch = 0.05f;
            snprintf(g_status, sizeof g_status, "Skeleton %s: %d bones (open the 3D View tab)", name.c_str(), g_model.numBones()); }
        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
        return;
    }
    if (data.size() >= 4 && memcmp(data.data(), "LMT\0", 4) == 0) {
        g_lmtBuf = data;
        g_lmtDirty = false;
        if (g_lmt.Parse(g_lmtBuf, err, /*fresh=*/true)) { g_active = AK_Lmt; g_srcEntry = srcEntry;
            snprintf(g_status, sizeof g_status, "Motion %s: LMT v%d, %d live motions",
                     name.c_str(), g_lmt.version(), g_lmt.liveCount()); }
        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
    } else { // treat as efl / generic
        if (g_file.LoadFromMemory(data, name, err)) { g_active = AK_Efl; g_srcEntry = srcEntry;
            snprintf(g_status, sizeof g_status, "%s: %s, %zu refs", name.c_str(),
                     g_file.isEFL() ? "EFL" : "non-EFL", g_file.pathRefs().size()); }
        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
    }
}

// Open a file from disk: sniff magic -> arc / efl / lmt / generic.
static void OpenPath(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { snprintf(g_status, sizeof g_status, "Error: cannot open %s", p.c_str()); return; }
    efx::Bytes data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() >= 4 && memcmp(data.data(), "ARC\0", 4) == 0) {
        std::string err;
        if (g_arc.Load(p, err)) { g_active = AK_None; g_srcEntry = -1; g_texSel.clear(); g_texEntry = -1;
            // remember the game root (…/Special Edition/) so the Mods tab can find MODS/
            size_t nd = p.find("nativeDX10");
            if (nd != std::string::npos) g_gameRoot = p.substr(0, nd);
            snprintf(g_status, sizeof g_status, "Archive %s: %zu entries", p.c_str(), g_arc.entries().size()); }
        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
        return;
    }
    // DMC3 HD .pac / PNST container -> PAC browser tab
    if (pac::IsContainer(data)) {
        std::string err;
        if (g_pac.Load(p, err))
            snprintf(g_status, sizeof g_status, "%s container %s: %zu entries (open the PAC tab)",
                     g_pac.magic().c_str(), p.c_str(), g_pac.entries().size());
        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
        return;
    }
    // a loose file replaces any open archive context
    size_t slash = p.find_last_of("/\\");
    AdoptBuffer(data, slash == std::string::npos ? p : p.substr(slash + 1), -1);
}

// ---- File identification (infernalwarks FILE-ID thread + Sectus ARCTool tutorial) ----
// Plain-English description of an arc entry so the archive is browsable for mod-making.
// Character is the pl### / wp### prefix; part comes from the name; type from the ext().
static std::string IdCharacter(const std::string& n) {
    auto has = [&](const char* s){ return n.find(s) != std::string::npos; };
    if (has("pl000")) return "Nero";
    if (has("pl006")) return "Dante";
    if (has("pl007")) return "Trish";
    if (has("pl008")) return "Lady";
    if (has("pl024")) return "Tutorial Nero";
    if (has("pl030")) return "Vergil";
    if (has("pl032")) return "Vergil DT";
    if (has("pl033")) return "Vergil Sparda";
    if (has("pl036")) return "Vergil alt";
    if (has("wp042")) return "Yamato";
    if (has("wp043")) return "Force Edge";
    if (has("wp044") || has("beowulf")) return "Beowulf";
    if (has("wp045") || has("geneiken")) return "Phantom Sword";
    return "";
}
// _ex00 = alternate costume, _ex01 = recolor (per tutorial).
static std::string FriendlyName(const std::string& name, const std::string& ext) {
    std::string lower = name; for (auto& c : lower) c = (char)tolower((unsigned char)c);
    auto find = [&](const char* s){ return lower.find(s) != std::string::npos; };
    std::string who = IdCharacter(lower);
    const char* part = "";
    static const struct { const char* key; const char* label; } kParts[] = {
        {"coatlining","coat lining"},{"coatbutton","coat button"},{"coat","coat"},
        {"body","body"},{"arm","arm/sleeve"},{"hand","glove"},{"glove","glove"},
        {"face","FACE (keep)"},{"skin","skin"},{"hair","hair"},{"head","head"},
        {"boots","boots"},{"shoes","shoes"},{"pants","pants"},{"scarf","scarf"},
        {"komono","accessory"},{"wing","wing"},{"hakkou","DT glow"},{"lace","lace"},
        {"button","button"},{"skull","skull"},{"accessor","accessory"},{"eff","effect mesh"},
    };
    for (auto& p : kParts) if (find(p.key)) { part = p.label; break; }
    std::string type;
    if (ext == "mod") type = "MODEL";
    else if (ext == "sdl") type = "scene / stage definition (SDL)";
    else if (ext == "mrl") type = "material (texture refs)";
    else if (ext == "tex") {
        if (find("_bm")) type = "texture: COLOUR/diffuse";
        else if (find("_nm")) type = "texture: normal map";
        else if (find("_mm")) type = "texture: mask/multiplier";
        else type = "texture";
    }
    else if (ext == "lmt") type = "motion / animation";
    else if (ext == "efl") type = "effect (particles+colour)";
    else if (ext.size() == 8 &&
             ext.find_first_not_of("0123456789ABCDEF") == std::string::npos)
        type = "aux model data";   // the "bunch of numbers" files (hash-named)
    else type = ext;
    std::string out;
    if (!who.empty()) out += who + " ";
    if (part[0])      { out += part; out += ' '; }
    out += type;
    if (find("_ex00")) out += "  [alt costume]";
    else if (find("_ex01")) out += "  [recolor]";
    return out;
}
// Repack order for the ARCTool .txt (per tutorial vs1 edit): textures, then .mrl, then .mod.
// Lower = earlier. Everything else keeps original order.
static int RepackRank(const std::string& ext) {
    if (ext == "tex") return 0;
    if (ext == "mrl") return 1;
    if (ext == "mod") return 2;
    return 3;
}

static void DrawSdlTab() {
    ImGui::TextColored(theme::kRedHot, "Scene / stage definition: %s", g_sdlName.c_str());
    ImGui::TextDisabled("SDL = the stage scene graph (cameras, fog, orb/item placement). Decoded outline below.");
    if (ImGui::Button("Export outline as .txt")) {
        std::string p = SaveFileDialogF("Text\0*.txt\0All files\0*.*\0", "txt", (SafeStem(g_sdlName)+"_sdl.txt").c_str());
        if (!p.empty()) {
            efx::Bytes t(g_sdlText.begin(), g_sdlText.end());
            if (WriteFileBytes(p, t)) snprintf(g_status, sizeof g_status, "Wrote %s", p.c_str());
            else snprintf(g_status, sizeof g_status, "Could not write %s", p.c_str());
        }
    }
    ImGui::Separator();
    ImGui::BeginChild("sdltext", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::TextUnformatted(g_sdlText.c_str());
    ImGui::EndChild();
}

static void DrawPacTab() {
    ImGui::TextColored(theme::kRedHot, "%s container", g_pac.magic().c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("  %zu entries - DMC3 HD .pac stores no filenames, so entries are named by type + slot",
                        g_pac.entries().size());
    ImGui::TextDisabled("%s", g_pac.path().c_str());
    static char filter[64] = "";
    ImGui::SetNextItemWidth(220); ImGui::InputText("Filter type", filter, sizeof filter);
    ImGui::Separator();
    const auto& es = g_pac.entries();
    if (ImGui::BeginTable("pac", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 420))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Tag",  ImGuiTableColumnFlags_WidthFixed, 56);
        ImGui::TableSetupColumn("What is it", ImGuiTableColumnFlags_WidthStretch, 0.7f);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 92);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < es.size(); ++i) {
            const pac::Entry& e = es[i];
            if (filter[0] && e.desc.find(filter) == std::string::npos && e.tag.find(filter) == std::string::npos) continue;
            ImGui::TableNextRow(); ImGui::PushID((int)i);
            ImGui::TableNextColumn(); ImGui::Text("%d", e.slot);
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", e.tag.c_str());
            ImGui::TableNextColumn(); ImGui::TextColored(theme::kRed, "%s", e.desc.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%u", e.size);
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Export")) {
                pac::Bytes out; std::string err;
                if (g_pac.Extract(i, out, err)) {
                    char def[64]; snprintf(def, sizeof def, "slot%02d_%s.bin", e.slot, e.tag.c_str());
                    std::string dn = def; for (char& c : dn) if (c==' '||c=='/'||c=='\\') c = '_';
                    std::string pth = SaveFileDialogF("Binary\0*.bin\0All files\0*.*\0", "bin", dn.c_str());
                    if (!pth.empty()) {
                        if (WriteFileBytes(pth, out)) snprintf(g_status, sizeof g_status, "Exported slot %d (%s) -> %s", e.slot, e.type.c_str(), pth.c_str());
                        else snprintf(g_status, sizeof g_status, "Could not write %s", pth.c_str());
                    }
                } else snprintf(g_status, sizeof g_status, "Extract failed: %s", err.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void DrawArchiveTab() {
    if (!g_arc.loaded()) { ImGui::TextDisabled("No .arc open. File > Open an archive."); return; }
    ImGui::TextColored(theme::kRedHot, "ARC v%d", g_arc.version());
    ImGui::SameLine(); ImGui::TextDisabled("  %zu entries  |  %s", g_arc.entries().size(), g_arc.path().c_str());
    if (g_arc.dirty()) { ImGui::SameLine(); ImGui::TextColored(theme::kRedHot, "  [modified]"); }
    ImGui::TextDisabled("Open an .efl (effects) or .lmt (motion) entry to edit it; commit, then File > Save Archive.");
    // one-click: find and load the character body skeleton (the MOD with the most bones)
    if (ImGui::Button("Load body skeleton (auto)")) {
        const auto& es = g_arc.entries();
        int bestIdx = -1, bestBones = -1; std::string err;
        for (size_t i = 0; i < es.size(); ++i) {
            if (es[i].ext() != "mod") continue;            // only model entries
            efx::Bytes out;
            if (!g_arc.Extract(i, out, err)) continue;
            if (out.size() < 8 || memcmp(out.data(), "MOD\0", 4) != 0) continue;
            int bones = *reinterpret_cast<const uint16_t*>(&out[6]);
            if (bones > bestBones) { bestBones = bones; bestIdx = (int)i; }
        }
        if (bestIdx >= 0) {
            efx::Bytes out;
            if (g_arc.Extract(bestIdx, out, err)) AdoptBuffer(out, es[bestIdx].name, bestIdx);
        } else snprintf(g_status, sizeof g_status, "No model (MOD) found in this archive");
    }
    ImGui::SameLine(); ImGui::TextDisabled("<- finds the 74-bone body for the 3D View");
    static char filter[64] = "";
    ImGui::SetNextItemWidth(220); ImGui::InputText("Filter name", filter, sizeof filter);
    // quick type filter — click a type to show only those entries
    static char typeFilter[12] = "";   // "" = all
    ImGui::TextDisabled("Type:"); ImGui::SameLine();
    const char* kTypes[] = { "all", "efl", "lmt", "tex", "mod", "mrl" };
    for (const char* t : kTypes) {
        bool on = (t[0]=='a' && typeFilter[0]==0) || (strcmp(t, typeFilter)==0);
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, theme::kRedHot);
        if (ImGui::SmallButton(t)) { if (t[0]=='a') typeFilter[0]=0; else { strncpy(typeFilter, t, sizeof typeFilter-1); typeFilter[sizeof typeFilter-1]=0; } }
        if (on) ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    ImGui::NewLine();
    // multi-select: tick rows (Shift+click = range) then "Apply to SELECTED" below.
    static std::set<int> selSet; static int lastSel = -1;
    ImGui::Separator();
    if (ImGui::BeginTable("arc", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 360))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Sel", ImGuiTableColumnFlags_WidthFixed, 30);
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("What is it", ImGuiTableColumnFlags_WidthStretch, 0.40f);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 44);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 72);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();
        const auto& es = g_arc.entries();
        for (size_t i = 0; i < es.size(); ++i) {
            const arc::Entry& e = es[i];
            if (filter[0] && e.name.find(filter) == std::string::npos) continue;
            std::string ext = e.ext();
            if (typeFilter[0] && ext != typeFilter) continue;
            ImGui::TableNextRow(); ImGui::PushID((int)i);
            ImGui::TableNextColumn();
            bool sel = selSet.count((int)i) != 0;
            if (ImGui::Checkbox("##sel", &sel)) {
                if (ImGui::GetIO().KeyShift && lastSel >= 0) {          // Shift = range select
                    int a = lastSel < (int)i ? lastSel : (int)i, b = lastSel < (int)i ? (int)i : lastSel;
                    for (int k = a; k <= b; ++k) selSet.insert(k);
                } else { if (sel) selSet.insert((int)i); else selSet.erase((int)i); lastSel = (int)i; }
            }
            ImGui::TableNextColumn();
            ImGui::TextColored(g_srcEntry == (int)i ? theme::kRedHot : theme::kText, "%s", e.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextColored(theme::kRed, "%s", FriendlyName(e.name, ext).c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", ext.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%u", e.decompSize());
            ImGui::TableNextColumn();
            // Open routes by the entry's real magic: MOD->skeleton, LMT->motion, EFL->effect.
            if (ImGui::SmallButton("Open")) {
                efx::Bytes out; std::string err;
                if (g_arc.Extract(i, out, err)) AdoptBuffer(out, e.name, (int)i);
                else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    // --- Batch recolour EVERYTHING in the archive (effects + textures) -----------
    ImGui::Separator();
    ImGui::TextColored(theme::kRedHot, "Recolour ALL at once");
    ImGui::TextWrapped("Pick a colour, tick what to hit, Apply -> Save ALL. Effects use the _BM "
        "colour-code method; textures are colorised (shading kept). Each entry is read from "
        "pristine, so re-applying never compounds.");
    static float fxcol[3] = { 1.0f, 0.0f, 0.0f };
    static bool inclEfl=true, inclBM=true, inclMM=false, inclNM=false;
    ImGui::SetNextItemWidth(180);
    ImGui::ColorEdit3("##fxcol", fxcol, ImGuiColorEditFlags_DisplayRGB);
    ImGui::SameLine(); ImGui::Checkbox("effects(.efl)", &inclEfl);
    ImGui::SameLine(); ImGui::Checkbox("_BM", &inclBM);
    ImGui::SameLine(); ImGui::Checkbox("_MM", &inclMM);
    ImGui::SameLine(); ImGui::Checkbox("_NM", &inclNM);
    // .mrl / .mod carry no pixel colour — colour lives in the _BM/_MM/_NM textures they
    // reference. Disabled here (every texture is already covered by "Apply to ALL"); to
    // recolour just one material/model's textures, tick its row and use "Apply to SELECTED".
    ImGui::SameLine(); ImGui::BeginDisabled(); bool z=false;
    ImGui::Checkbox(".mrl", &z); ImGui::SameLine(); ImGui::Checkbox(".mod", &z);
    ImGui::EndDisabled();
    ImGui::TextDisabled(".mrl = texture references, .mod = geometry - no colour of their own. "
        "Select .mrl/.mod rows + \"Apply to SELECTED\" recolours the _BM/_MM/_NM textures they use.");
    auto suffixIs = [](const std::string& n, const char* s){
        return n.size() >= 3 && n.compare(n.size()-3, 3, s) == 0; };
    auto batchRecolor = [&](uint8_t r, uint8_t g, uint8_t bb){
        int nE=0,nC=0,nT=0; const auto& es=g_arc.entries();
        tex::Recolor rc; rc.colorize=true; rc.colStrength=1.0f;
        rc.colR=r/255.f; rc.colG=g/255.f; rc.colB=bb/255.f;
        for (size_t i=0;i<es.size();++i) {
            std::string ext=es[i].ext(), err;
            if (ext=="efl" && inclEfl) {
                efx::Bytes out; if(!g_arc.Extract(i,out,err,true)) continue;
                efx::EffectFile ef; if(!ef.LoadFromMemory(out,es[i].name,err)||!ef.isEFL()) continue;
                int codes=(int)ef.colorCodes().size(); if(!codes) continue;
                for(int c=0;c<codes;++c) ef.SetColorCode((size_t)c,r,g,bb,err);
                g_arc.Replace(i,ef.raw(),err); nE++; nC+=codes;
            } else if (ext=="tex") {
                const std::string& nm=es[i].name;
                bool want=(inclBM&&suffixIs(nm,"_BM"))||(inclMM&&suffixIs(nm,"_MM"))||(inclNM&&suffixIs(nm,"_NM"));
                if(!want) continue;
                efx::Bytes out; if(!g_arc.Extract(i,out,err,true)) continue;
                tex::TexInfo ti=tex::Parse(out); if(!ti.ok||!ti.recolorable()) continue;
                tex::Apply(out, ti, rc); g_arc.Replace(i,out,err); nT++;
            }
        }
        snprintf(g_status,sizeof g_status,"Recoloured %d effects (%d codes) + %d textures - Save ALL to write",nE,nC,nT);
    };
    if (ImGui::Button("Apply to ALL"))
        batchRecolor((uint8_t)(fxcol[0]*255+0.5f),(uint8_t)(fxcol[1]*255+0.5f),(uint8_t)(fxcol[2]*255+0.5f));
    ImGui::SameLine();
    if (ImGui::Button("ALL -> RED")) batchRecolor(255,0,0);

    // Resolve a referenced texture path (as embedded in an .mrl/.mod) to its .tex
    // entry index. Match is case-insensitive and slash-agnostic.
    auto texIndexByName=[&](std::string s)->int{
        for(char&c:s){ if(c=='/')c='\\'; if(c>='A'&&c<='Z')c=(char)(c+32); }
        const auto& es=g_arc.entries();
        for(size_t i=0;i<es.size();++i){ if(es[i].ext()!="tex") continue;
            std::string n=es[i].name; for(char&c:n){ if(c=='/')c='\\'; if(c>='A'&&c<='Z')c=(char)(c+32); }
            if(n==s) return (int)i; }
        return -1; };
    // .mrl/.mod store the textures they use as embedded resource-path strings
    // (e.g. "scr\\st705\\stage\\st700-t-mannaka_BM"). Scan a buffer for any ASCII
    // run that matches a .tex entry in this archive and collect those indices.
    auto collectTexRefs=[&](const efx::Bytes& buf, std::set<int>& out){
        std::string cur;
        auto flush=[&](){ if(cur.size()>=4){ int t=texIndexByName(cur); if(t>=0) out.insert(t); } cur.clear(); };
        for(unsigned char c:buf){ if(c>=0x20&&c<0x7f) cur.push_back((char)c); else flush(); }
        flush(); };
    auto findEntry=[&](const std::string& nm, const char* wantExt)->int{
        const auto& es=g_arc.entries();
        for(size_t i=0;i<es.size();++i) if(es[i].ext()==wantExt && es[i].name==nm) return (int)i;
        return -1; };

    // Apply only to the rows you ticked. efl -> colour codes; tex -> colorise.
    // .mrl / .mod own no pixels, so selecting one resolves the textures it references
    // (mod -> its sibling .mrl -> the _BM/_MM/_NM it uses) and recolours those. The
    // resolved textures honour the _BM/_MM/_NM tick-boxes above so normal maps aren't
    // touched unless asked; a directly-ticked .tex is always recoloured. Reads pristine.
    auto applySelected = [&](uint8_t r, uint8_t g, uint8_t bb){
        int nE=0,nC=0,nT=0; const auto& es=g_arc.entries();
        tex::Recolor rc; rc.colorize=true; rc.colStrength=1.0f;
        rc.colR=r/255.f; rc.colG=g/255.f; rc.colB=bb/255.f;
        std::set<int> texToDo;
        for (int idx : selSet) {
            if (idx < 0 || idx >= (int)es.size()) continue;
            std::string ext=es[idx].ext(), err;
            efx::Bytes out; if(!g_arc.Extract((size_t)idx,out,err,true)) continue;
            if (ext=="efl") {
                efx::EffectFile ef; if(!ef.LoadFromMemory(out,es[idx].name,err)||!ef.isEFL()) continue;
                int codes=(int)ef.colorCodes().size(); if(!codes) continue;
                for(int c=0;c<codes;++c) ef.SetColorCode((size_t)c,r,g,bb,err);
                g_arc.Replace((size_t)idx,ef.raw(),err); nE++; nC+=codes;
            } else if (ext=="tex") {
                texToDo.insert(idx);                       // explicit pick
            } else if (ext=="mrl") {
                collectTexRefs(out, texToDo);              // material -> its textures
            } else if (ext=="mod") {
                int m=findEntry(es[idx].name,"mrl");        // model -> sibling .mrl -> textures
                if(m>=0){ efx::Bytes mb; std::string e2; if(g_arc.Extract((size_t)m,mb,e2,true)) collectTexRefs(mb,texToDo); }
                collectTexRefs(out, texToDo);               // + any paths the .mod embeds directly
            }
        }
        for (int idx : texToDo) {
            std::string err; const std::string& nm=es[idx].name;
            bool picked = selSet.count(idx)!=0;             // directly ticked tex -> always
            bool suffixOK = (inclBM&&suffixIs(nm,"_BM"))||(inclMM&&suffixIs(nm,"_MM"))||(inclNM&&suffixIs(nm,"_NM"));
            if(!picked && !suffixOK) continue;              // mrl/mod-resolved -> honour tick-boxes
            efx::Bytes out; if(!g_arc.Extract((size_t)idx,out,err,true)) continue;
            tex::TexInfo ti=tex::Parse(out); if(!ti.ok||!ti.recolorable()) continue;
            tex::Apply(out, ti, rc); g_arc.Replace((size_t)idx,out,err); nT++;
        }
        snprintf(g_status,sizeof g_status,"SELECTED: %d effects (%d codes) + %d textures recoloured (mrl/mod refs resolved) - Save ALL",nE,nC,nT);
    };
    ImGui::TextDisabled("Selected: %zu", selSet.size()); ImGui::SameLine();
    if (ImGui::Button("Apply to SELECTED"))
        applySelected((uint8_t)(fxcol[0]*255+0.5f),(uint8_t)(fxcol[1]*255+0.5f),(uint8_t)(fxcol[2]*255+0.5f));
    ImGui::SameLine(); if (ImGui::Button("SELECTED -> RED")) applySelected(255,0,0);
    // Select all that's CURRENTLY SHOWN (honours the name + type filter) — not the whole list.
    ImGui::SameLine(); if (ImGui::Button("Select all shown")) {
        const auto& es = g_arc.entries();
        for (size_t i = 0; i < es.size(); ++i) {
            if (filter[0] && es[i].name.find(filter) == std::string::npos) continue;
            if (typeFilter[0] && es[i].ext() != typeFilter) continue;
            selSet.insert((int)i);
        }
    }
    ImGui::SameLine(); if (ImGui::Button("Clear selection")) { selSet.clear(); lastSel=-1; }
    ImGui::Separator();
    SaveAllButton();
    ImGui::SameLine();
    ImGui::TextDisabled("Edits to textures/effects/motions are kept automatically — this writes them all at once.");
}

static void DrawMotionTab() {
    if (g_active != AK_Lmt || !g_lmt.valid()) { ImGui::TextDisabled("No LMT motion loaded."); return; }
    ImGui::TextColored(theme::kRedHot, "LMT motion list  (version %d)", g_lmt.version());
    ImGui::SameLine(); ImGui::TextDisabled("  %zu slots, %d live", g_lmt.motions().size(), g_lmt.liveCount());
    ImGui::TextWrapped("CHANGE WHICH ANIMATION A MOVE PLAYS: set a slot's \"Plays\" to another slot # "
        "(the classic moveset-swap). Safe & reversible - it only redirects the motion table, never the "
        "animation data. Changed rows turn red; set Plays back to its own # to undo. You can also tweak "
        "frame/loop scalars. (Re-posing bone keyframes is not supported - that needs a 3D editor.)");
    ImGui::Separator();
    static int swapA = 0, swapB = 0;
    ImGui::SetNextItemWidth(70); ImGui::InputInt("##sa", &swapA, 0);
    ImGui::SameLine(); ImGui::TextDisabled("<->"); ImGui::SameLine();
    ImGui::SetNextItemWidth(70); ImGui::InputInt("##sb", &swapB, 0);
    ImGui::SameLine();
    if (ImGui::Button("Swap these two slots")) {
        std::string err;
        if (g_lmt.SwapSlots(g_lmtBuf, swapA, swapB, err)) { g_lmt.Parse(g_lmtBuf, err); g_lmtDirty = true;
            snprintf(g_status, sizeof g_status, "Swapped slots %d <-> %d (Commit + Save Archive to apply)", swapA, swapB); }
        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
    }
    ImGui::Separator();
    if (ImGui::BeginTable("lmt", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, ImVec2(0, 360))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Slot",   ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Plays",  ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Tracks", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Frames", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Loop",   ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();
        int nslots = (int)g_lmt.motions().size();
        for (const lmt::Motion& m : g_lmt.motions()) {
            int plays = g_lmt.playsSlot(m.index);
            bool remapped = (plays != m.index);
            // a slot is shown if it's live OR currently remapped to point at live data
            if (m.empty && !remapped) continue;
            ImGui::TableNextRow(); ImGui::PushID(m.index);
            ImGui::TableNextColumn();
            ImGui::TextColored(remapped ? theme::kRedHot : theme::kText, "%d", m.index);
            ImGui::TableNextColumn();
            { int v = plays; ImGui::SetNextItemWidth(-FLT_MIN);
              if (ImGui::InputInt("##pl", &v, 1, 1, ImGuiInputTextFlags_EnterReturnsTrue)) {
                  if (v < 0) v = 0; if (v >= nslots) v = nslots - 1;
                  std::string err;
                  if (g_lmt.RemapSlot(g_lmtBuf, m.index, v, err)) { g_lmt.Parse(g_lmtBuf, err); g_lmtDirty = true;
                      snprintf(g_status, sizeof g_status, "Slot %d now plays slot %d's animation (Commit + Save Archive)", m.index, v); }
                  else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
              } }
            ImGui::TableNextColumn(); ImGui::Text("%u", m.numTracks);
            ImGui::TableNextColumn();
            { int v = (int)m.numFrames; ImGui::SetNextItemWidth(-FLT_MIN);
              if (ImGui::InputInt("##fr", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && v >= 0) {
                  std::string err;
                  if (g_lmt.SetField(g_lmtBuf, m.index, lmt::LmtFile::NumFrames, (uint32_t)v, err)) {
                      g_lmt.Parse(g_lmtBuf, err); g_lmtDirty = true;
                      snprintf(g_status, sizeof g_status, "slot %d frames = %d", m.index, v);
                  } else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
              } }
            ImGui::TableNextColumn();
            { int v = (int)m.loopFrame; ImGui::SetNextItemWidth(-FLT_MIN);
              if (ImGui::InputInt("##lp", &v, 0, 0, ImGuiInputTextFlags_EnterReturnsTrue) && v >= 0) {
                  std::string err;
                  if (g_lmt.SetField(g_lmtBuf, m.index, lmt::LmtFile::LoopFrame, (uint32_t)v, err)) {
                      g_lmt.Parse(g_lmtBuf, err); g_lmtDirty = true;
                      snprintf(g_status, sizeof g_status, "slot %d loop = %d", m.index, v);
                  } else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
              } }
            ImGui::TableNextColumn(); ImGui::TextDisabled("0x%X", m.offset);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    SaveAllButton();   // save remaps/swaps without going through the File menu
    ImGui::SameLine(); ImGui::TextDisabled("save motion edits straight from here");
}

static void DrawInfoTab() {
    if (!g_file.loaded()) { ImGui::TextDisabled("No .efl loaded."); return; }
    if (g_file.isEFL())
        ImGui::TextColored(theme::kRedHot, "EFL effect file  (version 0x%08X)", g_file.version());
    else
        ImGui::TextColored(theme::kRedHot, "Not an EFL file (no 'EFL\\0' magic) - hex/manual tools still work");
    ImGui::TextDisabled("%zu bytes  |  %s", g_file.size(), g_file.path().c_str());
    ImGui::TextDisabled("%zu embedded path reference(s) found.", g_file.pathRefs().size());
    ImGui::Separator();
    ImGui::TextWrapped("A DMC4 .efl holds texture/animation path references (editable below) plus "
        "binary parameter blocks (colors, coordinates, sizes as IEEE floats). Swap a texture/anim "
        "pointer to re-skin an effect; use Manual Values + the Hex Inspector to pin numeric params.");
}

static void DrawPointersTab() {
    if (!g_file.loaded()) { ImGui::TextDisabled("No file loaded."); return; }
    const auto& refs = g_file.pathRefs();
    if (refs.empty()) { ImGui::TextDisabled("No path references found in this file."); return; }
    ImGui::TextDisabled("Edit the texture (_BM) and animation (ean) a particle uses. Max length = slot size.");
    ImGui::Separator();
    if (ImGui::BeginTable("refs", 4,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Kind",   ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Path",   ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn("Max",    ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < refs.size(); ++i) {
            const efx::PathRef& r = refs[i];
            ImGui::TableNextRow(); ImGui::PushID((int)i);
            ImGui::TableNextColumn(); ImGui::Text("0x%zX", r.offset);
            ImGui::TableNextColumn();
            ImGui::TextColored(r.kind == efx::PathRef::Texture ? theme::kRedHot : theme::kTextDim,
                               "%s", RefKind(r.kind));
            ImGui::TableNextColumn();
            char buf[256]; strncpy(buf, r.value.c_str(), sizeof buf - 1); buf[sizeof buf - 1] = 0;
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##p", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
                std::string err;
                if (g_file.SetPathRef(i, buf, err))
                    snprintf(g_status, sizeof g_status, "Set ref @0x%zX = %s", r.offset, buf);
                else
                    snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
            }
            ImGui::TableNextColumn(); ImGui::TextDisabled("%zu", r.slot ? r.slot - 1 : 0);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static const char* kTypeNames[] = { "float32", "float64", "uint8", "int32" };
// Typed read/write directly on the LMT buffer (efl goes through EffectFile).
static bool LmtRead(size_t off, efx::ValType t, std::string& out) {
    char tmp[64];
    if (t == efx::ValType::F32) { if (off+4>g_lmtBuf.size()) return false; float v; memcpy(&v,&g_lmtBuf[off],4); snprintf(tmp,sizeof tmp,"%g",v);}
    else if (t == efx::ValType::F64) { if (off+8>g_lmtBuf.size()) return false; double v; memcpy(&v,&g_lmtBuf[off],8); snprintf(tmp,sizeof tmp,"%g",v);}
    else if (t == efx::ValType::I32) { if (off+4>g_lmtBuf.size()) return false; int32_t v; memcpy(&v,&g_lmtBuf[off],4); snprintf(tmp,sizeof tmp,"%d",v);}
    else { if (off+1>g_lmtBuf.size()) return false; snprintf(tmp,sizeof tmp,"%u",g_lmtBuf[off]); }
    out = tmp; return true;
}
static bool LmtWrite(size_t off, efx::ValType t, const char* in) {
    if (t == efx::ValType::F32) { if (off+4>g_lmtBuf.size()) return false; float v=strtof(in,0); memcpy(&g_lmtBuf[off],&v,4);}
    else if (t == efx::ValType::F64) { if (off+8>g_lmtBuf.size()) return false; double v=strtod(in,0); memcpy(&g_lmtBuf[off],&v,8);}
    else if (t == efx::ValType::I32) { if (off+4>g_lmtBuf.size()) return false; int32_t v=(int32_t)strtol(in,0,0); memcpy(&g_lmtBuf[off],&v,4);}
    else { if (off+1>g_lmtBuf.size()) return false; long v=strtol(in,0,0); if(v<0||v>255) return false; g_lmtBuf[off]=(uint8_t)v; }
    return true;
}

static void DrawManualTab() {
    if (g_active == AK_None) { ImGui::TextDisabled("No file loaded."); return; }
    static int  off = 0;
    static int  type = 0;
    static char val[64] = "";
    ImGui::TextWrapped("Read/write a raw value at any byte offset of the active buffer (the open .efl "
        "or .lmt). Use the Hex Inspector to find a color/coordinate/float, then poke it here.");
    ImGui::Separator();
    ImGui::SetNextItemWidth(120); ImGui::InputInt("Offset (dec)", &off); if (off < 0) off = 0;
    ImGui::SameLine(); ImGui::SetNextItemWidth(110);
    ImGui::Combo("Type", &type, kTypeNames, IM_ARRAYSIZE(kTypeNames));
    efx::ValType vt = (efx::ValType)type;
    if (ImGui::Button("Read")) {
        std::string out; bool ok = (g_active == AK_Efl) ? g_file.ReadValue((size_t)off, vt, out) : LmtRead((size_t)off, vt, out);
        if (ok) { strncpy(val, out.c_str(), sizeof val - 1);
            snprintf(g_status, sizeof g_status, "Read %s @%d = %s", kTypeNames[type], off, out.c_str()); }
        else snprintf(g_status, sizeof g_status, "Error: offset past end");
    }
    ImGui::SameLine(); ImGui::SetNextItemWidth(160); ImGui::InputText("Value", val, sizeof val);
    ImGui::SameLine();
    if (ImGui::Button("Write")) {
        std::string err;
        bool ok = (g_active == AK_Efl) ? g_file.WriteValue((size_t)off, vt, val, err) : LmtWrite((size_t)off, vt, val);
        if (ok) snprintf(g_status, sizeof g_status, "Wrote %s @%d = %s", kTypeNames[type], off, val);
        else snprintf(g_status, sizeof g_status, "Error: %s", err.empty() ? "offset past end / range" : err.c_str());
    }
    ImGui::SameLine(); ImGui::TextColored(theme::kTextDim, "  (offset 0x%X)", off);
}

static void DrawHexTab() {
    if (g_active == AK_None) { ImGui::TextDisabled("No file loaded."); return; }

    // --- Effect colour codes (infernalwarks "_BM" method) -----------------------
    // Each effect texture (_BM) has a primary RGB colour code: 4 bytes, stored doubled,
    // 64 bytes before the name. Pick a colour -> bytes rewritten (both copies in sync).
    if (g_active == AK_Efl && g_file.isEFL() && !g_file.colorCodes().empty()) {
        const auto& cc = g_file.colorCodes();
        ImGui::TextColored(theme::kRedHot, "Effect colour codes  (%zu found)", cc.size());
        ImGui::TextWrapped("Primary colour per effect texture (the clean recolour method). "
            "Pick a colour to recolour that emitter - written straight to the bytes.");
        if (ImGui::Button("Set ALL to RED")) {
            std::string err;
            for (size_t i = 0; i < cc.size(); ++i) g_file.SetColorCode(i, 255, 0, 0, err);
            snprintf(g_status, sizeof g_status, "Set %zu effect colour codes to red", cc.size());
        }
        ImGui::SameLine(); ImGui::TextDisabled("(then File > Save Archive)");
        if (ImGui::BeginTable("ccodes", 4, ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|
                ImGuiTableFlags_ScrollY, ImVec2(0, 180))) {
            ImGui::TableSetupColumn("Primary colour", ImGuiTableColumnFlags_WidthFixed, 230);
            ImGui::TableSetupColumn("Palette", ImGuiTableColumnFlags_WidthFixed, 56);
            ImGui::TableSetupColumn("Offset",  ImGuiTableColumnFlags_WidthFixed, 70);
            ImGui::TableSetupColumn("Texture", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < cc.size(); ++i) {
                ImGui::TableNextRow(); ImGui::PushID((int)i);
                ImGui::TableNextColumn();
                float col[3] = { cc[i].r/255.f, cc[i].g/255.f, cc[i].b/255.f };
                if (ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_DisplayRGB)) {
                    std::string err;
                    g_file.SetColorCode(i, (uint8_t)(col[0]*255+0.5f), (uint8_t)(col[1]*255+0.5f),
                                            (uint8_t)(col[2]*255+0.5f), err);
                }
                ImGui::SameLine();
                ImGui::Text("%02X %02X %02X", cc[i].r, cc[i].g, cc[i].b);
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", cc[i].palette <= 1 ? "RGB" : (cc[i].palette >= 5 ? "CMYK" : "?"));
                ImGui::TableNextColumn(); ImGui::TextDisabled("0x%zX", cc[i].offset);
                ImGui::TableNextColumn(); ImGui::TextDisabled("%s", cc[i].label.c_str());
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::Separator();
    }

    ImGui::TextDisabled("Find a marker, read offsets, then poke values in the Manual Values tab.");
    static char find[64] = "_BM";
    ImGui::SetNextItemWidth(200);
    ImGui::InputText("Find text", find, sizeof find);
    ImGui::SameLine();
    if (ImGui::Button("Find")) {
        const efx::Bytes& b = ActiveBuf();
        long at = -1;
        for (size_t i = 0; find[0] && i + strlen(find) <= b.size(); ++i)
            if (memcmp(&b[i], find, strlen(find)) == 0) { at = (long)i; break; }
        if (at >= 0) { g_hexScroll = (int)(at / 16);
            snprintf(g_status, sizeof g_status, "'%s' found at 0x%lX (%ld)", find, at, at); }
        else snprintf(g_status, sizeof g_status, "'%s' not found", find);
    }
    ImGui::Separator();
    const efx::Bytes& raw = ActiveBuf();
    int rows = (int)((raw.size() + 15) / 16);
    ImGui::BeginChild("hex", ImVec2(0,0), true, ImGuiWindowFlags_HorizontalScrollbar);
    if (g_hexScroll > 0) { ImGui::SetScrollY(g_hexScroll * ImGui::GetTextLineHeightWithSpacing()); g_hexScroll = 0; }
    ImGuiListClipper clip; clip.Begin(rows);
    while (clip.Step()) {
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            size_t base = (size_t)row * 16;
            char line[160]; int p = 0;
            p += snprintf(line+p, sizeof line-p, "%08zX  ", base);
            for (int c = 0; c < 16; ++c) {
                if (base + c < raw.size()) p += snprintf(line+p, sizeof line-p, "%02X ", raw[base+c]);
                else p += snprintf(line+p, sizeof line-p, "   ");
            }
            p += snprintf(line+p, sizeof line-p, " ");
            for (int c = 0; c < 16 && base + c < raw.size(); ++c) {
                uint8_t ch = raw[base+c];
                line[p++] = (ch >= 32 && ch < 127) ? (char)ch : '.';
            }
            line[p] = 0;
            ImGui::TextUnformatted(line);
        }
    }
    ImGui::EndChild();
}

static void DrawViewTab(IDirect3DDevice9* dev) {
    if (!g_modelOK || !g_model.valid()) {
        ImGui::TextDisabled("No skeleton loaded.");
        ImGui::TextWrapped("Open a body model to view its rig: File > Open a body arc "
            "(e.g. nativeDX10/rom/player/costume/plmod_pl030.arc), then in the Archive tab Open "
            "the 'model\\game\\plNNN\\plNNN' entry (the one that loads as a skeleton).");
        return;
    }
    ImGui::TextColored(theme::kRedHot, "Model: %s", g_modelName.c_str());
    ImGui::SameLine(); ImGui::TextDisabled("  %d bones, %zu verts", g_model.numBones(), g_model.meshCloud().size());
    ImGui::SameLine();
    if (ImGui::SmallButton("Export OBJ (Blender)")) {
        std::string p = SaveFileDialogF("Wavefront OBJ\0*.obj\0All files\0*.*\0", "obj", (SafeStem(g_modelName)+".obj").c_str());
        if (!p.empty()) {
            std::string err;
            if (g_model.ExportOBJ(p, err)) snprintf(g_status, sizeof g_status, "Exported %d verts (per-mesh, exact order) -> %s", g_model.meshVertTotal(), p.c_str());
            else snprintf(g_status, sizeof g_status, "Export failed: %s", err.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Import OBJ (reshape)")) {
        std::string p = OpenFileDialogF("Wavefront OBJ\0*.obj\0All files\0*.*\0", "Import edited OBJ (same vertex count/order)");
        if (!p.empty()) {
            efx::Bytes newMod; std::string err; int chg = 0, clmp = 0;
            if (g_model.ImportOBJ(p, newMod, err, &chg, &clmp)) {
                char tail[64]; snprintf(tail, sizeof tail, "%d vert(s) moved%s", chg, clmp ? " (some clamped to model bounds)" : "");
                if (g_modelEntry >= 0 && g_arc.loaded()) {
                    std::string e2;
                    if (g_arc.Replace((size_t)g_modelEntry, newMod, e2)) {
                        g_model.Parse(newMod, e2);   // re-show the reshaped mesh
                        snprintf(g_status, sizeof g_status, "Reshaped: %s - Save ALL changes to write the arc", tail);
                    } else snprintf(g_status, sizeof g_status, "Replace failed: %s", e2.c_str());
                } else {
                    std::string sp = SaveFileDialogF("MOD model\0*.mod\0All files\0*.*\0", "mod", (SafeStem(g_modelName)+".mod").c_str());
                    if (!sp.empty()) {
                        if (WriteFileBytes(sp, newMod)) { g_model.Parse(newMod, err); snprintf(g_status, sizeof g_status, "Reshaped -> %s", sp.c_str()); }
                        else snprintf(g_status, sizeof g_status, "Could not write %s", sp.c_str());
                    }
                }
            } else snprintf(g_status, sizeof g_status, "Import failed: %s", err.c_str());
        }
    }
    static bool showSkel = true;
    ImGui::Checkbox("Skeleton", &showSkel); ImGui::SameLine();
    ImGui::TextDisabled("  L-drag = rotate, R-drag = move, wheel = zoom");
    ImGui::SameLine();
    if (ImGui::SmallButton("Recenter")) {
        const auto& bp = g_model.bindWorldPos();
        mdl::Vec3 lo{1e9f,1e9f,1e9f}, hi{-1e9f,-1e9f,-1e9f};
        for (auto& p : bp) { lo.x=p.x<lo.x?p.x:lo.x; lo.y=p.y<lo.y?p.y:lo.y; lo.z=p.z<lo.z?p.z:lo.z;
                             hi.x=p.x>hi.x?p.x:hi.x; hi.y=p.y>hi.y?p.y:hi.y; hi.z=p.z>hi.z?p.z:hi.z; }
        g_cam.target = { (lo.x+hi.x)*0.5f, (lo.y+hi.y)*0.5f, (lo.z+hi.z)*0.5f };
        float ext = hi.y-lo.y; if (ext<50) ext=50; g_cam.dist = ext*1.6f;
    }

    // --- animation player (needs a loaded LMT motion) ----------------------
    bool haveMotion = (g_active == AK_Lmt) && g_lmt.valid();
    uint32_t motionOff = 0; int numFrames = 0;
    if (haveMotion) {
        // build list of live motion slots
        const auto& ms = g_lmt.motions();
        if (g_motionSel < 0) for (auto& m : ms) if (!m.empty) { g_motionSel = m.index; break; }
        // selector
        char label[64]; snprintf(label, sizeof label, "slot %d", g_motionSel);
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("Motion", label)) {
            for (auto& m : ms) {
                int plays = g_lmt.playsSlot(m.index);
                if (m.empty && plays == m.index) continue;
                char it[80]; snprintf(it, sizeof it, "slot %d%s", m.index,
                                      plays != m.index ? " (remapped)" : "");
                if (ImGui::Selectable(it, g_motionSel == m.index)) { g_motionSel = m.index; g_frame = 0; }
            }
            ImGui::EndCombo();
        }
        // prev/next motion arrows — step to the neighbouring live slot
        auto stepMotion = [&](int dir) {
            int n = (int)ms.size(); if (n <= 0) return;
            for (int s = 1; s <= n; ++s) {
                int idx = (((g_motionSel + dir * s) % n) + n) % n;
                if (!(ms[idx].empty && g_lmt.playsSlot(idx) == idx)) { g_motionSel = idx; g_frame = 0; break; }
            }
        };
        ImGui::SameLine(); if (ImGui::ArrowButton("##prevM", ImGuiDir_Left))  stepMotion(-1);
        ImGui::SameLine(); if (ImGui::ArrowButton("##nextM", ImGuiDir_Right)) stepMotion(+1);
        if (g_motionSel >= 0 && g_motionSel < (int)ms.size()) motionOff = ms[g_motionSel].offset;
        anim::MotionInfo mi = anim::GetMotion(g_lmtBuf, motionOff);
        numFrames = mi.numFrames;
        // warn if this motion doesn't belong to the loaded skeleton (prevents the explode)
        float fit = anim::MatchScore(g_lmtBuf, motionOff, g_model);
        if (fit < 0.5f) {
            ImGui::SameLine();
            ImGui::TextColored(theme::kRedHot, "  WRONG CHARACTER (%.0f%% bone match) - load this motion's body",
                               fit * 100.0f);
        }
        ImGui::SameLine();
        if (ImGui::Button(g_playing ? "Stop" : "Play")) g_playing = !g_playing;
        ImGui::SameLine();
        if (ImGui::Button("|<")) { g_frame = 0; g_playing = false; }
        ImGui::SameLine(); ImGui::SetNextItemWidth(90);
        ImGui::DragFloat("fps", &g_fps, 0.5f, 1.0f, 120.0f, "%.0f");
        ImGui::SetNextItemWidth(-90);
        if (ImGui::SliderFloat("frame", &g_frame, 0.0f, numFrames > 1 ? (float)(numFrames-1) : 1.0f, "%.1f"))
            g_playing = false;
        ImGui::SameLine(); ImGui::Text("%d/%d", (int)g_frame, numFrames);
        if (g_playing && numFrames > 1) {
            g_frame += ImGui::GetIO().DeltaTime * g_fps;
            if (g_frame >= numFrames - 1) g_frame = 0; // loop
        }
        // --- swap moves by number (like the Motion tab), applied live so you see it here ---
        ImGui::Separator();
        ImGui::TextDisabled("Swap this move with that move (by slot #):");
        static int swA = 0, swB = 0;
        ImGui::SetNextItemWidth(64); ImGui::InputInt("##swa3d", &swA, 0);
        ImGui::SameLine(); ImGui::TextDisabled("<->"); ImGui::SameLine();
        ImGui::SetNextItemWidth(64); ImGui::InputInt("##swb3d", &swB, 0);
        ImGui::SameLine();
        if (ImGui::Button("Swap##3d")) {
            std::string err;
            if (g_lmt.SwapSlots(g_lmtBuf, swA, swB, err)) { g_lmt.Parse(g_lmtBuf, err); g_lmtDirty = true; g_frame = 0;
                snprintf(g_status, sizeof g_status, "Swapped slots %d <-> %d (showing live)", swA, swB); }
            else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
        }
        ImGui::SameLine(); ImGui::TextDisabled("  |  this slot (%d) plays:", g_motionSel);
        ImGui::SameLine(); static int playAs = 0; ImGui::SetNextItemWidth(64);
        ImGui::InputInt("##playas3d", &playAs, 0); ImGui::SameLine();
        if (ImGui::Button("Apply##3d")) {
            std::string err;
            if (g_lmt.RemapSlot(g_lmtBuf, g_motionSel, playAs, err)) { g_lmt.Parse(g_lmtBuf, err); g_lmtDirty = true; g_frame = 0;
                snprintf(g_status, sizeof g_status, "Slot %d now plays %d (showing live)", g_motionSel, playAs); }
            else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
        }
        ImGui::Separator();
        SaveAllButton();   // save motion (and any) edits without File menu
        ImGui::SameLine(); ImGui::TextDisabled("save move swaps straight from here");
    } else {
        ImGui::TextDisabled("Load a motion (.lmt) to animate this skeleton: open a body arc for the rig, "
                            "then open an LMT entry; a Play control appears here.");
    }
    ImGui::Separator();

    ImVec2 avail = ImGui::GetContentRegionAvail();
    int vw = (int)avail.x, vh = (int)(avail.y);
    if (vh < 64) vh = 64;

    static std::vector<mdl::Vec3> pos;
    bool animating = haveMotion && motionOff;
    if (animating) {
        auto locals = anim::PoseLocals(g_lmtBuf, motionOff, g_frame, g_model);
        pos = g_model.ComposeWorldPos(locals);
    } else {
        pos = g_model.bindWorldPos();
    }

    ImTextureID tex = view::Render(dev, vw, vh, pos, g_model.parents(), g_cam, -1,
                                   nullptr, showSkel, nullptr);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (tex) ImGui::Image(tex, ImVec2((float)vw, (float)vh));
    else ImGui::TextDisabled("(viewport target unavailable)");

    // mouse: left-drag = orbit, right-drag = pan/move, wheel = zoom
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            g_cam.yaw   -= io.MouseDelta.x * 0.01f;
            g_cam.pitch += io.MouseDelta.y * 0.01f;
            if (g_cam.pitch >  1.5f) g_cam.pitch =  1.5f;
            if (g_cam.pitch < -1.5f) g_cam.pitch = -1.5f;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            // pan the look-at target along the camera's right/up axes
            float cp = cosf(g_cam.pitch), sp = sinf(g_cam.pitch);
            float cy = cosf(g_cam.yaw),   sy = sinf(g_cam.yaw);
            // forward (target->eye) = (cp*sy, sp, cp*cy); right = normalize(cross(up,fwd))
            float fx = cp*sy, fy = sp, fz = cp*cy;
            float rx = fz, ry = 0, rz = -fx;                 // cross((0,1,0), fwd)
            float rl = sqrtf(rx*rx+rz*rz); if (rl<1e-4f) rl=1; rx/=rl; rz/=rl;
            float ux = ry*fz - rz*fy, uy = rz*fx - rx*fz, uz = rx*fy - ry*fx; // cross(right,fwd)
            float k = g_cam.dist * 0.0018f;
            g_cam.target.x += (-io.MouseDelta.x * rx + io.MouseDelta.y * ux) * k;
            g_cam.target.y += (-io.MouseDelta.x * ry + io.MouseDelta.y * uy) * k;
            g_cam.target.z += (-io.MouseDelta.x * rz + io.MouseDelta.y * uz) * k;
        }
        if (io.MouseWheel != 0) {
            g_cam.dist *= (io.MouseWheel > 0) ? 0.9f : 1.1f;
            if (g_cam.dist < 20)  g_cam.dist = 20;
            if (g_cam.dist > 2000) g_cam.dist = 2000;
        }
    }
    (void)p0;
}

// Is this arc entry recolorable: a _BM/_MM texture, or an .efl effect (RGBA colours)?
static bool IsRecolorTex(const arc::Entry& e) {
    if (e.ext() == "efl") return true;
    if (e.ext() != "tex") return false;
    const std::string& n = e.name;
    auto ends = [&](const char* s){ size_t l=strlen(s); return n.size()>=l && n.compare(n.size()-l,l,s)==0; };
    return ends("_BM") || ends("_MM") || ends("_NM");
}

// Apply the current recolor to every checked entry (from its ORIGINAL bytes, so it
// never compounds), staging each into the archive. Returns how many were recolored.
static int ApplyRecolorToSelected() {
    std::string err; int n = 0;
    for (int idx : g_texSel) {
        efx::Bytes data;
        if (!g_arc.Extract((size_t)idx, data, err, /*original=*/true)) continue;
        if (g_arc.entries()[idx].ext() == "efl") {          // effect file: recolor RGBA floats
            if (tex::RecolorEfl(data, g_texRC) > 0 && g_arc.Replace((size_t)idx, data, err)) ++n;
            continue;
        }
        tex::TexInfo ti = tex::Parse(data);                 // texture: recolor BCn blocks
        if (!ti.ok || !ti.recolorable()) continue;
        tex::Apply(data, ti, g_texRC);
        if (g_arc.Replace((size_t)idx, data, err)) ++n;
    }
    return n;
}

// Compact hue-wheel picker, shown to the RIGHT of a texture preview/canvas. Sets the
// shared brush colour, and (in "Pick a colour" mode) the recolour target. Fixed size so
// it never balloons to fill a maximised window.
static void DrawColourWheelBeside() {
    ImGui::BeginGroup();
    ImGui::TextColored(theme::kRedHot, "Colour");
    ImGui::SetNextItemWidth(170);
    if (ImGui::ColorPicker3("##wheel", g_brushColor,
            ImGuiColorEditFlags_PickerHueWheel | ImGuiColorEditFlags_NoSidePreview |
            ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel)) {
        if (g_texRC.colorize) { g_texRC.colR=g_brushColor[0]; g_texRC.colG=g_brushColor[1]; g_texRC.colB=g_brushColor[2]; g_texDirtyPreview = true; }
    }
    ImGui::EndGroup();
}

// Recolour embedded DDS textures of the open .pac in place (DMC3 HD). Shares the colour
// state (g_texRC) chosen above. Recolour runs on an isolated copy of each DDS so it only
// touches that texture's blocks, then splices back; Save PAC writes the file (one-time .bak).
static std::set<int> g_pacTexSel;
static int           g_pacTexFocus = -1;
static void DrawPacTextures(IDirect3DDevice9* dev) {
    static std::string lastPac;
    if (lastPac != g_pac.path()) { lastPac = g_pac.path(); g_pacTexSel.clear(); g_pacTexFocus = -1; g_texDirtyPreview = true; }
    const auto& ts = g_pac.textures();
    ImGui::TextColored(theme::kRedHot, "%zu texture(s) in this .pac", ts.size());
    ImGui::TextDisabled("Recolour in place with the colour above, then Save PAC - no export/.bin needed.");
    ImGui::TextDisabled("Reads BC1/2/3, BC5, BC7 (stage textures) and uncompressed RGBA. BC7 is re-encoded on save.");
    if (ts.empty()) { ImGui::TextDisabled("(no embedded textures found in this container)"); return; }

    static char filter[64] = "";
    ImGui::SetNextItemWidth(200); ImGui::InputText("Filter", filter, sizeof filter);
    ImGui::SameLine(); if (ImGui::SmallButton("Select all")) for (int i=0;i<(int)ts.size();++i) g_pacTexSel.insert(i);
    ImGui::SameLine(); if (ImGui::SmallButton("Select none")) g_pacTexSel.clear();
    ImGui::SameLine(); ImGui::TextDisabled("%zu selected", g_pacTexSel.size());

    ImGui::BeginChild("pactexlist", ImVec2(0, 200), true);
    for (int i = 0; i < (int)ts.size(); ++i) {
        if (filter[0] && ts[i].name.find(filter) == std::string::npos) continue;
        bool sel = g_pacTexSel.count(i) > 0; ImGui::PushID(i);
        if (ImGui::Checkbox("##c", &sel)) { if (sel) g_pacTexSel.insert(i); else g_pacTexSel.erase(i); }
        ImGui::SameLine();
        if (ImGui::Selectable(ts[i].name.c_str(), g_pacTexFocus == i)) { g_pacTexFocus = i; g_texDirtyPreview = true; }
        ImGui::PopID();
    }
    ImGui::EndChild();

    auto recolorOne = [&](const pac::TexRef& t, std::string& err) -> bool {
        const pac::Bytes& f = g_pac.data();
        if ((size_t)t.off + t.size > f.size()) { err = "texture out of range"; return false; }
        pac::Bytes dds(f.begin() + t.off, f.begin() + t.off + t.size);  // isolated copy
        tex::TexInfo ti = tex::ParseDDS(dds, 0);
        if (!ti.ok) { err = "DDS parse failed"; return false; }
        tex::Apply(dds, ti, g_texRC);
        return g_pac.Splice(t.off, dds, err);
    };

    if (g_pacTexSel.empty()) ImGui::BeginDisabled();
    char ab[64]; snprintf(ab, sizeof ab, "Apply colour to %zu selected", g_pacTexSel.size());
    if (ImGui::Button(ab)) {
        int n = 0; std::string err;
        for (int i : g_pacTexSel) if (i >= 0 && i < (int)ts.size() && recolorOne(ts[i], err)) ++n;
        g_texDirtyPreview = true;
        snprintf(g_status, sizeof g_status, "Recoloured %d texture(s) in memory - click Save PAC to write", n);
    }
    if (g_pacTexSel.empty()) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!g_pac.dirty()) ImGui::BeginDisabled();
    if (ImGui::Button("Save PAC")) {
        std::string err;
        if (g_pac.SaveInPlace(err)) snprintf(g_status, sizeof g_status, "Saved %s (.bak kept)", g_pac.path().c_str());
        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
    }
    if (!g_pac.dirty()) ImGui::EndDisabled();
    if (g_pac.dirty()) { ImGui::SameLine(); ImGui::TextColored(theme::kRedHot, "[unsaved]"); }

    ImGui::Separator();
    ImGui::Checkbox("Paint mode - click/drag the image to edit specific pixels", &g_paintMode);
    if (g_pacTexFocus < 0 || g_pacTexFocus >= (int)ts.size()) {
        ImGui::TextDisabled("Select a texture above to preview or paint it.");
        return;
    }
    const pac::TexRef& t = ts[g_pacTexFocus];

    if (!g_paintMode) {
        // preview the focused texture with the current recolour applied (non-destructive copy)
        static int pw = 0, ph = 0; static ImTextureID pt = (ImTextureID)0; static int last = -1;
        if (g_texDirtyPreview || last != g_pacTexFocus) {
            last = g_pacTexFocus;
            const pac::Bytes& f = g_pac.data();
            if ((size_t)t.off + t.size <= f.size()) {
                pac::Bytes dds(f.begin() + t.off, f.begin() + t.off + t.size);
                tex::TexInfo ti = tex::ParseDDS(dds, 0);
                if (ti.ok) {
                    tex::Apply(dds, ti, g_texRC);
                    int mip = tex::PreviewMip(ti, 256); std::vector<uint8_t> rgba; int w=0,h=0;
                    if (tex::DecodeRGBA(dds, ti, mip, rgba, w, h)) { pt = view::MakePreview(dev, rgba.data(), w, h); pw=w; ph=h; }
                }
            }
            g_texDirtyPreview = false;
        }
        ImGui::TextDisabled("Preview: %s", t.name.c_str());
        if (pt) { float d = 256.0f; ImGui::Image(pt, ImVec2(d, d * (pw ? (float)ph/pw : 1.0f)));
                  ImGui::SameLine(); DrawColourWheelBeside(); }
        return;
    }

    // --- Paint mode: decode the focused texture to a full-res RGBA working buffer once,
    //     paint discs into it on click/drag, then bake it back (re-encodes + all mips). ---
    if (g_paintFor != g_pacTexFocus || g_paintRGBA.empty()) {
        const pac::Bytes& f = g_pac.data();
        g_paintRGBA.clear(); g_paintW = g_paintH = 0; g_paintFor = g_pacTexFocus;
        if ((size_t)t.off + t.size <= f.size()) {
            pac::Bytes dds(f.begin() + t.off, f.begin() + t.off + t.size);
            tex::TexInfo ti = tex::ParseDDS(dds, 0); int w=0,h=0;
            if (ti.ok && tex::DecodeRGBA(dds, ti, 0, g_paintRGBA, w, h)) { g_paintW=w; g_paintH=h; }
            g_paintDirty = true;
        }
    }
    if (g_paintRGBA.empty() || g_paintW <= 0) { ImGui::TextDisabled("Could not decode %s for painting.", t.name.c_str()); return; }

    ImGui::SetNextItemWidth(160); ImGui::SliderFloat("Brush size", &g_brushRadius, 1.0f, 96.0f, "%.0f px");
    ImGui::SameLine(); ImGui::SetNextItemWidth(140); ImGui::SliderFloat("Strength", &g_brushStrength, 0.05f, 1.0f, "%.2f");
    ImGui::SameLine(); ImGui::Checkbox("Soft edge", &g_brushSoft);
    ImGui::ColorEdit3("Brush colour", g_brushColor, ImGuiColorEditFlags_NoInputs);
    ImGui::SameLine(); ImGui::TextDisabled("(pick it on the wheel above) - drag on the image to paint");

    auto paintDisc = [&](int cx, int cy) {
        int R = (int)g_brushRadius; if (R < 1) R = 1;
        float br = g_brushColor[0]*255.f, bg = g_brushColor[1]*255.f, bb = g_brushColor[2]*255.f;
        for (int y = cy-R; y <= cy+R; ++y) for (int x = cx-R; x <= cx+R; ++x) {
            if (x < 0 || y < 0 || x >= g_paintW || y >= g_paintH) continue;
            float dx = (float)(x-cx), dy = (float)(y-cy); float dist = sqrtf(dx*dx + dy*dy);
            if (dist > R) continue;
            float fall = g_brushSoft ? (1.0f - dist/R) : 1.0f; float s = g_brushStrength * fall;
            size_t o = ((size_t)y*g_paintW + x)*4;
            g_paintRGBA[o]   = (uint8_t)(g_paintRGBA[o]  *(1-s) + br*s);
            g_paintRGBA[o+1] = (uint8_t)(g_paintRGBA[o+1]*(1-s) + bg*s);
            g_paintRGBA[o+2] = (uint8_t)(g_paintRGBA[o+2]*(1-s) + bb*s);
        }
    };

    static ImTextureID pt = (ImTextureID)0;
    if (g_paintDirty) { pt = view::MakePreview(dev, g_paintRGBA.data(), g_paintW, g_paintH); g_paintDirty = false; }
    float dispW = 380.0f, dispH = dispW * (float)g_paintH / g_paintW;
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    if (pt) ImGui::Image(pt, ImVec2(dispW, dispH));
    bool overImg = ImGui::IsItemHovered();
    ImGui::SameLine(); DrawColourWheelBeside();              // wheel to the right of the canvas
    if (overImg && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 m = ImGui::GetMousePos();
        int cx = (int)((m.x - p0.x)/dispW * g_paintW), cy = (int)((m.y - p0.y)/dispH * g_paintH);
        paintDisc(cx, cy); g_paintDirty = true;
    }

    if (ImGui::Button("Bake paint into texture")) {
        const pac::Bytes& f = g_pac.data();
        if ((size_t)t.off + t.size <= f.size()) {
            pac::Bytes dds(f.begin() + t.off, f.begin() + t.off + t.size);
            tex::TexInfo ti = tex::ParseDDS(dds, 0); std::string err;
            if (ti.ok && tex::EncodeFromRGBA(dds, ti, g_paintRGBA, err)) {
                std::string e2;
                if (g_pac.Splice(t.off, dds, e2)) snprintf(g_status, sizeof g_status, "Painted %s baked in memory - click Save PAC to write", t.name.c_str());
                else snprintf(g_status, sizeof g_status, "Splice failed: %s", e2.c_str());
            } else snprintf(g_status, sizeof g_status, "Encode failed: %s", err.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload (discard unbaked paint)")) { g_paintFor = -1; }
    ImGui::SameLine(); ImGui::TextDisabled("%dx%d - edits apply at full resolution", g_paintW, g_paintH);
}

static void DrawTexturesTab(IDirect3DDevice9* dev) {
    if (!g_arc.loaded() && !g_pac.loaded()) { ImGui::TextDisabled("Open an .arc or .pac first (File > Open)."); return; }

    static int seenGen = -1;
    static float tint[3] = {1,1,1};
    if (seenGen != g_texGen) { seenGen = g_texGen; g_texDirtyPreview = true; }

    // mode: Hue-shift (outfits) vs Pick a colour (effects)
    static int mode = 0; // 0 = hue shift, 1 = colorize
    bool ch = false;
    ImGui::TextColored(theme::kRedHot, "Pick a colour, check the textures, Apply to all.");
    ch |= ImGui::RadioButton("Hue shift (outfits)", &mode, 0); ImGui::SameLine();
    ch |= ImGui::RadioButton("Pick a colour (effects)", &mode, 1);
    g_texRC.colorize = (mode == 1);

    if (mode == 0) {
        ImGui::TextDisabled("Rotate the colour (good for outfits).");
        ch |= ImGui::SliderFloat("Hue",        &g_texRC.hue, -180, 180, "%.0f deg");
        ch |= ImGui::SliderFloat("Saturation", &g_texRC.sat, 0.0f, 2.0f, "%.2f");
        ch |= ImGui::SliderFloat("Brightness", &g_texRC.val, 0.2f, 2.0f, "%.2f");
        if (ImGui::ColorEdit3("Tint (multiply)", tint, ImGuiColorEditFlags_NoInputs)) {
            g_texRC.tintR = tint[0]; g_texRC.tintG = tint[1]; g_texRC.tintB = tint[2]; ch = true;
        }
    } else {
        ImGui::TextDisabled("Set everything to one colour (good for effects). Pick from the table:");
        struct SW { const char* n; float r,g,b; };
        static const SW pal[] = {
            {"Red",1,0,0},{"Orange",1,0.5f,0},{"Yellow",1,1,0},{"Green",0,1,0},
            {"Cyan",0,1,1},{"Blue",0,0.3f,1},{"Purple",0.6f,0,1},{"Pink",1,0.3f,0.7f},
            {"White",1,1,1},{"Black",0.05f,0.05f,0.05f},
        };
        for (int i = 0; i < (int)(sizeof(pal)/sizeof(pal[0])); ++i) {
            ImGui::PushID(i);
            if (ImGui::ColorButton(pal[i].n, ImVec4(pal[i].r,pal[i].g,pal[i].b,1),
                                   0, ImVec2(28,28))) {
                g_texRC.colR=pal[i].r; g_texRC.colG=pal[i].g; g_texRC.colB=pal[i].b; ch = true;
            }
            ImGui::PopID();
            if ((i % 5) != 4 && i != (int)(sizeof(pal)/sizeof(pal[0]))-1) ImGui::SameLine();
        }
        float col[3] = { g_texRC.colR, g_texRC.colG, g_texRC.colB };
        if (ImGui::ColorEdit3("Custom colour", col, ImGuiColorEditFlags_NoInputs)) {
            g_texRC.colR=col[0]; g_texRC.colG=col[1]; g_texRC.colB=col[2]; ch = true;
        }
        ch |= ImGui::SliderFloat("Strength", &g_texRC.colStrength, 0.0f, 1.0f, "%.2f");
    }
    if (ImGui::Button("Reset colour")) { g_texRC = tex::Recolor(); g_texRC.colorize=(mode==1); tint[0]=tint[1]=tint[2]=1; ch = true; }
    if (ch) { g_texDirtyPreview = true; }
    ImGui::Separator();

    // DMC3 .pac stores its textures as embedded DDS — same colour controls, own list.
    if (g_pac.loaded() && !g_arc.loaded()) { DrawPacTextures(dev); return; }

    // ---- multi-select list of recolorable textures (_BM / _MM only) ----------
    static char filter[64] = "";
    ImGui::SetNextItemWidth(200); ImGui::InputText("Filter", filter, sizeof filter);
    ImGui::SameLine();
    // gather matching entries
    const auto& es = g_arc.entries();
    std::vector<int> rows;
    for (int i = 0; i < (int)es.size(); ++i)
        if (IsRecolorTex(es[i]) && (!filter[0] || es[i].name.find(filter) != std::string::npos))
            rows.push_back(i);
    if (ImGui::SmallButton("Select all")) for (int i : rows) g_texSel.insert(i);
    ImGui::SameLine(); if (ImGui::SmallButton("Select none")) g_texSel.clear();
    ImGui::SameLine(); ImGui::TextDisabled("%d item(s) [_BM/_MM + .efl], %zu selected", (int)rows.size(), g_texSel.size());

    ImGui::BeginChild("texlist", ImVec2(0, 220), true);
    for (int i : rows) {
        bool sel = g_texSel.count(i) > 0;
        ImGui::PushID(i);
        if (ImGui::Checkbox("##c", &sel)) { if (sel) g_texSel.insert(i); else g_texSel.erase(i); }
        ImGui::SameLine();
        const char* fn = es[i].name.c_str();
        bool isEfl = es[i].ext() == "efl";
        if (ImGui::Selectable(fn, g_texEntry == i, ImGuiSelectableFlags_AllowOverlap) && !isEfl) {
            efx::Bytes data; std::string err;   // preview textures (efl has no image)
            if (g_arc.Extract((size_t)i, data, err, /*original=*/true)) AdoptBuffer(data, es[i].name, i);
        }
        if (isEfl) { ImGui::SameLine(); ImGui::TextDisabled("(effect)"); }
        ImGui::PopID();
    }
    ImGui::EndChild();

    // apply to selected
    if (g_texSel.empty()) ImGui::BeginDisabled();
    char ab[64]; snprintf(ab, sizeof ab, "Apply colour to %zu selected", g_texSel.size());
    if (ImGui::Button(ab)) {
        int n = ApplyRecolorToSelected();
        snprintf(g_status, sizeof g_status, "Recolored %d texture(s) - now Save ALL changes", n);
    }
    if (g_texSel.empty()) ImGui::EndDisabled();
    ImGui::SameLine(); SaveAllButton();

    // DDS round-trip for the focused texture: export to edit anywhere, import the result.
    if (g_texEntry >= 0 && g_texInfo.ok) {
        ImGui::Separator();
        ImGui::TextDisabled("Round-trip: export this texture to .dds, recolour/edit it in any tool, then import it back.");
        if (ImGui::Button("Export DDS...")) {
            tex::Bytes dds;
            if (tex::ToDDS(g_texOrig, g_texInfo, dds)) {
                std::string p = SaveFileDialogF("DDS texture\0*.dds\0All files\0*.*\0", "dds", (SafeStem(g_texName)+".dds").c_str());
                if (!p.empty()) {
                    if (WriteFileBytes(p, dds))
                        snprintf(g_status, sizeof g_status, "Exported %s (%dx%d BCn) -> %s", g_texName.c_str(), g_texInfo.width, g_texInfo.height, p.c_str());
                    else snprintf(g_status, sizeof g_status, "Could not write %s", p.c_str());
                }
            } else snprintf(g_status, sizeof g_status, "Cannot export: texture format 0x%X has no DDS mapping", g_texInfo.format);
        }
        ImGui::SameLine();
        if (ImGui::Button("Import DDS...")) {
            std::string p = OpenFileDialogF("DDS texture\0*.dds\0All files\0*.*\0", "Import edited DDS (same size + BCn format)");
            if (!p.empty()) {
                efx::Bytes dds;
                if (!ReadFileBytes(p, dds)) snprintf(g_status, sizeof g_status, "Could not read %s", p.c_str());
                else {
                    efx::Bytes nt; std::string err;
                    if (tex::FromDDS(dds, g_texOrig, g_texInfo, nt, err)) {
                        std::string e2;
                        if (g_arc.Replace((size_t)g_texEntry, nt, e2)) {
                            g_texOrig = nt; g_texInfo = tex::Parse(nt); g_texRC = tex::Recolor();
                            g_texChanged = false; g_texDirtyPreview = true; g_texGen++;
                            snprintf(g_status, sizeof g_status, "Imported into %s - Save ALL changes to write it to the archive", g_texName.c_str());
                        } else snprintf(g_status, sizeof g_status, "Import staged but Replace failed: %s", e2.c_str());
                    } else snprintf(g_status, sizeof g_status, "Import rejected: %s", err.c_str());
                }
            }
        }
        ImGui::SameLine(); ImGui::TextDisabled("(must keep the same dimensions + BCn compression)");
    }

    // preview of the focused texture
    if (g_texEntry >= 0 && g_texInfo.ok) {
        static int pw=0, ph=0; static ImTextureID ptex=(ImTextureID)0;
        if (g_texDirtyPreview) {
            efx::Bytes tmp = g_texOrig; tex::Apply(tmp, g_texInfo, g_texRC);
            int mip = tex::PreviewMip(g_texInfo, 256);
            std::vector<uint8_t> rgba; int w=0,h=0;
            if (tex::DecodeRGBA(tmp, g_texInfo, mip, rgba, w, h)) { ptex=view::MakePreview(dev,rgba.data(),w,h); pw=w; ph=h; }
            g_texDirtyPreview = false;
        }
        ImGui::Separator();
        ImGui::TextDisabled("Preview: %s (click a name above to preview it)", g_texName.c_str());
        if (ptex) { float d=256.0f; ImGui::Image(ptex, ImVec2(d, d*(ph?(float)ph/pw:1.0f)));
                    ImGui::SameLine(); DrawColourWheelBeside(); }
    }
}

namespace fs = std::filesystem;

// Install a mod: copy every .arc under the mod folder over the matching game arc in
// nativeDX10/, backing up each original to .bak first. Returns count installed.
static int InstallMod(const fs::path& modDir, std::string& msg) {
    int n = 0, fail = 0;
    std::error_code ec;
    for (auto& it : fs::recursive_directory_iterator(modDir, ec)) {
        if (ec) break;
        if (!it.is_regular_file()) continue;
        if (it.path().extension() != ".arc") continue;
        // relative path inside the mod, e.g. rom/player/uPlayerVergil.arc
        fs::path rel = fs::relative(it.path(), modDir, ec);
        fs::path dest = fs::path(g_gameRoot) / "nativeDX10" / rel;
        if (!fs::exists(dest)) { ++fail; continue; }   // mod file with no game counterpart
        fs::path bak = dest; bak += ".bak";
        if (!fs::exists(bak)) fs::copy_file(dest, bak, ec);   // one-time original backup
        fs::copy_file(it.path(), dest, fs::copy_options::overwrite_existing, ec);
        if (!ec) ++n; else ++fail;
    }
    char b[160]; snprintf(b, sizeof b, "Installed %d arc(s)%s. Launch the game to see it.",
                          n, fail ? " (some had no game match)" : "");
    msg = b;
    return n;
}

static void DrawModsTab() {
    if (g_gameRoot.empty()) {
        ImGui::TextDisabled("Open any game .arc first (File > Open) so I know your game folder.");
        return;
    }
    ImGui::TextColored(theme::kRedHot, "Mods");
    ImGui::TextDisabled("Game: %s", g_gameRoot.c_str());
    ImGui::TextWrapped("One-click apply a colour/effect overhaul (like Jade Remastered). It copies the "
        "mod's arcs over the game (originals backed up to .bak). This is the SAFE way to recolor "
        "effects - the mod author edited only the right values, so nothing breaks.");
    ImGui::Separator();

    fs::path modsDir = fs::path(g_gameRoot) / "MODS";
    std::error_code ec;
    if (!fs::exists(modsDir)) { ImGui::TextDisabled("No MODS folder found at %s", modsDir.string().c_str()); return; }

    if (ImGui::BeginTable("mods", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY, ImVec2(0, 320))) {
        ImGui::TableSetupColumn("Mod", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableHeadersRow();
        for (auto& d : fs::directory_iterator(modsDir, ec)) {
            if (ec) break;
            if (!d.is_directory()) continue;
            // only show mods that contain a rom/ tree (player arcs)
            bool hasRom = fs::exists(d.path() / "rom") || fs::exists(d.path() / "nativeDX10");
            if (!hasRom) continue;
            std::string name = d.path().filename().string();
            ImGui::TableNextRow(); ImGui::PushID(name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(name.c_str());
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Apply")) {
                std::string msg; InstallMod(d.path(), msg);
                snprintf(g_status, sizeof g_status, "%s: %s", name.c_str(), msg.c_str());
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();
    if (ImGui::Button("Restore ALL game arcs (undo every mod)")) {
        int n = 0;
        for (auto& it : fs::recursive_directory_iterator(fs::path(g_gameRoot) / "nativeDX10", ec)) {
            if (ec) break;
            if (it.path().extension() == ".bak") {
                fs::path arc = it.path(); arc.replace_extension();  // strip .bak
                fs::copy_file(it.path(), arc, fs::copy_options::overwrite_existing, ec);
                if (!ec) ++n;
            }
        }
        snprintf(g_status, sizeof g_status, "Restored %d arc(s) to original (mods undone)", n);
    }
    ImGui::SameLine(); ImGui::TextDisabled("reverts every arc to its .bak original");
}

// Auto-find the Steam DMC save files inside this CrossOver bottle so the user can open
// them with one click (no digging through Program Files). Returns the first that exists.
static std::string FindSaveDefaultDir() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path ud = fs::path(getenv("HOME") ? getenv("HOME") : "") /
        "CXPBottles/Steam/drive_c/Program Files (x86)/Steam/userdata";
    if (!fs::exists(ud, ec)) return "";
    for (auto& acc : fs::directory_iterator(ud, ec)) {           // userdata/<accountid>/
        if (ec || !acc.is_directory()) continue;
        // prefer the DMC HD Collection remote (dmc1/2/3.sav)
        fs::path hd = acc.path() / "631510" / "remote";
        if (fs::exists(hd, ec)) return hd.string();
    }
    return ud.string();
}

static void DrawSaveTab() {
    ImGui::TextColored(theme::kRedHot, "Steam Save Editor — DMC 1 / 2 / 3 (HD), DMC4SE, DMC5");
    ImGui::TextDisabled("Opens a save, edits values, writes back with a one-time .bak. Close the game + ");
    ImGui::SameLine(0,0); ImGui::TextDisabled("Steam (or disable Cloud) first, or Steam will overwrite your edit.");

    if (ImGui::Button("Open save file...")) {
        static std::string lastDir;
        if (lastDir.empty()) lastDir = FindSaveDefaultDir();
        std::string p = OpenFileDialogF("DMC saves\0*.sav;*.bin;*.dat\0All files\0*.*\0",
                                        "Open a DMC save (dmc1/2/3.sav, data00*.bin, DMC4SE save)");
        if (!p.empty()) {
            std::string err;
            if (g_save.Load(p, err)) { g_saveSlot = 0;
                snprintf(g_status, sizeof g_status, "Loaded %s — %s", p.c_str(), g_save.gameName()); }
            else snprintf(g_status, sizeof g_status, "Save load error: %s", err.c_str());
        }
    }
    ImGui::SameLine();
    if (g_save.loaded()) ImGui::TextColored(theme::kRed, "%s  (%zu bytes)%s", g_save.gameName(), g_save.size(), g_save.dirty()?"  [unsaved]":"");
    if (!g_save.loaded()) { ImGui::TextDisabled("No save open. Default location: …/Steam/userdata/<id>/631510/remote/"); return; }
    ImGui::Separator();

    // slot selector for slotted games (DMC1)
    if (g_save.slotted()) {
        ImGui::Text("Slot:"); ImGui::SameLine();
        for (int s = 0; s < g_save.slotCount(); ++s) {
            char b[24]; snprintf(b, sizeof b, "%d%s", s, g_save.slotUsed(s) ? "" : "·");
            if (s) ImGui::SameLine();
            bool sel = g_saveSlot == s;
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, theme::kRed);
            if (ImGui::SmallButton(b)) g_saveSlot = s;
            if (sel) ImGui::PopStyleColor();
        }
        ImGui::SameLine(); ImGui::TextDisabled(g_save.slotUsed(g_saveSlot) ? "(in use)" : "(empty · )");
    }

    // friendly fields
    const auto& fl = g_save.fields();
    if (!fl.empty()) {
        ImGui::Spacing(); ImGui::TextColored(theme::kRedHot, "Values");
        if (ImGui::BeginTable("svf", 2, ImGuiTableFlags_SizingFixedFit)) {
            for (const auto& f : fl) {
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Text("%s", f.name);
                if (f.help && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", f.help);
                ImGui::TableNextColumn();
                int v = (int)g_save.ReadField(f, g_saveSlot);
                ImGui::PushID(f.name); ImGui::SetNextItemWidth(160);
                int maxv = f.maxv ? (int)f.maxv : (f.bytes>=4 ? 2000000000 : (1<<(f.bytes*8))-1);
                if (ImGui::InputInt("##v", &v)) { if (v<0)v=0; if (v>maxv)v=maxv; g_save.WriteField(f, g_saveSlot, (uint32_t)v); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextDisabled("No friendly fields mapped for %s yet — use the raw editor below.", g_save.gameName());
        if (g_save.game()==sav::GAME_DMC5)
            ImGui::TextDisabled("DMC5 edits write directly (no resign needed for local edits).");
    }

    // arbitrary offset/value poke — "change whatever you want"
    ImGui::Spacing(); ImGui::Separator();
    ImGui::TextColored(theme::kRedHot, "Raw edit (any offset)");
    static int roff = 0, rwidth = 2, rval = 0; static bool hexIn = true;
    ImGui::SetNextItemWidth(120); ImGui::InputInt("Offset", &roff, 1, 16,
        hexIn ? ImGuiInputTextFlags_CharsHexadecimal : 0);
    ImGui::SameLine(); ImGui::Checkbox("hex offset", &hexIn);
    ImGui::SameLine(); ImGui::SetNextItemWidth(120);
    ImGui::Combo("Width", &rwidth, "1 byte\0" "2 bytes\0" "4 bytes\0");
    int w = rwidth==0?1:rwidth==1?2:4;
    if (roff < 0) roff = 0;
    if (ImGui::Button("Read")) rval = (int)g_save.ReadAt((size_t)roff, w);
    ImGui::SameLine(); ImGui::SetNextItemWidth(160); ImGui::InputInt("Value", &rval);
    ImGui::SameLine(); if (ImGui::Button("Write")) { if (g_save.WriteAt((size_t)roff, w, (uint32_t)rval)) snprintf(g_status,sizeof g_status,"Wrote %d (%d bytes) @ 0x%X",rval,w,roff); }
    ImGui::TextDisabled("Reads/writes little-endian. Offset is absolute (add slot*%d for DMC1).", g_save.slotSize());

    // hex viewer (clipped — handles the 12MB DMC5 save fine)
    ImGui::Spacing();
    if (ImGui::CollapsingHeader("Hex view")) {
        const auto& d = g_save.data();
        ImGui::BeginChild("svhex", ImVec2(0, 260), true, ImGuiWindowFlags_HorizontalScrollbar);
        ImGuiListClipper clip; clip.Begin((int)((d.size()+15)/16));
        while (clip.Step()) for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            size_t base = (size_t)row*16; char line[128]; int n=0;
            n += snprintf(line+n, sizeof line-n, "%06zX  ", base);
            for (int i=0;i<16;++i){ if(base+i<d.size()) n+=snprintf(line+n,sizeof line-n,"%02X ",d[base+i]); else n+=snprintf(line+n,sizeof line-n,"   "); }
            n += snprintf(line+n,sizeof line-n," ");
            for (int i=0;i<16 && base+i<d.size();++i){ uint8_t c=d[base+i]; line[n++]=(c>=32&&c<127)?(char)c:'.'; }
            line[n]=0; ImGui::TextUnformatted(line);
        }
        ImGui::EndChild();
    }

    // save
    ImGui::Spacing(); ImGui::Separator();
    if (!g_save.dirty()) ImGui::BeginDisabled();
    if (ImGui::Button("Save (writes .bak first)")) {
        std::string err;
        if (g_save.Save(err)) snprintf(g_status, sizeof g_status, "Saved %s (.bak kept)", g_save.path().c_str());
        else snprintf(g_status, sizeof g_status, "Save error: %s", err.c_str());
    }
    if (!g_save.dirty()) ImGui::EndDisabled();
    if (g_save.dirty()) { ImGui::SameLine(); ImGui::TextColored(theme::kRedHot, "[unsaved edits]"); }
}

static void DrawAboutTab() {
    ImGui::TextColored(theme::kRedHot, "DMCSEEDITOR ( Mistress Revamp )");
    ImGui::TextDisabled("Clean-room English rebuild of the RGBlue16 DMC effect-file editor.");
    ImGui::Spacing();
    ImGui::BulletText("Edits the real DMC4 .efl (Effect List) format - C++, no E-language runtime.");
    ImGui::BulletText("Texture/animation pointer swapping is verified against real arc samples.");
    ImGui::BulletText("Removed from the original: passphrase gate, MAC beacon, SMTP mailer.");
    ImGui::BulletText("Save writes in place (optional .bak); never deletes the source file.");
    ImGui::Spacing();
    ImGui::TextDisabled("Numeric params (colors/coords) are IEEE floats in the binary blocks;");
    ImGui::TextDisabled("map them with Manual Values + Hex Inspector against samples/ .efl files.");
}

// ---- main loop --------------------------------------------------------------
int main(int, char**) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, GetModuleHandle(nullptr),
                       nullptr, nullptr, nullptr, nullptr, L"RGBlueEN", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"DMCSEEDITOR ( Mistress Revamp )",
        WS_OVERLAPPEDWINDOW, 80, 80, 980, 680, nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) { CleanupDeviceD3D(); UnregisterClassW(wc.lpszClassName, wc.hInstance); return 1; }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    theme::Apply();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0u, 0u, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;
        if (g_DeviceLost) {
            HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST) { Sleep(10); continue; }
            if (hr == D3DERR_DEVICENOTRESET) ResetDevice();
            g_DeviceLost = false;
        }
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            g_d3dpp.BackBufferWidth = g_ResizeWidth;
            g_d3dpp.BackBufferHeight = g_ResizeHeight;
            g_ResizeWidth = g_ResizeHeight = 0;
            ResetDevice();
        }

        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Full-window panel
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open... (.arc / .pac / .efl / .lmt)")) {
                    std::string p = OpenFileDialog();
                    if (!p.empty()) OpenPath(p);
                }
                bool looseEfl = (g_active == AK_Efl && g_srcEntry < 0);
                if (ImGui::MenuItem("Save loose .efl", nullptr, false, looseEfl)) {
                    std::string err;
                    if (g_file.Save(g_file.path(), g_keepBackup, err)) snprintf(g_status, sizeof g_status, "Saved %s", g_file.path().c_str());
                    else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
                }
                bool anyArc = g_arc.loaded() && (g_arc.dirty() || HasUnstagedEdit());
                if (ImGui::MenuItem("Save ALL changes to Archive", "every edit at once", false, anyArc))
                    SaveAll();
                // restore the archive to its original (the .bak auto-made on first save)
                if (ImGui::MenuItem("Restore archive from backup (.bak)", nullptr, false, g_arc.loaded())) {
                    std::string p = g_arc.path(), bak = p + ".bak", err;
                    std::ifstream src(bak, std::ios::binary);
                    if (src.good()) {
                        { std::ofstream dst(p, std::ios::binary | std::ios::trunc); dst << src.rdbuf(); }
                        if (g_arc.Load(p, err)) { g_texSel.clear(); g_texEntry = -1; g_active = AK_None;
                            snprintf(g_status, sizeof g_status, "Restored original archive from %s", bak.c_str()); }
                        else snprintf(g_status, sizeof g_status, "Error: %s", err.c_str());
                    } else snprintf(g_status, sizeof g_status, "No backup found at %s", bak.c_str());
                }
                ImGui::MenuItem("Keep .bak backup (loose save)", nullptr, &g_keepBackup);
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) running = false;
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        if (ImGui::BeginTabBar("tabs")) {
            if (g_arc.loaded())
                if (ImGui::BeginTabItem("Archive"))  { DrawArchiveTab(); ImGui::EndTabItem(); }
            if (g_pac.loaded())
                if (ImGui::BeginTabItem("PAC"))      { DrawPacTab(); ImGui::EndTabItem(); }
            if (!g_sdlText.empty())
                if (ImGui::BeginTabItem("SDL"))      { DrawSdlTab(); ImGui::EndTabItem(); }
            if (g_active == AK_Efl) {
                if (ImGui::BeginTabItem("Effect Info"))  { DrawInfoTab();     ImGui::EndTabItem(); }
                if (ImGui::BeginTabItem("Pointers"))     { DrawPointersTab(); ImGui::EndTabItem(); }
            }
            if (g_active == AK_Lmt)
                if (ImGui::BeginTabItem("Motion (LMT)")) { DrawMotionTab();   ImGui::EndTabItem(); }
            if (g_arc.loaded() || g_pac.loaded())
                if (ImGui::BeginTabItem("Textures"))  { DrawTexturesTab(g_pd3dDevice); ImGui::EndTabItem(); }
            if (!g_gameRoot.empty())
                if (ImGui::BeginTabItem("Mods"))      { DrawModsTab(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("3D View"))      { DrawViewTab(g_pd3dDevice); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Manual Values")){ DrawManualTab();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Hex Inspector")){ DrawHexTab();      ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Save Editor"))  { DrawSaveTab();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("About"))        { DrawAboutTab();    ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        // status bar
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - ImGui::GetFrameHeightWithSpacing());
        ImGui::Separator();
        ImVec4 sc = (strncmp(g_status, "Error", 5) == 0) ? theme::kRedHot : theme::kTextDim;
        ImGui::TextColored(sc, "%s", g_status);
        if (g_file.dirty()) { ImGui::SameLine(); ImGui::TextColored(theme::kRedHot, "  [unsaved]"); }
        ImGui::End();

        ImGui::EndFrame();
        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        D3DCOLOR clear = D3DCOLOR_RGBA(10, 10, 12, 255);
        g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, clear, 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }
        HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
        if (result == D3DERR_DEVICELOST) g_DeviceLost = true;
    }

    view::Shutdown();
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// GUI subsystem entry — forward to main().
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(0, nullptr); }
