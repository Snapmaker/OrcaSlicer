#include "GLGizmoUtils.hpp"

#include "slic3r/GUI/ImGuiWrapper.hpp"

#include <algorithm>
#include <imgui.h>

namespace Slic3r::GUI::GLGizmoUtils {

void push_orca_button_style()
{
    const ImVec4 base_orca = ImGuiWrapper::COL_ORCA;

    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(base_orca.x, base_orca.y, base_orca.z, h, s, v);

    ImVec4 hover, active;
    // Lighter variant for Hover (Value up by ~12%)
    ImGui::ColorConvertHSVtoRGB(h, s, std::min(v + 0.12f, 1.0f), hover.x, hover.y, hover.z);
    hover.w = base_orca.w;
    // Darker variant for Active (Value down by ~12%)
    ImGui::ColorConvertHSVtoRGB(h, s, std::max(v - 0.12f, 0.0f), active.x, active.y, active.z);
    active.w = base_orca.w;

    ImGui::PushStyleColor(ImGuiCol_Button, base_orca);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::COL_WINDOW_BG);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
}

void pop_orca_button_style()
{
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(4);
}

} // namespace Slic3r::GUI::GLGizmoUtils
