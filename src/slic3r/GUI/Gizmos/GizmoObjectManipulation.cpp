#include "slic3r/GUI/ImGuiWrapper.hpp"
#include <imgui/imgui_internal.h>

#include "GizmoObjectManipulation.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
//#include "I18N.hpp"
#include "GLGizmosManager.hpp"
#include "GLGizmoMove.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/UndoRedo.hpp"
#include "libslic3r/AppConfig.hpp"

#include "libslic3r/Model.hpp"
#include "libslic3r/Geometry.hpp"
#include "slic3r/GUI/Selection.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/MainFrame.hpp"

#include <boost/algorithm/string.hpp>

#define MAX_NUM 9999.99
#define MAX_SIZE std::string_view{"9999.99"}

namespace Slic3r
{
namespace GUI
{

const double GizmoObjectManipulation::in_to_mm = 25.4;
const double GizmoObjectManipulation::mm_to_in = 0.0393700787;
const double GizmoObjectManipulation::oz_to_g = 28.34952;
const double GizmoObjectManipulation::g_to_oz = 0.035274;

// Helper function to be used by drop to bed button. Returns lowest point of this
// volume in world coordinate system.
static double get_volume_min_z(const GLVolume* volume)
{
    const Transform3f& world_matrix = volume->world_matrix().cast<float>();

    // need to get the ModelVolume pointer
    const ModelObject* mo = wxGetApp().model().objects[volume->composite_id.object_id];
    const ModelVolume* mv = mo->volumes[volume->composite_id.volume_id];
    const TriangleMesh& hull = mv->get_convex_hull();

    float min_z = std::numeric_limits<float>::max();
    for (const stl_vertex& vert : hull.its.vertices) {
        min_z = std::min(min_z, Vec3f::UnitZ().dot(world_matrix * vert));
    }
    return min_z;
}

GizmoObjectManipulation::GizmoObjectManipulation(GLCanvas3D& glcanvas)
    : m_glcanvas(glcanvas)
{
    m_alignment_helper = new GLGizmoAlignment(glcanvas);
    m_imperial_units = wxGetApp().app_config->get("use_inches") == "1";
    m_new_unit_string = m_imperial_units ? L("in") : L("mm");

    const wxString shift                   = _L("Shift+");
    const wxString alt                     = GUI::shortkey_alt_prefix();
    const wxString ctrl                    = GUI::shortkey_ctrl_prefix();

    m_desc_move["part_selection_caption"] = alt + _L("Left mouse button");
    m_desc_move["part_selection"]         = _L("Part selection");
    m_desc_move["snap_step_caption"] = shift + _L("Left mouse button");
    m_desc_move["snap_step"]        = _L("Fixed step drag");

    m_desc_rotate["part_selection_caption"] = alt + _L("Left mouse button");
    m_desc_rotate["part_selection"]         = _L("Part selection");

    m_desc_scale["part_selection_caption"] = alt + _L("Left mouse button");
    m_desc_scale["part_selection"]         = _L("Part selection");
    m_desc_scale["snap_step_caption"]      = shift + _L("Left mouse button");
    m_desc_scale["snap_step"]              = _L("Fixed step drag");
    m_desc_scale["single_sided_caption"] = ctrl + _L("Left mouse button");
    m_desc_scale["single_sided"]         = _L("Single sided scaling");
}

void GizmoObjectManipulation::UpdateAndShow(const bool show)
{
	if (show) {
        this->set_dirty();
		this->update_if_dirty();
	}
}

void GizmoObjectManipulation::update_ui_from_settings()
{
    if (m_imperial_units != (wxGetApp().app_config->get("use_inches") == "1")) {
        m_imperial_units = wxGetApp().app_config->get("use_inches") == "1";

        m_new_unit_string = m_imperial_units ? L("in") : L("mm");

        update_buffered_value();
    }
}
void delete_negative_sign(Vec3d& value) {
    for (size_t i = 0; i < value.size(); i++) {
        if (abs(value[i]) < 0.001)
            value[i] = 0.f;
    }
}

void GizmoObjectManipulation::update_settings_value(const Selection &selection)
{
	m_new_move_label_string   = L("Position");
    m_new_rotate_label_string = L("Rotate (relative)");
    m_new_rotation            = Vec3d::Zero();
    m_new_absolute_rotation   = Vec3d::Zero();
    m_new_scale_label_string  = L("Scale ratios");

    ObjectList* obj_list = wxGetApp().obj_list();
    if (selection.is_single_full_instance()) {
        // all volumes in the selection belongs to the same instance, any of them contains the needed instance data, so we take the first one
        const GLVolume* volume = selection.get_first_volume();
        m_new_position = volume->get_instance_offset();
        auto rotation = volume->get_instance_transformation().get_rotation_by_quaternion();
        m_new_absolute_rotation = rotation * (180. / M_PI);
        delete_negative_sign(m_new_absolute_rotation);
        if (is_world_coordinates()) {//for move and rotate
            m_new_size     = selection.get_bounding_box_in_current_reference_system().first.size();
            m_unscale_size = selection.get_unscaled_instance_bounding_box().size();
            m_new_scale    = m_new_size.cwiseQuotient(m_unscale_size) * 100.0;
		}
        else {//if (is_local_coordinates()) {//for scale
            auto tran      = selection.get_first_volume()->get_instance_transformation();
            m_new_position = tran.get_matrix().inverse() * cs_center;
            m_new_size  = selection.get_bounding_box_in_current_reference_system().first.size();
            m_unscale_size = selection.get_full_unscaled_instance_local_bounding_box().size();
            m_new_scale    = m_new_size.cwiseQuotient(m_unscale_size) * 100.0;
		}

        m_new_enabled  = true;
        // BBS: change "Instance Operations" to "Object Operations"
        m_new_title_string = L("Object Operations");
    }
    else if (selection.is_single_full_object() && obj_list->is_selected(itObject)) {
        const BoundingBoxf3& box = selection.get_bounding_box();
        m_new_position = box.center();
        m_new_scale    = Vec3d(100., 100., 100.);
        m_new_size     = selection.get_bounding_box_in_current_reference_system().first.size();
		m_new_scale_label_string  = L("Scale");
        m_new_enabled  = true;
        m_new_title_string = L("Object Operations");
    } else if (selection.is_single_volume_or_modifier()) {
        const GLVolume *volume = selection.get_first_volume();
        auto            rotation = volume->get_volume_transformation().get_rotation_by_quaternion();
        m_new_absolute_rotation  = rotation * (180. / M_PI);
        delete_negative_sign(m_new_absolute_rotation);
        if (is_world_coordinates()) {//for move and rotate
            const Geometry::Transformation trafo(volume->world_matrix());
            const Vec3d &offset = trafo.get_offset();
            m_new_position            = offset;
            m_new_scale               = Vec3d(100.0, 100.0, 100.0);
            m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
            m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        } else if (is_local_coordinates()) {//for scale
            m_new_position            = Vec3d::Zero();
            m_new_scale               = volume->get_volume_scaling_factor() * 100.0;
            m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
            m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        } else {
            m_new_position            = volume->get_volume_offset();
            m_new_scale_label_string  = L("Scale");
            m_new_scale               = Vec3d(100.0, 100.0, 100.0);
            m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
            m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        }
        m_new_enabled = true;
        m_new_title_string = L("Volume Operations");
    } else if (obj_list->is_connectors_item_selected() || obj_list->multiple_selection() || obj_list->is_selected(itInstanceRoot)) {
        reset_settings_value();
		m_new_move_label_string   = L("Translate");
		m_new_scale_label_string  = L("Scale");
        m_unscale_size            = selection.get_bounding_box_in_current_reference_system().first.size();
        m_new_size                = selection.get_bounding_box_in_current_reference_system().first.size();
        m_new_enabled  = true;
        m_new_title_string = L("Group Operations");
    } else if (selection.is_wipe_tower()) {
        const BoundingBoxf3 &box = selection.get_bounding_box();
        m_new_position           = box.center();
    }
	else {
        // No selection, reset the cache.
//		assert(selection.is_empty());
		reset_settings_value();
	}
}

void GizmoObjectManipulation::update_buffered_value()
{
    if (this->m_imperial_units)
        m_buffered_position = this->m_new_position * this->mm_to_in;
    else
        m_buffered_position = this->m_new_position;

    m_buffered_rotation = this->m_new_rotation;
    m_buffered_absolute_rotation = this->m_new_absolute_rotation;
    m_buffered_scale = this->m_new_scale;

    if (this->m_imperial_units)
        m_buffered_size = this->m_new_size * this->mm_to_in;
    else
        m_buffered_size = this->m_new_size;
}

void GizmoObjectManipulation::update_if_dirty()
{
    if (! m_dirty)
        return;

    const Selection &selection = m_glcanvas.get_selection();
    this->update_settings_value(selection);
    this->update_buffered_value();

    auto update_label = [](wxString &label_cache, const std::string &new_label) {
        wxString new_label_localized = _(new_label) + ":";
        if (label_cache != new_label_localized) {
            label_cache = new_label_localized;
        }
    };
    update_label(m_cache.move_label_string,   m_new_move_label_string);
    update_label(m_cache.rotate_label_string, m_new_rotate_label_string);
    update_label(m_cache.rotate_label_string, m_new_rotate_label_string);
    update_label(m_cache.scale_label_string,  m_new_scale_label_string);

    enum ManipulationEditorKey
    {
        mePosition = 0,
        meRotation,
        meScale,
        meSize
    };

    for (int i = 0; i < 3; ++ i) {
        auto update = [this, i](Vec3d &cached, Vec3d &cached_rounded,  const Vec3d &new_value) {
			//wxString new_text = double_to_string(new_value(i), 2);
			double new_rounded = round(new_value(i)*100)/100.0;
			//new_text.ToDouble(&new_rounded);
			if (std::abs(cached_rounded(i) - new_rounded) > EPSILON) {
				cached_rounded(i) = new_rounded;
                //const int id = key_id*3+i;
                //if (m_imperial_units && (key_id == mePosition || key_id == meSize))
                //    new_text = double_to_string(new_value(i)*mm_to_in, 2);
                //if (id >= 0) m_editors[id]->set_value(new_text);
            }
			cached(i) = new_value(i);
		};
        update(m_cache.position, m_cache.position_rounded,  m_new_position);
        update(m_cache.scale,    m_cache.scale_rounded,     m_new_scale);
        update(m_cache.size,     m_cache.size_rounded,      m_new_size);
        update(m_cache.rotation, m_cache.rotation_rounded,  m_new_rotation);
        update(m_cache.absolute_rotation, m_cache.absolute_rotation_rounded, m_new_absolute_rotation);
    }

    update_reset_buttons_visibility();
    //update_mirror_buttons_visibility();

    m_dirty = false;
}

void GizmoObjectManipulation::update_reset_buttons_visibility()
{
    const Selection& selection = m_glcanvas.get_selection();

    if (selection.is_single_full_instance() || selection.is_single_volume_or_modifier()) {
        const GLVolume *               volume = selection.get_first_volume();

        Vec3d rotation;
        Vec3d scale;
        double min_z = 0.;

        if (selection.is_single_full_instance()) {
            rotation = volume->get_instance_rotation();
            scale = volume->get_instance_scaling_factor();
        }
        else {
            rotation = volume->get_volume_rotation();
            scale = volume->get_volume_scaling_factor();
            min_z = get_volume_min_z(volume);
        }
        m_show_clear_rotation = !rotation.isApprox(m_init_rotation);
        m_show_reset_0_rotation = !rotation.isApprox(Vec3d::Zero());
        m_show_clear_scale = (m_cache.scale / 100.0f - Vec3d::Ones()).norm() > 0.001;
        m_show_drop_to_bed = (std::abs(min_z) > EPSILON);
    }
}


void GizmoObjectManipulation::reset_settings_value()
{
    m_new_position = Vec3d::Zero();
    m_new_rotation = Vec3d::Zero();
    m_new_absolute_rotation = Vec3d::Zero();
    m_new_scale = Vec3d::Ones() * 100.;
    m_new_size = Vec3d::Zero();
    m_new_enabled = false;
    // no need to set the dirty flag here as this method is called from update_settings_value(),
    // which is called from update_if_dirty(), which resets the dirty flag anyways.
//    m_dirty = true;
}

void GizmoObjectManipulation::change_position_value(int axis, double value)
{
    if (std::abs(m_cache.position_rounded(axis) - value) < EPSILON)
        return;

    Vec3d position = m_cache.position;
    position(axis) = value;

    Selection& selection = m_glcanvas.get_selection();
    selection.setup_cache();
    TransformationType trafo_type;
    trafo_type.set_relative();
    switch (m_coordinates_type) {
    case ECoordinatesType::Instance: {
        trafo_type.set_instance();
        break;
    }
    case ECoordinatesType::Local: {
        trafo_type.set_local();
        break;
    }
    default: {
        break;
    }
    }
    selection.translate(position - m_cache.position, trafo_type);
    wxGetApp().plater()->take_snapshot("Set Position", UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_move("");

    m_cache.position = position;
	m_cache.position_rounded(axis) = DBL_MAX;
    this->UpdateAndShow(true);
}

void GizmoObjectManipulation::change_rotation_value(int axis, double value)
{
    if (std::abs(m_cache.rotation_rounded(axis) - value) < EPSILON)
        return;

    Vec3d rotation = m_cache.rotation;
    rotation(axis) = value;

    Selection& selection = m_glcanvas.get_selection();

    TransformationType transformation_type;
    transformation_type.set_relative();
    if (selection.is_single_full_instance())
        transformation_type.set_independent();
    if (is_local_coordinates())
        transformation_type.set_local();
    if (is_instance_coordinates())
        transformation_type.set_instance();

    selection.setup_cache();
    selection.rotate((M_PI / 180.0) * (transformation_type.absolute() ? rotation : rotation - m_cache.rotation), transformation_type);
    wxGetApp().plater()->take_snapshot(_u8L("Set Orientation"), UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_rotate("");

    m_cache.rotation = rotation;
	m_cache.rotation_rounded(axis) = DBL_MAX;
    this->UpdateAndShow(true);
}

void GizmoObjectManipulation::change_absolute_rotation_value(int axis, double value) {
    if (std::abs(m_cache.absolute_rotation_rounded(axis) - value) < EPSILON)
        return;

    Vec3d absolute_rotation = m_cache.absolute_rotation;
    absolute_rotation(axis) = value;

    Selection &selection = m_glcanvas.get_selection();
    TransformationType transformation_type;
    transformation_type.set_relative();
    if (selection.is_single_full_instance())
        transformation_type.set_independent();
    if (is_local_coordinates())
        transformation_type.set_local();
    if (is_instance_coordinates())
        transformation_type.set_instance();

    selection.setup_cache();
    auto diff_rotation = transformation_type.absolute() ? absolute_rotation : absolute_rotation - m_cache.absolute_rotation;
    selection.rotate((M_PI / 180.0) * diff_rotation, transformation_type);
    wxGetApp().plater()->take_snapshot("set absolute orientation", UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_rotate("");

    m_cache.absolute_rotation               = absolute_rotation;
    m_cache.absolute_rotation_rounded(axis) = DBL_MAX;
    this->UpdateAndShow(true);
}

void GizmoObjectManipulation::change_scale_value(int axis, double value)
{
    if (value <= 0.0)
        return;
    if (std::abs(m_cache.scale_rounded(axis) - value) < EPSILON) {
        m_show_clear_scale = (m_cache.scale / 100.0f - Vec3d::Ones()).norm() > 0.001;
        return;
    }
    Vec3d scale     = m_cache.scale;
    scale(axis)     = value;
    Vec3d ref_scale = m_cache.scale;
    const Selection &selection = m_glcanvas.get_selection();
    if (selection.is_single_volume_or_modifier()) {
        scale     = scale.cwiseQuotient(ref_scale); // scale / ref_scale
        ref_scale =  Vec3d::Ones();
    } else if (selection.is_single_full_instance())
        ref_scale = 100 * Vec3d::Ones();
    this->do_scale(axis, scale.cwiseQuotient(ref_scale));

    m_cache.scale = scale;
	m_cache.scale_rounded(axis) = DBL_MAX;
	this->UpdateAndShow(true);
}


void GizmoObjectManipulation::change_size_value(int axis, double value)
{
    if (value <= 0.0)
        return;
    if (std::abs(m_cache.size_rounded(axis) - value) < EPSILON)
        return;

    Vec3d size = m_cache.size;
    size(axis) = value;

    const Selection& selection = m_glcanvas.get_selection();

    Vec3d ref_size = m_cache.size;
    if (selection.is_single_volume_or_modifier()) {
        size     = size.cwiseQuotient(ref_size);
        ref_size = Vec3d::Ones();
    } else if (selection.is_single_full_instance()) {
        if (is_world_coordinates())
            ref_size = selection.get_full_unscaled_instance_bounding_box().size();
        else
            ref_size = selection.get_full_unscaled_instance_local_bounding_box().size();
    }

    this->do_scale(axis, size.cwiseQuotient(ref_size));

    m_cache.size = size;
	m_cache.size_rounded(axis) = DBL_MAX;
	this->UpdateAndShow(true);
}

void GizmoObjectManipulation::do_scale(int axis, const Vec3d &scale) const
{
    Selection& selection = m_glcanvas.get_selection();

    TransformationType transformation_type;
    if (is_local_coordinates())
        transformation_type.set_local();
    else if (is_instance_coordinates())
        transformation_type.set_instance();
    if (selection.is_single_volume_or_modifier() && !is_local_coordinates())
        transformation_type.set_relative();

    Vec3d scaling_factor = m_uniform_scale ? scale(axis) * Vec3d::Ones() : scale;
    limit_scaling_ratio(scaling_factor);

    selection.setup_cache();
    selection.scale(scaling_factor, transformation_type);
    m_glcanvas.do_scale(L("Set Scale"));
}


void GizmoObjectManipulation::limit_scaling_ratio(Vec3d &scaling_factor) const{
    for (size_t i = 0; i < scaling_factor.size(); i++) { // range protect //scaling_factor too big has problem
        if (scaling_factor[i] * m_unscale_size[i] > MAX_NUM) {
            scaling_factor[i] = MAX_NUM / m_unscale_size[i];
        }
    }
}

void GizmoObjectManipulation::on_change(const std::string &opt_key, int axis, double new_value)
{
    if (!m_cache.is_valid())
        return;

    if (m_imperial_units && (opt_key == "position" || opt_key == "size"))
        new_value *= in_to_mm;

    if (opt_key == "position")
        change_position_value(axis, new_value);
    else if (opt_key == "rotation")
        change_rotation_value(axis, new_value);
    else if (opt_key == "absolute_rotation")
        change_absolute_rotation_value(axis, new_value);
    else if (opt_key == "scale")
        change_scale_value(axis, new_value);
    else if (opt_key == "size")
        change_size_value(axis, new_value);
}

bool GizmoObjectManipulation::render_combo(
    ImGuiWrapper *imgui_wrapper, const std::string &label, const std::vector<std::string> &lines, size_t &selection_idx, float label_width, float item_width)
{
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(label);
    ImGui::SameLine(label_width);
    ImGui::PushItemWidth(item_width);

    size_t selection_out = selection_idx;

    const char *selected_str = (selection_idx >= 0 && selection_idx < int(lines.size())) ? lines[selection_idx].c_str() : "";
    if (ImGui::BBLBeginCombo(("##" + label).c_str(), selected_str, 0)) {
        for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
            ImGui::PushID(int(line_idx));
            if (ImGui::Selectable("", line_idx == selection_idx)) selection_out = line_idx;

            ImGui::SameLine();
            ImGui::Text("%s", lines[line_idx].c_str());
            ImGui::PopID();
        }

        ImGui::EndCombo();
    }

    bool is_changed = selection_idx != selection_out;
    selection_idx   = selection_out;

    return is_changed;
}

void GizmoObjectManipulation::reset_position_value()
{
    Selection& selection = m_glcanvas.get_selection();

    if (selection.is_single_volume() || selection.is_single_modifier()) {
        GLVolume* volume = const_cast<GLVolume*>(selection.get_first_volume());
        volume->set_volume_offset(Vec3d::Zero());
    }
    else if (selection.is_single_full_instance()) {
        for (unsigned int idx : selection.get_volume_idxs()) {
            GLVolume* volume = const_cast<GLVolume*>(selection.get_volume(idx));
            volume->set_instance_offset(Vec3d::Zero());
        }
    }
    else
        return;

    // Copy position values from GLVolumes into Model (ModelInstance / ModelVolume), trigger background processing.
    wxGetApp().plater()->take_snapshot(_u8L("Reset Position"), UndoRedo::SnapshotType::GizmoAction);
    m_glcanvas.do_move("");

    UpdateAndShow(true);
}

void GizmoObjectManipulation::reset_rotation_value(bool reset_relative)
{
    Selection &selection = m_glcanvas.get_selection();
    selection.setup_cache();
    if (selection.is_single_volume_or_modifier()) {
        GLVolume *               vol    = const_cast<GLVolume *>(selection.get_first_volume());
        Geometry::Transformation trafo  = vol->get_volume_transformation();
        if (reset_relative) {
            auto offset = trafo.get_offset();
            trafo.set_matrix(m_init_rotation_scale_tran);
            trafo.set_offset(offset);
        }
        else {
            trafo.reset_rotation();
        }
        vol->set_volume_transformation(trafo);
    } else if (selection.is_single_full_instance()) {
        Geometry::Transformation trafo  = selection.get_first_volume()->get_instance_transformation();
        if (reset_relative) {
            auto offset = trafo.get_offset();
            trafo.set_matrix(m_init_rotation_scale_tran);
            trafo.set_offset(offset);
        } else {
            trafo.reset_rotation();
        }
        for (unsigned int idx : selection.get_volume_idxs()) {
            const_cast<GLVolume *>(selection.get_volume(idx))->set_instance_transformation(trafo);
        }
    } else
        return;
    // Synchronize instances/volumes.

    selection.synchronize_unselected_instances(Selection::SyncRotationType::RESET);
    selection.synchronize_unselected_volumes();
    // Copy rotation values from GLVolumes into Model (ModelInstance / ModelVolume), trigger background processing.
    m_glcanvas.do_rotate(L("Reset Rotation"));

    UpdateAndShow(true);
}

void GizmoObjectManipulation::reset_scale_value()
{
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), "Reset scale");

    change_scale_value(0, 100.);
    change_scale_value(1, 100.);
    change_scale_value(2, 100.);
}

void GizmoObjectManipulation::set_uniform_scaling(const bool use_uniform_scale)
{
    if (!use_uniform_scale)
        // Recalculate cached values at this panel, refresh the screen.
        this->UpdateAndShow(true);

    m_uniform_scale = use_uniform_scale;
    set_dirty();
}

void GizmoObjectManipulation::set_coordinates_type(ECoordinatesType type)
{
    /*if (wxGetApp().get_mode() == comSimple)
        type = ECoordinatesType::World;*/

    if (m_coordinates_type == type) return;

    m_coordinates_type = type;
    //m_word_local_combo->SetSelection((int) m_coordinates_type);
    this->UpdateAndShow(true);
    GLCanvas3D *canvas = wxGetApp().plater()->canvas3D();
    canvas->get_gizmos_manager().update_data();
    canvas->set_as_dirty();
    canvas->request_extra_frame();

}

static const char* label_values[3][3] = {
{ "##position_x", "##position_y", "##position_z"},
{ "##rotation_x", "##rotation_y", "##rotation_z"},
{ "##absolute_rotation_x", "##absolute_rotation_y", "##absolute_rotation_z"}
};

static const char* label_scale_values[2][3] = {
{ "##scale_x", "##scale_y", "##scale_z"},
{ "##size_x", "##size_y", "##size_z"}
};

bool GizmoObjectManipulation::reset_button(ImGuiWrapper *imgui_wrapper, float caption_max, float unit_size, float space_size, float end_text_size)
{
    bool        pressed   = false;
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_HOVER);

