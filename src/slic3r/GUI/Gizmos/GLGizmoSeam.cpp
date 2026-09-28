#include "GLGizmoSeam.hpp"

#include "libslic3r/Model.hpp"

//#include "slic3r/GUI/3DScene.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Jobs/SeamAutoPaintJob.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/PresetBundle.hpp"

#include <boost/format.hpp>

#include <algorithm>
#include <array>

#include <glad/gl.h>


namespace Slic3r::GUI {



void GLGizmoSeam::on_shutdown()
{
    m_parent.toggle_model_objects_visibility(true);
}



bool GLGizmoSeam::on_init()
{
    m_shortcut_key = WXK_CONTROL_P;

    // FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
    const wxString ctrl  = _L("Ctrl+");
    // FIXME: maybe should be using GUI::shortkey_alt_prefix() or equivalent?
    const wxString alt   = _L("Alt+");
    const wxString shift = _L("Shift+");

    m_desc["clipping_of_view_caption"] = alt + _L("Mouse wheel");
    m_desc["clipping_of_view"] = _L("Section view");
    m_desc["reset_direction"]  = _L("Reset direction");
    m_desc["cursor_size_caption"] = ctrl + _L("Mouse wheel");
    m_desc["cursor_size"]      = _L("Brush size");
    m_desc["cursor_type"]      = _L("Brush shape");
    m_desc["enforce_caption"]  = _L("Left mouse button");
    m_desc["enforce"]          = _L("Enforce seam");
    m_desc["block_caption"]    = _L("Right mouse button");
    m_desc["block"]            = _L("Block seam");
    m_desc["remove_caption"]   = shift + _L("Left mouse button");
    m_desc["remove"]           = _L("Erase");
    m_desc["remove_all"]       = _L("Erase all painting");
    m_desc["circle"]           = _L("Circle");
    m_desc["sphere"]           = _L("Sphere");

    return true;
}

GLGizmoSeam::GLGizmoSeam(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id)
    : GLGizmoPainterBase(parent, icon_filename, sprite_id), m_current_tool(ImGui::CircleButtonIcon)
{

}


std::string GLGizmoSeam::on_get_name() const
{
    return _u8L("Seam painting");
}



void GLGizmoSeam::render_painter_gizmo()
{
    const Selection& selection = m_parent.get_selection();

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glEnable(GL_DEPTH_TEST));

    render_triangles(selection);

    m_c->object_clipper()->render_cut();
    m_c->instances_hider()->render_cut();
    render_cursor();

    glsafe(::glDisable(GL_BLEND));
}

// BBS
bool GLGizmoSeam::on_key_down_select_tool_type(int keyCode) {
    switch (keyCode)
    {
    case 'S':
        m_current_tool = ImGui::SphereButtonIcon;
        break;
    case 'C':
        m_current_tool = ImGui::CircleButtonIcon;
        break;
    default:
        return false;
        break;
    }
    return true;
}

