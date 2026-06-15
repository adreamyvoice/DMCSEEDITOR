// theme.h — black/red "Mistress" styling for the RGBlue effect editor.
// Matches the dmc4se-overlay aesthetic: near-black panels, crimson accents.
#pragma once
#include "imgui.h"

namespace theme {

// Palette
inline const ImVec4 kBlack   = ImVec4(0.04f, 0.04f, 0.05f, 1.00f);
inline const ImVec4 kPanel   = ImVec4(0.08f, 0.08f, 0.09f, 1.00f);
inline const ImVec4 kPanel2  = ImVec4(0.12f, 0.10f, 0.11f, 1.00f);
inline const ImVec4 kRed     = ImVec4(0.78f, 0.07f, 0.12f, 1.00f);
inline const ImVec4 kRedDim  = ImVec4(0.45f, 0.05f, 0.09f, 1.00f);
inline const ImVec4 kRedHot  = ImVec4(0.95f, 0.15f, 0.20f, 1.00f);
inline const ImVec4 kText    = ImVec4(0.90f, 0.90f, 0.92f, 1.00f);
inline const ImVec4 kTextDim = ImVec4(0.55f, 0.55f, 0.58f, 1.00f);

inline void Apply() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 4.0f;
    s.FrameRounding     = 3.0f;
    s.GrabRounding      = 3.0f;
    s.TabRounding       = 3.0f;
    s.ScrollbarRounding = 3.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(12, 12);
    s.FramePadding      = ImVec2(8, 5);
    s.ItemSpacing       = ImVec2(8, 7);

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]            = kText;
    c[ImGuiCol_TextDisabled]    = kTextDim;
    c[ImGuiCol_WindowBg]        = kBlack;
    c[ImGuiCol_ChildBg]         = kPanel;
    c[ImGuiCol_PopupBg]         = kPanel;
    c[ImGuiCol_Border]          = kRedDim;
    c[ImGuiCol_FrameBg]         = kPanel2;
    c[ImGuiCol_FrameBgHovered]  = kRedDim;
    c[ImGuiCol_FrameBgActive]   = kRed;
    c[ImGuiCol_TitleBg]         = kBlack;
    c[ImGuiCol_TitleBgActive]   = kRedDim;
    c[ImGuiCol_MenuBarBg]       = kPanel;
    c[ImGuiCol_Header]          = kRedDim;
    c[ImGuiCol_HeaderHovered]   = kRed;
    c[ImGuiCol_HeaderActive]    = kRedHot;
    c[ImGuiCol_Button]          = kRedDim;
    c[ImGuiCol_ButtonHovered]   = kRed;
    c[ImGuiCol_ButtonActive]    = kRedHot;
    c[ImGuiCol_CheckMark]       = kRedHot;
    c[ImGuiCol_SliderGrab]      = kRed;
    c[ImGuiCol_SliderGrabActive]= kRedHot;
    c[ImGuiCol_Separator]       = kRedDim;
    c[ImGuiCol_Tab]             = kPanel2;
    c[ImGuiCol_TabHovered]      = kRed;
    c[ImGuiCol_TabActive]       = kRedDim;
    c[ImGuiCol_TabUnfocused]    = kPanel;
    c[ImGuiCol_TabUnfocusedActive] = kPanel2;
    c[ImGuiCol_TableHeaderBg]   = kPanel2;
    c[ImGuiCol_TableBorderStrong] = kRedDim;
    c[ImGuiCol_TextSelectedBg]  = kRedDim;
}

} // namespace theme