    float font_size = ImGui::GetFontSize();
    ImVec2 button_size = ImVec2(font_size, font_size);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    pressed = ImGui::ImageButton3(normal_id, hover_id, button_size);

    ImGui::PopStyleVar(1);
    return pressed;
}

bool GizmoObjectManipulation::reset_zero_button(ImGuiWrapper *imgui_wrapper, float caption_max, float unit_size, float space_size, float end_text_size)
{
    bool        pressed   = false;
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_ZERO);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_RESET_ZERO_HOVER);

    float  font_size   = ImGui::GetFontSize() * 1.1;
    ImVec2 button_size = ImVec2(font_size, font_size);

    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);

    pressed = ImGui::ImageButton3(normal_id, hover_id, button_size);

    ImGui::PopStyleVar(1);
    return pressed;
}

 float GizmoObjectManipulation::max_unit_size(int number, Vec3d &vec1, Vec3d &vec2,std::string str)
 {
     if (number <= 1) return -1;
     Vec3d vec[2] = {vec1, vec2};
     float nuit_max[4] = {0};
     float vec_max = 0, unit_size = 0;

     for (int i = 0; i < number; i++)
     {
         char buf[3][64] = {0};
         float buf_size[3] = {0};
         for (int j = 0; j < 3; j++) {
             ImGui::DataTypeFormatString(buf[j], IM_ARRAYSIZE(buf[j]), ImGuiDataType_Double, (void *) &vec[i][j], "%.2f");
             buf_size[j]  = ImGui::CalcTextSize(buf[j]).x;
             vec_max = std::max(buf_size[j], vec_max);
             nuit_max[i]  = vec_max;
         }
         unit_size = std::max(nuit_max[i], unit_size);
     }

     return unit_size + 8.0;
 }

 bool GizmoObjectManipulation::bbl_checkbox(const wxString &label, bool &value)
{
     bool result;
     bool b_value = value;
     if (b_value) {
         ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
         ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
         ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.00f, 0.68f, 0.26f, 1.00f));
     }
     auto label_utf8 = into_u8(label);
     result          = ImGui::BBLCheckbox(label_utf8.c_str(), &value);

     if (b_value) { ImGui::PopStyleColor(3); }
     return result;
}