void GLGizmoSeam::show_tooltip_information(float caption_max, float x, float y)
{
    ImTextureID normal_id = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_parent.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += m_imgui->calc_text_size(std::string_view{": "}).x + 35.f;

    float  scale       = m_parent.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, 0}); // ORCA: Dont add padding
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &caption_max](const wxString &caption, const wxString &text) {
            m_imgui->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            m_imgui->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 5>{"enforce", "block", "remove", "cursor_size", "clipping_of_view"}) draw_text_with_caption(m_desc.at(t + "_caption") + ": ", m_desc.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GLGizmoSeam::tool_changed(wchar_t old_tool, wchar_t new_tool)
{
    if ((old_tool == ImGui::GapFillIcon && new_tool == ImGui::GapFillIcon) ||
        (old_tool != ImGui::GapFillIcon && new_tool != ImGui::GapFillIcon))
        return;

    for (auto& selector_ptr : m_triangle_selectors) {
        TriangleSelectorPatch* tsp = dynamic_cast<TriangleSelectorPatch*>(selector_ptr.get());
        tsp->set_filter_state(new_tool == ImGui::GapFillIcon);
    }
}

void GLGizmoSeam::on_render_input_window(float x, float y, float bottom_limit)
{
    if (! m_c->selection_info()->model_object())
        return;

    dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);

    wchar_t old_tool = m_current_tool;
    //BBS
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());

    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar));
    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    // First calculate width of all the texts that are could possibly be shown. We will decide set the dialog width based on that:
    const float space_size = m_imgui->get_style_scaling() * 8;
    const float clipping_slider_left = std::max(m_imgui->calc_text_size(m_desc.at("clipping_of_view")).x,
                                                m_imgui->calc_text_size(m_desc.at("reset_direction")).x + ImGui::GetStyle().FramePadding.x * 2)
                                           + m_imgui->scaled(1.5f);
    const float cursor_size_slider_left = m_imgui->calc_text_size(m_desc.at("cursor_size")).x + m_imgui->scaled(1.f);
    const float empty_button_width      = m_imgui->calc_button_size("").x;

    float caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 6>{"enforce", "block", "remove", "cursor_size", "clipping_of_view"}) {
        caption_max    = std::max(caption_max, m_imgui->calc_text_size(m_desc[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, m_imgui->calc_text_size(m_desc[t]).x);
    }

    const float sliders_left_width = std::max(cursor_size_slider_left, clipping_slider_left);
    const float slider_icon_width  = m_imgui->get_slider_icon_size().x;

    const float sliders_width = m_imgui->scaled(7.0f);
    const float drag_left_width = ImGui::GetStyle().WindowPadding.x + sliders_left_width + sliders_width - space_size;

    const float max_tooltip_width = ImGui::GetFontSize() * 20.0f;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_type"));
    std::array<wchar_t, 2> tool_ids = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon };
    std::array<wchar_t, 2> icons;
    if (m_is_dark_mode)
        icons = { ImGui::CircleButtonDarkIcon, ImGui::SphereButtonDarkIcon};
    else
        icons = { ImGui::CircleButtonIcon, ImGui::SphereButtonIcon };
    std::array<wxString, 2> tool_tips = { _L("Circle"), _L("Sphere")};
    for (int i = 0; i < tool_ids.size(); i++) {
        std::string  str_label = std::string("##");
        std::wstring btn_name = icons[i] + boost::nowide::widen(str_label);

        if (i != 0) ImGui::SameLine((empty_button_width + m_imgui->scaled(1.75f)) * i + m_imgui->scaled(1.3f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.f, 0.f, 0.f, 0.f));                     // ORCA Removes button background on dark mode
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));                       // ORCA: Fixes icon rendered without colors while using Light theme
        if (m_current_tool == tool_ids[i]) {
            ImGui::PushStyleColor(ImGuiCol_Button,          ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,   ImVec4(0.f, 0.59f, 0.53f, 0.25f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,    ImVec4(0.f, 0.59f, 0.53f, 0.30f));  // ORCA use orca color for selected tool / brush
            ImGui::PushStyleColor(ImGuiCol_Border,          ImGuiWrapper::COL_ORCA);            // ORCA use orca color for border on selected tool / brush
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 1.0);
        }
        bool btn_clicked = ImGui::Button(into_u8(btn_name).c_str());
        if (m_current_tool == tool_ids[i])
        {
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar(2);
        }
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar(1);
        if (btn_clicked && m_current_tool != tool_ids[i]) {
            m_current_tool = tool_ids[i];
            for (auto& triangle_selector : m_triangle_selectors) {
                triangle_selector->seed_fill_unselect_all_triangles();
                triangle_selector->request_update_render_data();
            }
        }

        if (ImGui::IsItemHovered()) {
            m_imgui->tooltip(tool_tips[i], max_tooltip_width);
        }
    }

    if (m_current_tool != old_tool)
        this->tool_changed(old_tool, m_current_tool);

    ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.1));

    if (m_current_tool == ImGui::CircleButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::CIRCLE;
        m_tool_type = ToolType::BRUSH;
    } else if (m_current_tool == ImGui::SphereButtonIcon) {
        m_cursor_type = TriangleSelector::CursorType::SPHERE;
        m_tool_type = ToolType::BRUSH;
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("cursor_size"));
    ImGui::SameLine(sliders_left_width);

    ImGui::PushItemWidth(sliders_width);
    m_imgui->bbl_slider_float_style("##cursor_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f", 1.0f, true);
    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    ImGui::BBLDragFloat("##cursor_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");

    ImGui::Separator();
    if (m_c->object_clipper()->get_position() == 0.f) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clipping_of_view"));
    }
    else {
        if (m_imgui->button(m_desc.at("reset_direction"))) {
            wxGetApp().CallAfter([this](){
                    m_c->object_clipper()->set_position_by_ratio(-1., false);
                });
        }
    }

    auto clp_dist = float(m_c->object_clipper()->get_position());
    ImGui::SameLine(sliders_left_width);

    ImGui::PushItemWidth(sliders_width);
    bool slider_clp_dist = m_imgui->bbl_slider_float_style("##clp_dist", &clp_dist, 0.f, 1.f, "%.2f", 1.0f, true);

    ImGui::SameLine(drag_left_width);
    ImGui::PushItemWidth(1.5 * slider_icon_width);
    bool b_clp_dist_input = ImGui::BBLDragFloat("##clp_dist_input", &clp_dist, 0.05f, 0.0f, 0.0f, "%.2f");
    if (slider_clp_dist || b_clp_dist_input) { m_c->object_clipper()->set_position_by_ratio(clp_dist, true); }

    ImGui::Separator();
    m_imgui->bbl_checkbox(_L("Vertical"), m_vertical_only);

    ImGui::Separator();
    render_auto_paint_section(sliders_left_width, sliders_width, drag_left_width, 1.5f * slider_icon_width, max_tooltip_width);

    ImGui::Separator();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 10.0f));
    float get_cur_y = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    show_tooltip_information(caption_max, x, get_cur_y);

    float f_scale =m_parent.get_gizmos_manager().get_layout_scale();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f * f_scale));

    ImGui::SameLine();

    if (m_imgui->button(m_desc.at("remove_all"))) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset selection", UndoRedo::SnapshotType::GizmoAction);
        ModelObject         *mo  = m_c->selection_info()->model_object();
        int                  idx = -1;
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part()) {
                ++idx;
                m_triangle_selectors[idx]->reset();
                m_triangle_selectors[idx]->request_update_render_data(true);
            }

        update_model_object();
        m_parent.set_as_dirty();
    }
    ImGui::PopStyleVar(2);
    GizmoImguiEnd();

    //BBS
    ImGuiWrapper::pop_toolbar_style();
}

