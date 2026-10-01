#include "StabilizerBakeJob.hpp"

#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r { namespace GUI {

StabilizerBakeJob::StabilizerBakeJob(Plater                      *plater,
                                     const PrintObject           *print_object,
                                     ObjectID                     model_object_id,
                                     const StabilizerBakeOptions &options,
                                     const std::string           &object_name)
    : m_plater(plater)
    , m_print_object(print_object)
    , m_object_id(model_object_id)
    , m_options(options)
    , m_name(object_name)
{}

void StabilizerBakeJob::process(Ctl &ctl)
{
    if (m_print_object == nullptr)
        return;
    ctl.update_status(0, _u8L("Baking the stabilizers"));
    try {
        m_result = bake_stabilizers(*m_print_object, m_options);
    } catch (const std::exception &e) {
        // Clipper and the boolean can both throw on degenerate input; report it rather than unwind.
        m_error = e.what();
        return;
    }
    if (ctl.was_canceled())
        return;
    ctl.update_status(100, _u8L("Stabilizers baked."));
}

void StabilizerBakeJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr)
        return;

    if (! m_error.empty()) {
        show_error(m_plater, from_u8((boost::format(_u8L("Could not bake the stabilizers of \"%1%\": %2%")) % m_name % m_error).str()));
        return;
    }
    if (m_result.mesh.indices.empty()) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("Could not bake the stabilizers of \"%1%\": %2%.")) % m_name % m_result.error).str());
        return;
    }

    Model &model   = m_plater->model();
    int    obj_idx = -1;
    for (size_t i = 0; i < model.objects.size(); ++i)
        if (model.objects[i]->id() == m_object_id) {
            obj_idx = int(i);
            break;
        }
    if (obj_idx < 0) {
        wxGetApp().notification_manager()->push_plater_warning_notification(
            (boost::format(_u8L("\"%1%\" was removed while its stabilizers were being baked; nothing was changed.")) % m_name).str());
        return;
    }
    ModelObject *source = model.objects[size_t(obj_idx)];

    // One snapshot for the new geometry and the switched-off live setting, so one Undo takes back both.
    Plater::TakeSnapshot snapshot(m_plater, "Bake stabilizers");
    ModelObject *target = apply_stabilizer_bake(model, *source, m_result, m_options);
    if (target == nullptr)
        return;

    ObjectList *obj_list = wxGetApp().obj_list();
    PartPlateList &plates = m_plater->get_partplate_list();
    if (m_options.placement == StabilizerBakePlacement::SeparateObject) {
        // Built by hand rather than through load_mesh_object, which would recentre the mesh and drop it
        // into an empty spot of the plate: the stabilizers belong exactly where they were planned (see
        // SliceBakeJob for the same choice). The mesh's Z is the printed Z, so no ensure_on_bed() either.
        const size_t new_idx = model.objects.size() - 1;
        model.InitializeAssemblyPositions({ target });
        Slic3r::save_object_mesh(*target);
        obj_list->paste_objects_into_list({ new_idx });
        for (size_t i = 0; i < target->instances.size(); ++i)
            plates.notify_instance_update(int(new_idx), int(i));
    } else {
        Slic3r::save_object_mesh(*source);
        obj_list->add_volumes_to_object_in_list(size_t(obj_idx));
        obj_list->changed_object(obj_idx);
        obj_list->notify_instance_updated(obj_idx);
        obj_list->update_info_items(size_t(obj_idx));
    }
    // The live setting went Off on the source (or stayed as it was, if asked to).
    obj_list->refresh_object_settings(obj_idx);
    m_plater->changed_object(obj_idx);
    if (PartPlate *plate = plates.get_curr_plate())
        plate->update_slice_result_valid_state(false);

    std::string summary = (boost::format(_u8L("Baked %1% stabilizer struts of \"%2%\" into %3% triangles.")) % m_result.struts.size()
                           % m_name % m_result.mesh_report.triangles).str();
    if (m_options.placement == StabilizerBakePlacement::SeparateObject) {
        summary += " " + (boost::format(_u8L("They are the object \"%1%\": it does not follow moves of the part or arrange, so bake again "
                                             "after moving the part.")) % target->name).str();
        if (target->instances.size() < source->instances.size())
            summary += " " + _u8L("Copies of the part with another rotation or scale got no stabilizers; bake those separately.");
    }
    if (m_options.turn_off_live)
        summary += " " + _u8L("The object's live side stabilizers are now off.");
    if (! m_result.plan_report.unreachable.empty())
        summary += " " + (boost::format(_u8L("%1% painted stabilizer point(s) could not be reached.")) % m_result.plan_report.unreachable.size()).str();
    if (! m_result.mesh_report.unioned)
        summary += " " + _u8L("The pieces could not be merged into one solid; they are kept as overlapping solids, which slicers merge.");
    wxGetApp().notification_manager()->push_notification(summary);
}

}} // namespace Slic3r::GUI