void GizmoObjectManipulation::show_move_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y)
{
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_glcanvas.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &imgui_wrapper,& caption_max](const wxString &caption, const wxString &text) {
            imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 2>{"part_selection", "snap_step"})
            draw_text_with_caption(m_desc_move.at(t + "_caption") + ": ", m_desc_move.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GizmoObjectManipulation::show_rotate_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y)
{
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_glcanvas.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &imgui_wrapper, &caption_max](const wxString &caption, const wxString &text) {
            imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 1>{"part_selection"})
            draw_text_with_caption(m_desc_rotate.at(t + "_caption") + ": ", m_desc_rotate.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GizmoObjectManipulation::show_scale_tooltip_information(ImGuiWrapper *imgui_wrapper, float caption_max, float x, float y)
{
    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP);
    ImTextureID hover_id  = m_glcanvas.get_gizmos_manager().get_icon_texture_id(GLGizmosManager::MENU_ICON_NAME::IC_TOOLBAR_TOOLTIP_HOVER);

    caption_max += imgui_wrapper->calc_text_size(": "sv).x + 35.f;

    float  scale       = m_glcanvas.get_scale();
    ImVec2 button_size = ImVec2(25 * scale, 25 * scale); // ORCA: Use exact resolution will prevent blur on icon
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {0, ImGui::GetStyle().FramePadding.y});
    ImGui::ImageButton3(normal_id, hover_id, button_size);

    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip2(ImVec2(x, y));
        auto draw_text_with_caption = [this, &imgui_wrapper, &caption_max](const wxString &caption, const wxString &text) {
            imgui_wrapper->text_colored(ImGuiWrapper::COL_ACTIVE, caption);
            ImGui::SameLine(caption_max);
            imgui_wrapper->text_colored(ImGuiWrapper::COL_WINDOW_BG, text);
        };

        for (const auto &t : std::array<std::string, 3>{"part_selection", "snap_step", "single_sided"})
            draw_text_with_caption(m_desc_scale.at(t + "_caption") + ": ", m_desc_scale.at(t));
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

void GizmoObjectManipulation::set_init_rotation(const Geometry::Transformation &value) {
    m_init_rotation_scale_tran = value.get_matrix_no_offset();
    m_init_rotation      = value.get_rotation();
}