// BBS
void GLGizmoSeam::on_set_state()
{
    GLGizmoPainterBase::on_set_state();

    if (get_state() == Off) {
        ModelObject* mo = m_c->selection_info()->model_object();
        if (mo) Slic3r::save_object_mesh(*mo);
    }
}

//BBS: remove const
void GLGizmoSeam::update_model_object()
{
    bool updated = false;
    ModelObject* mo = m_c->selection_info()->model_object();
    int idx = -1;
    for (ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;
        ++idx;
        updated |= mv->seam_facets.set(*m_triangle_selectors[idx].get());
    }

    if (updated) {
        const ModelObjectPtrs& mos = wxGetApp().model().objects;
        wxGetApp().obj_list()->update_info_items(std::find(mos.begin(), mos.end(), mo) - mos.begin());
        m_parent.post_event(SimpleEvent(EVT_GLCANVAS_SCHEDULE_BACKGROUND_PROCESS));
    }
}


//BBS: add logic to distinguish the first_time_update and later_update
void GLGizmoSeam::update_from_model_object(bool first_update)
{
    wxBusyCursor wait;

    const ModelObject* mo = m_c->selection_info()->model_object();
    m_triangle_selectors.clear();

    int volume_id = -1;
    std::vector<ColorRGBA> ebt_colors;
    ebt_colors.push_back(GLVolume::NEUTRAL_COLOR);
    ebt_colors.push_back(TriangleSelectorGUI::enforcers_color);
    ebt_colors.push_back(TriangleSelectorGUI::blockers_color);
    for (const ModelVolume* mv : mo->volumes) {
        if (! mv->is_model_part())
            continue;

        ++volume_id;

        // This mesh does not account for the possible Z up SLA offset.
        const TriangleMesh* mesh = &mv->mesh();

        m_triangle_selectors.emplace_back(std::make_unique<TriangleSelectorPatch>(*mesh, ebt_colors));
        // Reset of TriangleSelector is done inside TriangleSelectorGUI's constructor, so we don't need it to perform it again in deserialize().
        m_triangle_selectors.back()->deserialize(mv->seam_facets.get_data(), false);
        m_triangle_selectors.back()->request_update_render_data();
    }
}