void GizmoObjectManipulation::do_render_move_window(ImGuiWrapper *imgui_wrapper, std::string window_name, float x, float y, float bottom_limit, GLGizmoBase *dock_owner)
{
    if (dock_owner) {
        dock_owner->dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    } else {
        // BBS: GUI refactor: move gizmo to the right
        if (abs(last_move_input_window_width) > 0.01f) {
            if (x + last_move_input_window_width > m_glcanvas.get_canvas_size().get_width()) {
                if (last_move_input_window_width > m_glcanvas.get_canvas_size().get_width())
                    x = 0;
                else
                    x = m_glcanvas.get_canvas_size().get_width() - last_move_input_window_width;
            }
        }
#if BBS_TOOLBAR_ON_TOP
        imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 0.f, 0.0f);
#else
        imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    }

    // BBS
    ImGuiWrapper::push_toolbar_style(m_glcanvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 6.0));

    std::string name = this->m_new_title_string + "##" + window_name;
    if (dock_owner) {
        imgui_wrapper->set_next_window_bg_alpha(GLGizmoBase::gizmo_panel_opacity());
        imgui_wrapper->begin(_L(name), dock_owner->dock_window_flags(ImGuiWrapper::TOOLBAR_WINDOW_FLAGS | ImGuiWindowFlags_NoTitleBar));
        if (!dock_owner->dock_render_titlebar(this->m_new_title_string)) {
            imgui_wrapper->end();
            ImGui::PopStyleVar(1);
            ImGuiWrapper::pop_toolbar_style();
            return;
        }
    } else {
        imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS);
    }

    auto update = [this](unsigned int active_id, std::string opt_key, Vec3d original_value, Vec3d new_value) -> int {
        for (int i = 0; i < 3; i++) {
            if (original_value[i] != new_value[i]) {
                if (active_id != m_last_active_item) {
                    on_change(opt_key, i, new_value[i]);
                    return i;
                }
            }
        }
        return -1;
    };

    float space_size    = imgui_wrapper->get_style_scaling() * 8;
    float position_size = imgui_wrapper->calc_text_size(_L("Position")).x + space_size;
    float caption_max    = imgui_wrapper->calc_text_size(_L("Object coordinates")).x + 2 * space_size;
    float end_text_size = imgui_wrapper->calc_text_size(this->m_new_unit_string).x;

    // position
    Vec3d original_position;
    if (this->m_imperial_units)
        original_position = this->m_new_position * this->mm_to_in;
    else
        original_position = this->m_new_position;
    Vec3d display_position = m_buffered_position;

    // Ultra: bottom-referenced Z - show the object's bottom height above the bed
    // instead of its center Z (display-only; internal state stays center-based).
    const bool bottom_ref_z = m_coordinates_type == ECoordinatesType::World &&
                              wxGetApp().app_config->get("bottom_referenced_z") == "true";
    double bottom_shift = 0.;
    if (bottom_ref_z) {
        bottom_shift = m_new_size.z() / 2.;
        if (this->m_imperial_units)
            bottom_shift *= this->mm_to_in;
        display_position.z() -= bottom_shift;
        original_position.z() -= bottom_shift;
    }

    // Rotation
    float unit_size = imgui_wrapper->calc_text_size(MAX_SIZE).x + space_size;
    int   index      = 1;
    int   index_unit = 1;

    ImGui::AlignTextToFramePadding();
    unsigned int current_active_id = ImGui::GetActiveID();

    Selection &              selection = m_glcanvas.get_selection();
    std::vector<std::string> modes     = {_u8L("World coordinates"), _u8L("Object coordinates")};//_u8L("Part coordinates")
    if (selection.is_multiple_full_object() || selection.is_wipe_tower()) {
        modes.pop_back();
    }
    size_t selection_idx = (int) m_coordinates_type;
    if (selection_idx >= modes.size()) {
        set_coordinates_type(ECoordinatesType::World);
        selection_idx = 0;
    }

    float caption_cs_size     = imgui_wrapper->calc_text_size(""sv).x;
    float caption_size        = caption_cs_size + 2 * space_size;
    float combox_content_size = imgui_wrapper->calc_text_size(_L("Object coordinates")).x * 1.2 + imgui_wrapper->calc_text_size("xxx"sv).x + imgui_wrapper->scaled(3);
    ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
    bool combox_changed = false;
    if (render_combo(imgui_wrapper, "", modes, selection_idx, caption_size, combox_content_size)) {
        combox_changed = true;
    }
    ImGuiWrapper::pop_combo_style();
    caption_max = combox_content_size - 4 * space_size;
    // ORCA use TextColored to match axes color
    float offset_to_center = (unit_size - ImGui::CalcTextSize("O").x) / 2;
    ImGui::SameLine(caption_max + index * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X");
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z");

    index      = 1;
    index_unit = 1;
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Position"));
    ImGui::SameLine(caption_max + index * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_values[0][0], &display_position[0], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_values[0][1], &display_position[1], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_values[0][2], &display_position[2], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    imgui_wrapper->text(this->m_new_unit_string);
    // Ultra: convert the bottom-referenced display back to internal center space.
    if (bottom_ref_z) {
        display_position.z() += bottom_shift;
        original_position.z() += bottom_shift;
    }
    bool is_avoid_one_update{false};
    if (combox_changed) {
        combox_changed = false;
        set_coordinates_type((ECoordinatesType) selection_idx);
        UpdateAndShow(true);
        is_avoid_one_update = true; // avoid update(current_active_id, "position", original_position
    }

    if (!is_avoid_one_update) {
        for (int i = 0; i < display_position.size(); i++) {
            if (display_position[i] > MAX_NUM) display_position[i] = MAX_NUM;
            if (display_position[i] < -MAX_NUM) display_position[i] = -MAX_NUM;
        }
        m_buffered_position = display_position;
        update(current_active_id, "position", original_position, m_buffered_position);
    }
    // the init position values are not zero, won't add reset button

    // send focus to m_glcanvas
    bool focued_on_text = false;
    for (int j = 0; j < 3; j++) {
        unsigned int id = ImGui::GetID(label_values[0][j]);
        if (current_active_id == id) {
            m_glcanvas.handle_sidebar_focus_event(label_values[0][j] + 2, true);
            focued_on_text = true;
            break;
        }
    }
    if (!focued_on_text) m_glcanvas.handle_sidebar_focus_event("", false);

    // BBS: align and distribute buttons (ported from BambuStudio's Move panel).
    // Unlike upstream this is not gated on world coordinates: snorca switches the
    // panel to Instance coordinates for single-part selections, and the alignment
    // engine operates on world-space boxes regardless of the displayed system.
    size_t selection_count = selection.get_volume_idxs().size();
    if (selection_count >= 1) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        const bool is_dark_mode           = wxGetApp().dark_mode();
        bool  show_align_parts_objects = selection_count >= 2;
        bool  is_part_node = selection.is_single_volume_or_modifier() || selection.is_multiple_volume() || selection.is_multiple_modifier();
        std::vector<std::string> align_modes = {_u8L("Align selected"), is_part_node ? _u8L("Align Object") : _u8L("Align Plate")};
        size_t align_mode_idx = (size_t) m_align_choice_type;
        if (!show_align_parts_objects) {
            align_modes.erase(align_modes.begin());
            m_align_choice_type = AlignChoiceType::AlignParent;
            align_mode_idx      = 0;
        }
        if (align_mode_idx >= align_modes.size()) {
            m_align_choice_type = AlignChoiceType::AlignParent;
            align_mode_idx      = 0;
        }

        ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
        ImGui::AlignTextToFramePadding();
        ImGui::PushID(1);
        if (render_combo(imgui_wrapper, "", align_modes, align_mode_idx, caption_size, combox_content_size)) {
            if (align_modes.size() > 1 && show_align_parts_objects)
                m_align_choice_type = (AlignChoiceType) align_mode_idx;
        }
        ImGui::PopID();
        ImGuiWrapper::pop_combo_style();
        float temp_tip_caption_max = imgui_wrapper->calc_text_size(_L("Align top-bottom center") + " (-X)").x * 1.3f;
        if (ImGui::IsItemHovered()) {
            imgui_wrapper->tooltip(_L("Holded down Ctrl key continuously and click by left mouse button can select multiple objects, or holded down Alt key continuously and "
                                      "click by left mouse button can select multiple parts"),
                                   temp_tip_caption_max * 1.5);
        }
        // Feed the reference box for Align Plate / Align Object each frame. In BambuStudio this
        // happens in the Move gizmo's hover-preview render, which is not ported.
        if (m_align_choice_type == AlignChoiceType::AlignParent) {
            BoundingBoxf3 parent_box;
            Vec3d         edge_inset = Vec3d::Zero();
            if (selection.is_single_full_object() || selection.is_multiple_full_object()) {
                auto *plate = wxGetApp().plater()->get_partplate_list().get_curr_plate();
                parent_box  = plate->get_plate_box();
                // Z: floor = the bed itself, ceiling = the build height, so Z works like X/Y: Bottom
                // sits on the bed, Top touches the ceiling, Center is mid-height. No 0.1 mm shrink in Z,
                // and the alignment commit does not drop floating instances back onto the bed, so a
                // result above or below the bed stays where the user put it.
                parent_box.min[2] = plate->get_origin().z();
                // X/Y: keep the objects 0.1 mm inside the plate when an edge goes to the same-side
                // plate edge (AlignMath applies it only there; centre targets stay exact).
                edge_inset = Vec3d(0.1, 0.1, 0.0);
            } else {
                const int obj_idx  = selection.get_object_idx();
                const int inst_idx = selection.get_instance_idx();
                Model*    model    = selection.get_model();
                if (model != nullptr && obj_idx >= 0 && obj_idx < (int) model->objects.size()) {
                    ModelObject* mo = model->objects[obj_idx];
                    if (inst_idx >= 0 && inst_idx < (int) mo->instances.size())
                        parent_box = mo->instance_convex_hull_bounding_box((size_t) inst_idx);
                    else
                        parent_box = mo->bounding_box_approx();
                }
            }
            m_alignment_helper->set_parent_box(parent_box, edge_inset);
        }

        if (!m_align_origin_loaded)
            load_align_origins();
        const bool inter_item = m_align_choice_type == AlignChoiceType::AlignPartOrObject;
        const float row_left_x = ImGui::GetCursorPosX();


        float scale_icon = 1.2f;
        float icon_size  = ImGui::GetFrameHeight() * scale_icon;

        // Row layout: the origin dropdowns sit in a row above the icons, each over its axis group.
        // The groups' x positions are only known once the icon row is laid out, so that row is
        // reserved here, the icons are drawn below it, and the dropdowns are drawn afterwards.
        const float anchor_row_y = ImGui::GetCursorPosY();
        const float row_h        = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.y;
        const float origin_row_y = anchor_row_y + (inter_item ? row_h : 0.f);
        const float start_y      = origin_row_y + row_h;
        float       group_l[3]   = {0.f, 0.f, 0.f};
        float       group_r[3]   = {0.f, 0.f, 0.f};
        // Screen-space item rect edges converted to the window-local coordinates SetCursorPos uses.
        auto item_left  = [] { return ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x + ImGui::GetScrollX(); };
        auto item_right = [] { return ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX(); };
        float       text_height   = ImGui::GetTextLineHeight();
        float       button_height = icon_size + ImGui::GetStyle().FramePadding.y * 2.0f;
        float       max_h         = std::max(text_height, button_height);

        ImGui::SetCursorPosY(start_y + (max_h - text_height) * 0.5f);
        if (inter_item)
            imgui_wrapper->text(" " + _L("Align") + "/" + _L("Distribute"));
        else
            imgui_wrapper->text(" " + _L("Align selected"));

        float button_spacing = 0;
        float start_x = caption_max + space_size * 1.5;
        ImGui::SameLine(start_x);
        ImGui::SetCursorPosY(start_y + (max_h - button_height) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(61.f / 255.f, 203.f / 255.f, 115.f / 255.f, 1.f));
        show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::X_MIN,
                        (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_X_MIN_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_X_MIN), icon_size,
                        _L("Align left") + " (-X)", "");
        group_l[0] = item_left();
        ImGui::SameLine(0, button_spacing);
        show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::CENTER_X,
                        (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_X_CENTER_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_X_CENTER), icon_size,
                        _L("Align left-right center") + " (X)", "");
        ImGui::SameLine(0, button_spacing);
        show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::X_MAX,
                        (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_X_MAX_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_X_MAX), icon_size,
                        _L("Align right") + " (+X)", "");
        if (inter_item) {
            ImGui::SameLine(0, button_spacing);
            show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::DISTRIBUTE_X,
                            (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_DISTRIBUTE_X_DARK : GLGizmosManager::MENU_ICON_NAME::IC_DISTRIBUTE_X), icon_size,
                            _L("Distribute left-right") + " (X)", _L("Please select at least 3 parts or objects"), true);
        }

        group_r[0] = item_right();
        // Chain the Y/Z groups after the X group with an explicit gap instead of absolute
        // offsets: BBS's unit_size-based offsets land short of the X row's end with this
        // panel's metrics, drawing "Align front" on top of "Align right".
        float group_gap = space_size * 2.0f;
        ImGui::SameLine(0, group_gap);
        show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::Y_MIN,
                        (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Y_MIN_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Y_MIN), icon_size,
                        _L("Align front") + " (-Y)", "");
        group_l[1] = item_left();
        ImGui::SameLine(0, button_spacing);
        show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::CENTER_Y,
                        (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Y_CENTER_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Y_CENTER), icon_size,
                        _L("Align front-back center") + " (Y)", "");
        ImGui::SameLine(0, button_spacing);
        show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::Y_MAX,
                        (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Y_MAX_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Y_MAX), icon_size,
                        _L("Align back") + " (+Y)", "");
        if (inter_item) {
            ImGui::SameLine(0, button_spacing);
            show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::DISTRIBUTE_Y,
                            (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_DISTRIBUTE_Y_DARK : GLGizmosManager::MENU_ICON_NAME::IC_DISTRIBUTE_Y), icon_size,
                            _L("Distribute front-back") + " (Y)", _L("Please select at least 3 parts or objects"), true);
        }
        group_r[1] = item_right();
        {
            ImGui::SameLine(0, group_gap);
            show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::Z_MIN,
                            (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Z_MIN_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Z_MIN), icon_size,
                            _L("Align bottom") + " (-Z)", "");
            group_l[2] = item_left();
            ImGui::SameLine(0, button_spacing);
            show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::CENTER_Z,
                            (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Z_CENTER_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Z_CENTER), icon_size,
                            _L("Align top-bottom center") + " (Z)", "");
            ImGui::SameLine(0, button_spacing);
            show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::Z_MAX,
                            (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Z_MAX_DARK : GLGizmosManager::MENU_ICON_NAME::IC_ALIGN_Z_MAX), icon_size,
                            _L("Align top") + " (+Z)", "");
        }
        if (inter_item) {
            ImGui::SameLine(0, button_spacing);
            show_align_icon(imgui_wrapper, temp_tip_caption_max, GLGizmoAlignment::AlignType::DISTRIBUTE_Z,
                            (int) (is_dark_mode ? GLGizmosManager::MENU_ICON_NAME::IC_DISTRIBUTE_Z_DARK : GLGizmosManager::MENU_ICON_NAME::IC_DISTRIBUTE_Z), icon_size,
                            _L("Distribute top-bottom") + " (Z)", _L("Please select at least 3 parts or objects"), true);
        }
        group_r[2] = item_right();
        ImGui::PopStyleColor();

        // Origin dropdowns, one over each axis group. Each spans exactly its group: left edge on
        // the first button's left edge, right edge on the last button's right edge, taken from the
        // item rects drawn above, so it holds at any DPI scale and for any translation. A text
        // that does not fit is ellipsised, never allowed to widen the panel.
        {
            ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
            if (inter_item) {
                ImGui::SetCursorPos(ImVec2(row_left_x, anchor_row_y));
                ImGui::AlignTextToFramePadding();
                imgui_wrapper->text(" " + _L("Anchor"));
                // Spans all three axis groups, from the first button to the last.
                render_align_anchor_combo(imgui_wrapper, group_l[0], anchor_row_y, AlignMath::combo_frame_width(group_l[0], group_r[2]));
            }
            ImGui::SetCursorPos(ImVec2(row_left_x, origin_row_y));
            ImGui::AlignTextToFramePadding();
            imgui_wrapper->text(" " + _L("Origin"));
            if (ImGui::IsItemHovered())
                imgui_wrapper->tooltip(inter_item ? _L("Which point of each moved item is brought to the target. Auto uses the same side as the "
                                                       "button (edge to edge, center to center). Anything else is applied against the anchor.")
                                                  : _L("Which point of the selection's bounding box is brought to the target. Auto uses the same "
                                                       "side as the button (edge to edge, center to center)."),
                                       ImGui::GetFontSize() * 20.0f);
            for (int axis = 0; axis < 3; ++axis)
                render_align_origin_combo(imgui_wrapper, axis, group_l[axis], origin_row_y, AlignMath::combo_frame_width(group_l[axis], group_r[axis]));
            ImGuiWrapper::pop_combo_style();
        }
        ImGui::SetCursorPosY(start_y + max_h + ImGui::GetStyle().ItemSpacing.y);
        if (!ImGui::IsAnyItemHovered())
            m_align_type = GLGizmoAlignment::AlignType::NONE;
    }

    // Ultra: "Hide other plates while moving". do_render_move_window is shared by Move/Rotate/
    // Scale, so the row is drawn only while the Move gizmo is the current one. The value is kept
    // straight in AppConfig (no gizmo-local copy) so this row and the Ultra preferences page are
    // always showing the same single value.
    if (m_glcanvas.get_gizmos_manager().get_current_type() == GLGizmosManager::Move) {
        // EdgeSlicer: Snap face to surface (face project snapping with align rotation).
        if (auto *move_gizmo = dynamic_cast<GLGizmoMove3D *>(dock_owner))
            move_gizmo->render_snap_to_surface_ui(imgui_wrapper, ImGui::GetFontSize() * 20.0f);
        ImGui::Separator();
        bool hide_other_plates = wxGetApp().app_config->get_bool("hide_other_plates_on_move");
        if (imgui_wrapper->checkbox(_L("Hide other plates while moving"), hide_other_plates))
            wxGetApp().app_config->set_bool("hide_other_plates_on_move", hide_other_plates);
        if (ImGui::IsItemHovered())
            imgui_wrapper->tooltip(_L("Show only the plate you are working on while the Move tool is open. "
                                      "The other plates and their objects come back when the Move tool closes, "
                                      "and a plate you drag an object onto reappears as soon as you release it."),
                                   ImGui::GetFontSize() * 20.0f);
    }

    float get_cur_y      = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float tip_caption_max    = 0.f;
    float total_text_max = 0.f;
    for (const auto &t : std::array<std::string, 2>{"part_selection", "snap_step"}) {
        tip_caption_max = std::max(tip_caption_max, imgui_wrapper->calc_text_size(m_desc_move[t + "_caption"]).x);
        total_text_max = std::max(total_text_max, imgui_wrapper->calc_text_size(m_desc_move[t]).x);
    }
    show_move_tooltip_information(imgui_wrapper, tip_caption_max, x, get_cur_y);
    m_last_active_item = current_active_id;
    last_move_input_window_width = ImGui::GetWindowWidth();
    imgui_wrapper->end();
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
}

// BBS: one align/distribute button with enable-check, dispatch and tooltip (ported from BambuStudio)
void GizmoObjectManipulation::show_align_icon(ImGuiWrapper *              imgui_wrapper,
                                              float                       max_tooltip_width,
                                              GLGizmoAlignment::AlignType align_type,
                                              int                         icon,
                                              float                       icon_size,
                                              const wxString &            function_tip,
                                              const wxString &            enable_tip,
                                              bool                        show_enable_tip)
{
    bool can_align = false;
    if (m_align_choice_type == AlignChoiceType::AlignPartOrObject) {
        if (GLGizmoAlignment::AlignType::DISTRIBUTE_X <= align_type)
            can_align = m_alignment_helper->can_distribute(align_type);
        else
            can_align = m_alignment_helper->can_align(align_type);
    } else {
        can_align = true;
    }

    ImTextureID normal_id = m_glcanvas.get_gizmos_manager().get_icon_texture_id((GLGizmosManager::MENU_ICON_NAME) icon);
    if (!can_align) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.3f);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_Button]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_Button]);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
    if (ImGui::ImageButton3(normal_id, normal_id, ImVec2(icon_size, icon_size))) {
        if (can_align) {
            if (GLGizmoAlignment::AlignType::DISTRIBUTE_X <= align_type) {
                m_alignment_helper->distribute_objects(align_type);
            } else {
                m_alignment_helper->align_objects(align_type, make_align_options());
            }
        }
    }
    ImGui::PopStyleVar();
    if (!can_align) {
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
    }

    if (ImGui::IsItemHovered()) {
        if (can_align) {
            m_align_type           = align_type;
            m_align_to_parent_node = m_align_choice_type == AlignChoiceType::AlignParent;
        } else {
            m_align_type = GLGizmoAlignment::AlignType::NONE;
        }
        ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGuiWrapper::COL_WINDOW_BACKGROUND);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
        if (can_align || !show_enable_tip) {
            wxString tip = function_tip;
            // With a non-Auto origin the plain "Align left" no longer says what happens: spell it out.
            int                    axis = 0;
            AlignMath::Side        side = AlignMath::Side::Min;
            if (can_align && GLGizmoAlignment::decode_align_type(align_type, axis, side) && m_align_origin[axis] != AlignMath::Origin::Auto) {
                const bool inter_item = m_align_choice_type == AlignChoiceType::AlignPartOrObject;
                const bool parts      = m_glcanvas.get_selection().is_single_volume_or_modifier() || m_glcanvas.get_selection().is_multiple_volume() ||
                                        m_glcanvas.get_selection().is_multiple_modifier();
                wxString moved  = inter_item ? _L("each moved item") : _L("the selection");
                wxString target = parts ? _L("the object") : _L("the plate");
                bool     anchored = false;
                if (inter_item) {
                    const auto cands = m_alignment_helper->anchor_candidates();
                    const int  a     = m_alignment_helper->resolve_anchor_candidate(make_align_options());
                    anchored         = a >= 0 && a < (int) cands.size();
                    target           = anchored ? wxString::FromUTF8(cands[(size_t) a].label) : _L("the selection bounds");
                }
                tip += "\n" + wxString::Format(_L("Brings the %s of %s to the %s of %s."),
                                               align_face_name(axis, AlignMath::pick_side(m_align_origin[axis], side)), moved,
                                               align_face_name(axis, side), target);
                if (anchored)
                    tip += "\n" + _L("The anchor stays in place.");
            }
            ImGui::SetTooltip("%s", tip.ToUTF8().data());
        } else {
            wxString combined = function_tip + "\n" + enable_tip;
            ImGui::SetTooltip("%s", combined.ToUTF8().data());
        }
        ImGui::PopStyleColor(2);
    }
}