PainterGizmoType GLGizmoSeam::get_painter_type() const
{
    return PainterGizmoType::SEAM;
}

// ---------------------------------------------------------------------------------------------------------------
// Auto-paint seam

namespace {
struct AutoPaintMode
{
    SeamPosition mode;
    const char  *label;
};
// The Aligned family, in the order of the seam position list.
const std::array<AutoPaintMode, 5> &auto_paint_modes()
{
    static const std::array<AutoPaintMode, 5> modes = { { { spAligned, L("Aligned") },
                                                           { spAlignedBack, L("Aligned back") },
                                                           { spAlignedFront, L("Aligned front") },
                                                           { spLeft, L("Aligned left") },
                                                           { spRight, L("Aligned right") } } };
    return modes;
}
} // namespace

// The object's settings as the slicer sees them: its own value if it has one, else the print preset's.
void GLGizmoSeam::load_auto_paint_defaults(const ModelObject &mo)
{
    const DynamicPrintConfig &preset  = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    const DynamicPrintConfig &printer = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    auto option = [&mo, &preset](const char *key) -> const ConfigOption * {
        if (const ConfigOption *opt = mo.config.option(key))
            return opt;
        return preset.option(key);
    };

    m_autopaint_mode = 0;
    if (const ConfigOption *opt = option("seam_position")) {
        const auto &modes = auto_paint_modes();
        for (size_t i = 0; i < modes.size(); ++i)
            if (opt->getInt() == int(modes[i].mode))
                m_autopaint_mode = int(i);
    }
    const ConfigOption *joints = option("seam_prefer_part_joints");
    m_autopaint_joints         = joints == nullptr || joints->getBool();

    // The outer wall line width, resolved like Flow does: a percentage is of the nozzle diameter, zero falls back to
    // the general line width. Only for the labels: the painter uses each loop's real width.
    double nozzle = 0.4;
    if (const auto *nozzles = printer.option<ConfigOptionFloats>("nozzle_diameter"); nozzles != nullptr && !nozzles->values.empty())
        nozzle = nozzles->get_at(0);
    auto resolve = [nozzle](const ConfigOption *opt) {
        const auto *fop = dynamic_cast<const ConfigOptionFloatOrPercent *>(opt);
        return fop == nullptr ? 0. : fop->get_abs_value(nozzle);
    };
    double width = resolve(option("outer_wall_line_width"));
    if (width <= 0.)
        width = resolve(option("line_width"));
    if (width <= 0.)
        width = 1.125 * nozzle;
    m_autopaint_line_width = float(width);
    m_autopaint_width      = float(2. * width);
}

void GLGizmoSeam::render_auto_paint_section(float label_width, float control_width, float drag_left_width, float drag_width,
                                            float max_tooltip_width)
{
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    if (mo->id() != m_autopaint_defaults_of) {
        load_auto_paint_defaults(*mo);
        m_autopaint_defaults_of = mo->id();
    }

    ImGui::AlignTextToFramePadding();
    m_imgui->text(_L("Auto-paint seam"));

    const auto              &modes = auto_paint_modes();
    std::vector<std::string> labels;
    for (const AutoPaintMode &mode : modes)
        labels.push_back(_u8L(mode.label));
    ImGui::AlignTextToFramePadding();
    ImGuiWrapper::push_combo_style(m_parent.get_scale());
    m_imgui->combo(_L("Mode"), labels, m_autopaint_mode, 0, label_width, control_width);
    ImGuiWrapper::pop_combo_style();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Where the seam goes, as with the Aligned seam positions: Aligned hides it in the least "
                            "visible place, the others prefer that side of the bed."),
                         max_tooltip_width);

    m_imgui->bbl_checkbox(_L("Prefer part joints"), m_autopaint_joints);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Put the seam on the line where two parts of the object, or two touching objects, meet."),
                         max_tooltip_width);

    m_imgui->bbl_checkbox(_L("Automatic strip width"), m_autopaint_auto_width);
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(from_u8((boost::format(_u8L("Paint a strip twice as wide as the outer wall line (%1$.2f mm).")) %
                                  (2.f * m_autopaint_line_width)).str()),
                         max_tooltip_width);
    if (!m_autopaint_auto_width) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(_L("Strip width"));
        ImGui::SameLine(label_width);
        ImGui::PushItemWidth(control_width);
        m_imgui->bbl_slider_float_style("##autopaint_width", &m_autopaint_width, 0.2f, 5.f, "%.2f", 1.0f, true);
        ImGui::PopItemWidth();
        ImGui::SameLine(drag_left_width);
        ImGui::PushItemWidth(drag_width);
        ImGui::BBLDragFloat("##autopaint_width_input", &m_autopaint_width, 0.05f, 0.0f, 0.0f, "%.2f");
        ImGui::PopItemWidth();
        m_autopaint_width = std::clamp(m_autopaint_width, 0.2f, 5.f);
    }

    const std::string replace_label = _u8L("Replace existing paint");
    const std::string add_label     = _u8L("Add to existing");
    if (m_imgui->bbl_radio_button(replace_label.c_str(), m_autopaint_replace))
        m_autopaint_replace = true;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Erase all seam painting of the object (enforcers and blockers) first."), max_tooltip_width);
    ImGui::SameLine();
    if (m_imgui->bbl_radio_button(add_label.c_str(), !m_autopaint_replace))
        m_autopaint_replace = false;
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Keep the existing painting; the seams are placed as the slicer would place them with it."),
                         max_tooltip_width);

    const bool idle = wxGetApp().plater()->get_ui_job_worker().is_idle();
    if (m_imgui->button(_L("Auto-paint"), ImVec2(0.f, 0.f), idle))
        start_auto_paint();
    if (ImGui::IsItemHovered())
        m_imgui->tooltip(_L("Paint seam enforcers where the slicer would put the seam in this mode. The object's own seam "
                            "settings are not changed. Uses the sliced plate when it is up to date, otherwise slices the "
                            "object's walls in the background."),
                         max_tooltip_width);
}