// Entries of the origin dropdown for one axis, in AlignMath::Origin order: Auto, Center, Min, Max.
std::vector<std::string> GizmoObjectManipulation::align_origin_labels(int axis)
{
    std::vector<std::string> labels = {_u8L("Auto"), _u8L("Center")};
    switch (axis) {
    case 0:  labels.push_back(_u8L("Left"));   labels.push_back(_u8L("Right")); break;
    case 1:  labels.push_back(_u8L("Front"));  labels.push_back(_u8L("Back"));  break;
    default: labels.push_back(_u8L("Bottom")); labels.push_back(_u8L("Top"));   break;
    }
    return labels;
}

// "the right face", "the center": what a pick / target side is called on one axis, for tooltips.
wxString GizmoObjectManipulation::align_face_name(int axis, AlignMath::Side side)
{
    if (side == AlignMath::Side::Center)
        return _L("center");
    const bool low = side == AlignMath::Side::Min;
    switch (axis) {
    case 0:  return low ? _L("left face") : _L("right face");
    case 1:  return low ? _L("front face") : _L("back face");
    default: return low ? _L("bottom face") : _L("top face");
    }
}

static const char *const ALIGN_ORIGIN_KEYS[3] = {"align_origin_x", "align_origin_y", "align_origin_z"};
static const char *const ALIGN_ANCHOR_MODE_KEY = "align_anchor_mode";

void GizmoObjectManipulation::load_align_origins()
{
    const AppConfig *cfg = wxGetApp().app_config;
    for (int axis = 0; axis < 3; ++axis)
        m_align_origin[axis] = cfg != nullptr ? AlignMath::origin_from_key(cfg->get(ALIGN_ORIGIN_KEYS[axis])) : AlignMath::Origin::Auto;
    // Only the mode is remembered across sessions, never a specific item.
    m_align_anchor_mode   = cfg != nullptr ? AlignMath::anchor_mode_from_key(cfg->get(ALIGN_ANCHOR_MODE_KEY)) : AlignMath::AnchorMode::Last;
    m_align_origin_loaded = true;
}

void GizmoObjectManipulation::save_align_origin(int axis)
{
    if (AppConfig *cfg = wxGetApp().app_config)
        cfg->set(ALIGN_ORIGIN_KEYS[axis], AlignMath::origin_key(m_align_origin[axis]));
}

void GizmoObjectManipulation::save_align_anchor_mode()
{
    if (AppConfig *cfg = wxGetApp().app_config)
        cfg->set(ALIGN_ANCHOR_MODE_KEY, AlignMath::anchor_mode_key(m_align_anchor_mode));
}

GLGizmoAlignment::AlignOptions GizmoObjectManipulation::make_align_options() const
{
    GLGizmoAlignment::AlignOptions options;
    for (int axis = 0; axis < 3; ++axis)
        options.origin[axis] = m_align_origin[axis];
    options.to_parent           = m_align_choice_type == AlignChoiceType::AlignParent;
    options.anchor_mode         = m_align_anchor_mode;
    options.anchor_item_set     = m_align_anchor_item_set;
    options.anchor_object_idx   = m_align_anchor_object_idx;
    options.anchor_instance_idx = m_align_anchor_instance_idx;
    options.anchor_volume_idx   = m_align_anchor_volume_idx;
    return options;
}

// A dropdown whose visible frame is exactly `frame_width` wide at window-local (x, y). The preview
// text is already ellipsised by the caller. Returns true (and updates selection_idx) on a change.
bool GizmoObjectManipulation::render_spanning_combo(ImGuiWrapper *                  imgui_wrapper,
                                                    const char *                    id,
                                                    const std::vector<std::string> &lines,
                                                    size_t &                        selection_idx,
                                                    const std::string &             preview,
                                                    bool                            tinted,
                                                    float                           x,
                                                    float                           y,
                                                    float                           frame_width,
                                                    const wxString &                tooltip)
{
    // BBLBeginCombo draws a frame that is `width - 2 * frame height` wide (the arrow block counts twice).
    const float arrow = ImGui::GetFrameHeight();

    ImGui::SetCursorPos(ImVec2(x, y));
    ImGui::PushItemWidth(frame_width + 2.f * arrow);
    ImGui::PushID(id);
    int pushed = 0;
    if (tinted) {
        // A non-default choice changes what the buttons do, so it is tinted with the app accent.
        ImVec4 bg = ImGuiWrapper::COL_ORCA;
        bg.w      = 0.30f;
        ImVec4 bg_hover = ImGuiWrapper::COL_ORCA;
        bg_hover.w      = 0.45f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, bg);
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, bg_hover);
        ImGui::PushStyleColor(ImGuiCol_Border, ImGuiWrapper::COL_ORCA);
        pushed = 3;
    }
    const bool open = ImGui::BBLBeginCombo("##combo", preview.c_str(), 0);
    if (pushed > 0)
        ImGui::PopStyleColor(pushed);
    const bool hovered = ImGui::IsItemHovered();

    size_t selection_out = selection_idx;
    if (open) {
        for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
            ImGui::PushID(int(line_idx));
            if (ImGui::Selectable("", line_idx == selection_idx))
                selection_out = line_idx;
            ImGui::SameLine();
            ImGui::Text("%s", lines[line_idx].c_str());
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();
    ImGui::PopItemWidth();

    if (hovered && !open && !tooltip.empty())
        imgui_wrapper->tooltip(tooltip, ImGui::GetFontSize() * 20.0f);

    const bool changed = selection_out != selection_idx;
    selection_idx      = selection_out;
    return changed;
}

bool GizmoObjectManipulation::render_align_origin_combo(ImGuiWrapper *imgui_wrapper, int axis, float x, float y, float frame_width)
{
    const std::vector<std::string> lines = align_origin_labels(axis);
    size_t                         idx   = std::min((size_t) m_align_origin[axis], lines.size() - 1);
    // Text space in the preview: after the arrow block, between the frame paddings. Ellipsise, never widen.
    const float       text_space = frame_width - ImGui::GetFrameHeight() - 2.f * ImGui::GetStyle().FramePadding.x;
    const std::string preview    = AlignMath::ellipsize(lines[idx], text_space, [&](const std::string &s) { return (double) imgui_wrapper->calc_text_size(s).x; });
    const wxString    tip = m_align_choice_type == AlignChoiceType::AlignPartOrObject ?
                                _L("Which point of each moved item is brought to the target. Auto uses the same side as the button "
                                   "(edge to edge, center to center). Anything else is applied against the anchor.") :
                                _L("Which point of the selection's bounding box is brought to the target. Auto uses the same side as "
                                   "the button (edge to edge, center to center).");
    const std::string id  = "origin" + std::to_string(axis);
    if (!render_spanning_combo(imgui_wrapper, id.c_str(), lines, idx, preview, m_align_origin[axis] != AlignMath::Origin::Auto, x, y, frame_width, tip))
        return false;
    m_align_origin[axis] = (AlignMath::Origin) idx;
    save_align_origin(axis);
    return true;
}

// The Anchor dropdown of Align selected: Last selected, First selected, one entry per selected item,
// None (selection bounds). Same width rules as the origin dropdowns.
bool GizmoObjectManipulation::render_align_anchor_combo(ImGuiWrapper *imgui_wrapper, float x, float y, float frame_width)
{
    const std::vector<GLGizmoAlignment::AnchorCandidate> candidates = m_alignment_helper->anchor_candidates();
    std::vector<std::string> lines = {_u8L("Last selected"), _u8L("First selected")};
    for (const auto &c : candidates)
        lines.push_back(c.label);
    lines.push_back(_u8L("None (selection bounds)"));

    // An explicit item only counts while it is still selected; otherwise fall back to Last selected.
    const GLGizmoAlignment::AlignOptions options = make_align_options();
    bool                                 missing = false;
    const int                            anchor  = m_alignment_helper->resolve_anchor_candidate(options, &missing);
    if (missing) {
        m_align_anchor_item_set = false;
        m_align_anchor_mode     = AlignMath::AnchorMode::Last;
        save_align_anchor_mode();
    }

    size_t      idx = 0;
    std::string preview;
    const std::string anchor_name = anchor >= 0 && anchor < (int) candidates.size() ? candidates[(size_t) anchor].label : std::string();
    if (m_align_anchor_item_set && anchor >= 0) {
        idx     = 2 + (size_t) anchor;
        preview = anchor_name;
    } else if (m_align_anchor_mode == AlignMath::AnchorMode::None) {
        idx     = lines.size() - 1;
        preview = lines[idx];
    } else {
        idx     = m_align_anchor_mode == AlignMath::AnchorMode::First ? 1 : 0;
        preview = anchor_name.empty() ? lines[idx] : lines[idx] + ": " + anchor_name;
    }
    const float text_space = frame_width - ImGui::GetFrameHeight() - 2.f * ImGui::GetStyle().FramePadding.x;
    preview                = AlignMath::ellipsize(preview, text_space, [&](const std::string &s) { return (double) imgui_wrapper->calc_text_size(s).x; });

    const bool non_default = m_align_anchor_item_set || m_align_anchor_mode != AlignMath::AnchorMode::Last;
    if (!render_spanning_combo(imgui_wrapper, "anchor", lines, idx, preview, non_default, x, y, frame_width,
                               _L("The item that stays where it is while the other selected items move to it. It is only used when an origin "
                                  "below is not Auto. None aligns every item to the selection's own extremes with nothing fixed.")))
        return false;

    if (idx >= 2 && idx < 2 + candidates.size()) {
        const auto &c               = candidates[idx - 2];
        m_align_anchor_item_set     = true;
        m_align_anchor_object_idx   = c.object_idx;
        m_align_anchor_instance_idx = c.instance_idx;
        m_align_anchor_volume_idx   = c.volume_idx;
        return true; // a specific item is never persisted
    }
    m_align_anchor_item_set = false;
    m_align_anchor_mode     = idx == 1 ? AlignMath::AnchorMode::First : (idx == 0 ? AlignMath::AnchorMode::Last : AlignMath::AnchorMode::None);
    save_align_anchor_mode();
    return true;
}

void GizmoObjectManipulation::do_render_rotate_window(ImGuiWrapper *imgui_wrapper, std::string window_name, float x, float y, float bottom_limit, GLGizmoBase *dock_owner)
{
    if (dock_owner) {
        dock_owner->dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    } else {
        // BBS: GUI refactor: move gizmo to the right
        if (abs(last_rotate_input_window_width) > 0.01f) {
            if (x + last_rotate_input_window_width > m_glcanvas.get_canvas_size().get_width()) {
                if (last_rotate_input_window_width > m_glcanvas.get_canvas_size().get_width())
                    x = 0;
                else
                    x = m_glcanvas.get_canvas_size().get_width() - last_rotate_input_window_width;
            }
        }
#if BBS_TOOLBAR_ON_TOP
        imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 0.f, 0.0f);
#else
        imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    }

    // BBS
    ImGuiWrapper::push_toolbar_style(m_glcanvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 6.0));

    std::string name = this->m_new_title_string + "##" + window_name;
    if (dock_owner) {
        imgui_wrapper->set_next_window_bg_alpha(GLGizmoBase::gizmo_panel_opacity());
        imgui_wrapper->begin(_L(name), dock_owner->dock_window_flags(ImGuiWrapper::TOOLBAR_WINDOW_FLAGS | ImGuiWindowFlags_NoTitleBar));
        if (!dock_owner->dock_render_titlebar(this->m_new_title_string)) {
            imgui_wrapper->end();
            ImGui::PopStyleVar(1);
            ImGuiWrapper::pop_toolbar_style();
            return;
        }
    } else {
        imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS);
    }

    auto update = [this](unsigned int active_id, std::string opt_key, Vec3d original_value, Vec3d new_value) -> int {
        for (int i = 0; i < 3; i++) {
            if (original_value[i] != new_value[i]) {
                if (active_id != m_last_active_item) {
                    on_change(opt_key, i, new_value[i]);
                    return i;
                }
            }
        }
        return -1;
    };

    float space_size    = imgui_wrapper->get_style_scaling() * 8;
    float position_size = imgui_wrapper->calc_text_size(_L("Rotate (relative)")).x + space_size;
    float World_size    = imgui_wrapper->calc_text_size(_L("World coordinates")).x + space_size;
    float caption_max   = std::max(position_size, World_size) + 2 * space_size;
    float end_text_size = imgui_wrapper->calc_text_size(this->m_new_unit_string).x;

    // position
    Vec3d original_position;
    if (this->m_imperial_units)
        original_position = this->m_new_position * this->mm_to_in;
    else
        original_position = this->m_new_position;
    Vec3d display_position = m_buffered_position;
    // Rotation
    Vec3d rotation   = this->m_buffered_rotation;
    Vec3d absolute_rotation = this->m_buffered_absolute_rotation;
    float unit_size = imgui_wrapper->calc_text_size(MAX_SIZE).x + space_size;
    int   index      = 1;
    int   index_unit = 1;

    ImGui::AlignTextToFramePadding();
    unsigned int current_active_id = ImGui::GetActiveID();
    ImGui::PushItemWidth(caption_max);
    imgui_wrapper->text(_L("World coordinates"));
    // ORCA use TextColored to match axes color
    float offset_to_center = (unit_size - ImGui::CalcTextSize("O").x) / 2;
    ImGui::SameLine(caption_max + index * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X");
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z");

    index      = 1;
    index_unit = 1;

    // ImGui::PushItemWidth(unit_size * 2);
    bool is_relative_input = false;
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Rotate (relative)"));
    ImGui::SameLine(caption_max + index * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[1][0], &rotation[0], 0.0f, 0.0f, "%.2f")) {
        is_relative_input = true;
    }
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[1][1], &rotation[1], 0.0f, 0.0f, "%.2f")) {
        is_relative_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[1][2], &rotation[2], 0.0f, 0.0f, "%.2f")) {
        is_relative_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    imgui_wrapper->text("°");
    m_buffered_rotation = rotation;
    if (is_relative_input) {
        m_last_rotate_type = RotateType::Relative;
    }
    if (m_last_rotate_type == RotateType::Relative) {
        bool is_valid = update(current_active_id, "rotation", this->m_new_rotation, m_buffered_rotation) >= 0;
        if (is_valid) {
            m_last_rotate_type = RotateType::None;
        }
    }

    if (m_show_clear_rotation) {
        ImGui::SameLine(caption_max + 3 * unit_size + 4 * space_size + end_text_size);
        if (reset_button(imgui_wrapper, caption_max, unit_size, space_size, end_text_size)) {
            reset_rotation_value(true);
        }
        if (ImGui::IsItemHovered()) {
            float tooltip_size = imgui_wrapper->calc_text_size(_L("Reset current rotation to the value when open the rotation tool.")).x + 3 * space_size;
            imgui_wrapper->tooltip(_u8L("Reset current rotation to the value when open the rotation tool."), tooltip_size);
        }
    } else {
        ImGui::SameLine(caption_max + 3 * unit_size + 5 * space_size + end_text_size);
        ImGui::InvisibleButton("", ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
    }
    // send focus to m_glcanvas
    bool focued_on_text = false;
    for (int j = 0; j < 3; j++) {
        unsigned int id = ImGui::GetID(label_values[1][j]);
        if (current_active_id == id) {
            m_glcanvas.handle_sidebar_focus_event(label_values[1][j] + 2, true);
            focued_on_text = true;
            break;
        }
    }

    index      = 1;
    index_unit = 1;
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Rotate (absolute)"));
    ImGui::SameLine(caption_max + index * space_size);
    ImGui::PushItemWidth(unit_size);
    bool is_absolute_input = false;
    if (ImGui::BBLInputDouble(label_values[2][0], &absolute_rotation[0], 0.0f, 0.0f, "%.2f")) {
        is_absolute_input = true;
    }
    ImGui::SameLine(caption_max + unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[2][1], &absolute_rotation[1], 0.0f, 0.0f, "%.2f")) {
        is_absolute_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    if (ImGui::BBLInputDouble(label_values[2][2], &absolute_rotation[2], 0.0f, 0.0f, "%.2f")) {
        is_absolute_input = true;
    }
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size);
    imgui_wrapper->text("°");
    m_buffered_absolute_rotation = absolute_rotation;
    if (is_absolute_input) {
        m_last_rotate_type = RotateType::Absolute;
    }
    if (m_last_rotate_type == RotateType::Absolute) {
        bool is_valid = update(current_active_id, "absolute_rotation", this->m_new_absolute_rotation, m_buffered_absolute_rotation) >= 0;
        if (is_valid) {
            m_last_rotate_type = RotateType::None;
        }
    }

    if (m_show_reset_0_rotation) {
        ImGui::SameLine(caption_max + 3 * unit_size + 4 * space_size + end_text_size);
        if (reset_zero_button(imgui_wrapper, caption_max, unit_size, space_size, end_text_size)) { reset_rotation_value(false); }
        if (ImGui::IsItemHovered()) {
            float tooltip_size = imgui_wrapper->calc_text_size(_L("Reset current rotation to real zeros.")).x + 3 * space_size;
            imgui_wrapper->tooltip(_L("Reset current rotation to real zeros."), tooltip_size);
        }
    }
    // send focus to m_glcanvas
    bool absolute_focued_on_text = false;
    for (int j = 0; j < 3; j++) {
        unsigned int id = ImGui::GetID(label_values[2][j]);
        if (current_active_id == id) {
            m_glcanvas.handle_sidebar_focus_event(label_values[2][j] + 2, true);
            absolute_focued_on_text = true;
            break;
        }
    }
    if (!focued_on_text  && !absolute_focued_on_text)
        m_glcanvas.handle_sidebar_focus_event("", false);

    float get_cur_y       = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float tip_caption_max = 0.f;
    float total_text_max  = 0.f;
    for (const auto &t : std::array<std::string, 1>{"part_selection"}) {
        tip_caption_max = std::max(tip_caption_max, imgui_wrapper->calc_text_size(m_desc_move[t + "_caption"]).x);
        total_text_max  = std::max(total_text_max, imgui_wrapper->calc_text_size(m_desc_move[t]).x);
    }
    show_rotate_tooltip_information(imgui_wrapper, tip_caption_max, x, get_cur_y);
    m_last_active_item = current_active_id;
    last_rotate_input_window_width = ImGui::GetWindowWidth();
    imgui_wrapper->end();

    // BBS
    ImGui::PopStyleVar(1);
    ImGuiWrapper::pop_toolbar_style();
}