void GLGizmoSeam::start_auto_paint()
{
    Plater *plater = wxGetApp().plater();
    Worker &worker = plater->get_ui_job_worker();
    if (!worker.is_idle())
        return;
    ModelObject *mo = m_c->selection_info()->model_object();
    if (mo == nullptr)
        return;
    const Selection &selection = m_parent.get_selection();
    const int        obj_idx   = selection.get_object_idx();
    int              inst_idx  = selection.get_instance_idx();
    const Model     &model     = plater->model();
    if (obj_idx < 0 || size_t(obj_idx) >= model.objects.size() || model.objects[size_t(obj_idx)] != mo)
        return;
    if (inst_idx < 0 || size_t(inst_idx) >= mo->instances.size())
        inst_idx = 0;

    const auto          &modes = auto_paint_modes();
    SeamAutoPaintRequest request;
    request.object_id          = mo->id();
    request.instance_id        = mo->instances[size_t(inst_idx)]->id();
    request.mode               = modes[size_t(std::clamp(m_autopaint_mode, 0, int(modes.size()) - 1))].mode;
    request.prefer_part_joints = m_autopaint_joints;
    request.replace            = m_autopaint_replace;
    request.strip_width        = m_autopaint_auto_width ? 0.f : m_autopaint_width;
    int idx = -1;
    for (const ModelVolume *mv : mo->volumes) {
        if (!mv->is_model_part())
            continue;
        ++idx;
        if (size_t(idx) >= m_triangle_selectors.size())
            return;
        request.parts.push_back({ mv->id(), mv->mesh_ptr(), m_triangle_selectors[size_t(idx)]->serialize() });
    }
    if (request.parts.empty())
        return;

    SeamAutoPaintJob::Finish finish = [this](SeamAutoPaintResult &&result) { apply_auto_paint(std::move(result)); };
    const Print *print = nullptr;
    if (const PrintObject *po = SeamAutoPaintJob::sliced_print_object(plater, obj_idx, inst_idx, !request.replace, &print)) {
        queue_job(worker, std::make_unique<SeamAutoPaintJob>(std::move(request), print, po, std::move(finish)));
        return;
    }
    DynamicPrintConfig     config;
    std::unique_ptr<Model> copy = SeamAutoPaintJob::private_model(plater, obj_idx, inst_idx, request.prefer_part_joints, config);
    if (!copy)
        return;
    queue_job(worker, std::make_unique<SeamAutoPaintJob>(std::move(request), std::move(copy), std::move(config), std::move(finish)));
}

void GLGizmoSeam::apply_auto_paint(SeamAutoPaintResult &&result)
{
    NotificationManager       *notifications = wxGetApp().notification_manager();
    ModelObject               *mo            = get_state() == On ? m_c->selection_info()->model_object() : nullptr;
    std::vector<ModelVolume *> parts;
    if (mo != nullptr && mo->id() == result.object_id)
        for (ModelVolume *mv : mo->volumes)
            if (mv->is_model_part())
                parts.push_back(mv);
    bool matches = !parts.empty() && parts.size() == result.volume_ids.size() && parts.size() == result.paint.size() &&
                   parts.size() == m_triangle_selectors.size();
    for (size_t i = 0; matches && i < parts.size(); ++i)
        matches = parts[i]->id() == result.volume_ids[i];
    if (!matches) {
        notifications->push_plater_warning_notification(
            _u8L("Auto-paint seam was discarded: the seam painting tool was closed or the object changed."));
        return;
    }

    // One undo step for the whole result, like "Erase all painting".
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Auto-paint seam"), UndoRedo::SnapshotType::GizmoAction);
    for (size_t i = 0; i < parts.size(); ++i) {
        m_triangle_selectors[i]->deserialize(result.paint[i]);
        m_triangle_selectors[i]->request_update_render_data(true);
    }
    update_model_object();
    m_parent.set_as_dirty();

    if (result.seams == 0) {
        notifications->push_plater_warning_notification(_u8L("Auto-paint seam found no outer wall to paint a seam on."));
    } else {
        const std::string source = result.from_sliced_plate ? _u8L("from the sliced plate") : _u8L("from a background slice of the walls");
        notifications->push_notification(NotificationType::CustomNotification,
                                         NotificationManager::NotificationLevel::RegularNotificationLevel,
                                         (boost::format(_u8L("Auto-paint seam painted %1% seams (%2%).")) % result.seams % source).str());
    }
}

wxString GLGizmoSeam::handle_snapshot_action_name(bool shift_down, GLGizmoPainterBase::Button button_down) const
{
    wxString action_name;
    if (shift_down)
        action_name = _L("Remove selection");
    else {
        if (button_down == Button::Left)
            action_name = _L("Enforce seam");
        else
            action_name = _L("Block seam");
    }
    return action_name;
}

} // namespace Slic3r::GUI