void GizmoObjectManipulation::do_render_scale_input_window(ImGuiWrapper* imgui_wrapper, std::string window_name, float x, float y, float bottom_limit, GLGizmoBase *dock_owner)
{
    if (dock_owner) {
        dock_owner->dock_setup_next_window(x, y, bottom_limit, 0.f, /*size_to_content=*/true);
    } else {
        //BBS: GUI refactor: move gizmo to the right
        if (abs(last_scale_input_window_width) > 0.01f) {
            if (x + last_scale_input_window_width > m_glcanvas.get_canvas_size().get_width()) {
                if (last_scale_input_window_width > m_glcanvas.get_canvas_size().get_width())
                    x = 0;
                else
                    x = m_glcanvas.get_canvas_size().get_width() - last_scale_input_window_width;
            }
        }
#if BBS_TOOLBAR_ON_TOP
        imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 0.f, 0.0f);
#else
        imgui_wrapper->set_next_window_pos(x, y, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    }

    //BBS
    ImGuiWrapper::push_toolbar_style(m_glcanvas.get_scale());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0, 6.0));

    std::string name = this->m_new_title_string + "##" + window_name;
    if (dock_owner) {
        imgui_wrapper->set_next_window_bg_alpha(GLGizmoBase::gizmo_panel_opacity());
        imgui_wrapper->begin(_L(name), dock_owner->dock_window_flags(ImGuiWrapper::TOOLBAR_WINDOW_FLAGS | ImGuiWindowFlags_NoTitleBar));
        if (!dock_owner->dock_render_titlebar(this->m_new_title_string)) {
            imgui_wrapper->end();
            ImGui::PopStyleVar(1);
            ImGuiWrapper::pop_toolbar_style();
            return;
        }
    } else {
        imgui_wrapper->begin(_L(name), ImGuiWrapper::TOOLBAR_WINDOW_FLAGS);
    }

    auto update = [this](unsigned int active_id, std::string opt_key, Vec3d original_value, Vec3d new_value)->int {
        for (int i = 0; i < 3; i++)
        {
            if (original_value[i] != new_value[i])
            {
                if (active_id != m_last_active_item)
                {
                    on_change(opt_key, i, new_value[i]);
                    return i;
                }
            }
        }
        return -1;
    };

    float space_size = imgui_wrapper->get_style_scaling() * 8;
    float scale_size = imgui_wrapper->calc_text_size(_L("Scale")).x + space_size;
    float caption_max   = imgui_wrapper->calc_text_size(_L("Object coordinates")).x + 2 * space_size;
    float end_text_size = imgui_wrapper->calc_text_size(this->m_new_unit_string).x;
    ImGui::AlignTextToFramePadding();
    unsigned int current_active_id = ImGui::GetActiveID();

    Vec3d scale = m_buffered_scale;
    Vec3d display_size = m_buffered_size;

    Vec3d display_position = m_buffered_position;

    float unit_size = imgui_wrapper->calc_text_size(MAX_SIZE).x + space_size;
    bool imperial_units = this->m_imperial_units;

    int index      = 2;
    int index_unit = 1;

    Selection &              selection = m_glcanvas.get_selection();
    std::vector<std::string> modes     = {_u8L("World coordinates"), _u8L("Object coordinates"), _u8L("Part coordinates")};
    if (selection.is_single_full_object()) { modes.pop_back(); }
    if (selection.is_multiple_full_object()) {
        modes.pop_back();
        modes.pop_back();
    }
    size_t selection_idx = (int) m_coordinates_type;
    if (selection_idx >= modes.size()) {
        set_coordinates_type(ECoordinatesType::World);
        selection_idx = 0;
    }

    float caption_cs_size     = imgui_wrapper->calc_text_size(""sv).x;
    float caption_size        = caption_cs_size + 2 * space_size;
    float combox_content_size = imgui_wrapper->calc_text_size(_L("Object coordinates")).x * 1.2 + imgui_wrapper->calc_text_size("xxx"sv).x + imgui_wrapper->scaled(3);
    ImGuiWrapper::push_combo_style(m_glcanvas.get_scale());
    bool combox_changed = false;
    if (render_combo(imgui_wrapper, "", modes, selection_idx, caption_size, combox_content_size)) {
        combox_changed = true;
    }
    ImGuiWrapper::pop_combo_style();
    caption_max = combox_content_size - 4 * space_size;
    //ImGui::Dummy(ImVec2(caption_max, -1));
    // ORCA use TextColored to match axes color
    float offset_to_center = (unit_size - ImGui::CalcTextSize("O").x) / 2;
    ImGui::SameLine(caption_max + space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::X()),"X");
    ImGui::SameLine(caption_max + unit_size + index * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Y()),"Y");
    ImGui::SameLine(caption_max + (++index_unit) * unit_size + (++index) * space_size + offset_to_center);
    ImGui::TextColored(ImGuiWrapper::to_ImVec4(ColorRGBA::Z()),"Z");

    index      = 2;
    index_unit = 1;

    //ImGui::PushItemWidth(unit_size * 2);
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Scale"));
    ImGui::SameLine(caption_max + space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[0][0], &scale[0], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + unit_size + index * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[0][1], &scale[1], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[0][2], &scale[2], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    imgui_wrapper->text("%");
    if (scale.x() > 0 && scale.y() > 0 && scale.z() > 0) {
        m_buffered_scale = scale;
    }

    if (m_show_clear_scale) {
        ImGui::SameLine(caption_max + 3 * unit_size + 4 * space_size + end_text_size);
        if (reset_button(imgui_wrapper, caption_max, unit_size, space_size, end_text_size))
            reset_scale_value();
    } else {
        ImGui::SameLine(caption_max + 3 * unit_size + 5 * space_size + end_text_size);
        ImGui::InvisibleButton("", ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
    }

    //Size
    Vec3d original_size;
    if (this->m_imperial_units)
        original_size = this->m_new_size * this->mm_to_in;
    else
        original_size = this->m_new_size;

    index              = 2;
    index_unit         = 1;
    //ImGui::PushItemWidth(unit_size * 2);
    ImGui::AlignTextToFramePadding();
    imgui_wrapper->text(_L("Size"));
    ImGui::SameLine(caption_max + space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[1][0], &display_size[0], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + unit_size + index * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[1][1], &display_size[1], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    ImGui::PushItemWidth(unit_size);
    ImGui::BBLInputDouble(label_scale_values[1][2], &display_size[2], 0.0f, 0.0f, "%.2f");
    ImGui::SameLine(caption_max + (++index_unit) *unit_size + (++index) * space_size);
    imgui_wrapper->text(this->m_new_unit_string);
    for (int i = 0; i < display_size.size(); i++) {
        if (std::abs(display_size[i]) > MAX_NUM) {
            display_size[i] = MAX_NUM;
        }
    }
    if (display_size.x() > 0 && display_size.y() > 0 && display_size.z() > 0) {
        m_buffered_size = display_size;
    }
    ImGui::Separator();
    ImGui::AlignTextToFramePadding();
    bool is_avoid_one_update{false};
    if (combox_changed) {
        combox_changed = false;
        set_coordinates_type((ECoordinatesType) selection_idx);
        UpdateAndShow(true);
        is_avoid_one_update = true;
    }

    auto uniform_scale_size =imgui_wrapper->calc_text_size(_L("uniform scale")).x;
    ImGui::PushItemWidth(uniform_scale_size);
    int size_sel{-1};
    if (!is_avoid_one_update) {
        size_sel    = update(current_active_id, "size", original_size, m_buffered_size);
    }
    ImGui::PopStyleVar(1);
    bool uniform_scale = this->m_uniform_scale;

    // BBS: when select multiple objects, uniform scale can be deselected
    //const Selection &selection = m_glcanvas.get_selection();
    //bool uniform_scale_only    = selection.is_multiple_full_object() || selection.is_multiple_full_instance() || selection.is_mixed() || selection.is_multiple_volume() ||
    //                          selection.is_multiple_modifier();

    //if (uniform_scale_only) {
    //    imgui_wrapper->disabled_begin(true);
    //    imgui_wrapper->bbl_checkbox(_L("uniform scale"), uniform_scale_only);
    //    imgui_wrapper->disabled_end();
    //} else {
        imgui_wrapper->bbl_checkbox(_L("uniform scale"), uniform_scale);
    //}
    if (uniform_scale != this->m_uniform_scale) { this->set_uniform_scaling(uniform_scale); }

     // for (int index = 0; index < 3; index++)
    //    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",before_index="<<index <<boost::format(",scale %1%, buffered %2%, original_id %3%, new_id %4%\n") %
    //    this->m_new_scale[index] % m_buffered_scale[index] % m_last_active_item % current_active_id;
    int scale_sel = update(current_active_id, "scale", this->m_new_scale, m_buffered_scale);
    if ((scale_sel >= 0)) {
        // for (int index = 0; index < 3; index++)
        //    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",after_index="<<index <<boost::format(",scale %1%, buffered %2%, original_id %3%, new_id %4%\n") %
        //    this->m_new_scale[index] % m_buffered_scale[index] % m_last_active_item % current_active_id;
        for (int i = 0; i < 3; ++i) {
            if (i != scale_sel) ImGui::ClearInputTextInitialData(label_scale_values[0][i], m_buffered_scale[i]);
            ImGui::ClearInputTextInitialData(label_scale_values[1][i], m_buffered_size[i]);
        }
    }

    if ((size_sel >= 0)) {
        // for (int index = 0; index < 3; index++)
        //    BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << ",after_index="<<index <<boost::format(",scale %1%, buffered %2%, original_id %3%, new_id %4%\n") %
        //    this->m_new_scale[index] % m_buffered_scale[index] % m_last_active_item % current_active_id;
        for (int i = 0; i < 3; ++i) {
            ImGui::ClearInputTextInitialData(label_scale_values[0][i], m_buffered_scale[i]);
            if (i != size_sel) ImGui::ClearInputTextInitialData(label_scale_values[1][i], m_buffered_size[i]);
        }
    }

    //send focus to m_glcanvas
    bool focued_on_text = false;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 3; j++)
        {
            unsigned int id = ImGui::GetID(label_scale_values[i][j]);
            if (current_active_id == id)
            {
                m_glcanvas.handle_sidebar_focus_event(label_scale_values[i][j] + 2, true);
                focued_on_text = true;
                break;
            }
        }
    if (!focued_on_text)
        m_glcanvas.handle_sidebar_focus_event("", false);
    float get_cur_y       = ImGui::GetContentRegionMax().y + ImGui::GetFrameHeight() + y;
    float tip_caption_max = 0.f;
    float total_text_max  = 0.f;
    for (const auto &t : std::array<std::string, 3>{"part_selection", "snap_step", "single_sided"}) {
        tip_caption_max = std::max(tip_caption_max, imgui_wrapper->calc_text_size(m_desc_scale[t + "_caption"]).x);
        total_text_max  = std::max(total_text_max, imgui_wrapper->calc_text_size(m_desc_scale[t]).x);
    }
    show_scale_tooltip_information(imgui_wrapper, tip_caption_max, x, get_cur_y);
    m_last_active_item = current_active_id;

    last_scale_input_window_width = ImGui::GetWindowWidth();
    imgui_wrapper->end();

    //BBS
    ImGuiWrapper::pop_toolbar_style();
}


} //namespace GUI
} //namespace Slic3r
