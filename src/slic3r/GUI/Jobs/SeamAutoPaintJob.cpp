#include "SeamAutoPaintJob.hpp"

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/GCode/SeamAutoPaint.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/format.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace Slic3r { namespace GUI {

SeamAutoPaintJob::SeamAutoPaintJob(SeamAutoPaintRequest request, const Print *print, const PrintObject *po, Finish finish)
    : m_request(std::move(request)), m_plate_print(print), m_plate_object(po), m_finish(std::move(finish))
{}

SeamAutoPaintJob::SeamAutoPaintJob(SeamAutoPaintRequest request, std::unique_ptr<Model> model, DynamicPrintConfig config, Finish finish)
    : m_request(std::move(request)), m_model(std::move(model)), m_config(std::move(config)), m_finish(std::move(finish))
{}

SeamAutoPaintJob::~SeamAutoPaintJob() = default;

// The model parts of `mo`, in order: the volumes the seam gizmo keeps a triangle selector for.
static std::vector<const ModelVolume *> model_parts(const ModelObject &mo)
{
    std::vector<const ModelVolume *> out;
    for (const ModelVolume *mv : mo.volumes)
        if (mv->is_model_part())
            out.push_back(mv);
    return out;
}

const PrintObject *SeamAutoPaintJob::sliced_print_object(Plater *plater, int obj_idx, int instance_idx, bool same_paint,
                                                         const Print **print_out)
{
    if (plater == nullptr || obj_idx < 0 || instance_idx < 0)
        return nullptr;
    // A slice that is running owns the layers the planner would read.
    if (plater->is_background_process_slicing())
        return nullptr;
    const Model &model = plater->model();
    if (size_t(obj_idx) >= model.objects.size())
        return nullptr;
    const ModelObject *mo = model.objects[size_t(obj_idx)];
    if (size_t(instance_idx) >= mo->instances.size())
        return nullptr;
    const ModelInstance *mi = mo->instances[size_t(instance_idx)];

    PartPlateList &plates    = plater->get_partplate_list();
    const int      plate_idx = plates.find_instance(obj_idx, instance_idx);
    if (plate_idx < 0)
        return nullptr;
    PartPlate *plate = plates.get_plate(plate_idx);
    Print     *print = plate != nullptr ? plate->fff_print() : nullptr;
    if (print == nullptr)
        return nullptr;

    for (const PrintObject *po : print->objects()) {
        if (po == nullptr || po->model_object() == nullptr || po->model_object()->id() != mo->id())
            continue;
        const PrintInstance *pi = nullptr;
        for (const PrintInstance &instance : po->instances())
            if (instance.model_instance != nullptr && instance.model_instance->id() == mi->id())
                pi = &instance;
        if (pi == nullptr)
            continue;
        if (!po->is_step_done(posPerimeters) || po->layer_count() == 0)
            return nullptr;
        // The Print's copy of the object must still be the object as it is now: same parts, same meshes, same
        // placement, same settings. Print::apply() normally keeps it so; this guards the moments in between.
        const ModelObject &copy = *po->model_object();
        if (copy.volumes.size() != mo->volumes.size() || !(copy.config.get() == mo->config.get()))
            return nullptr;
        for (size_t i = 0; i < mo->volumes.size(); ++i) {
            const ModelVolume &a = *copy.volumes[i];
            const ModelVolume &b = *mo->volumes[i];
            if (a.id() != b.id() || a.type() != b.type() || a.mesh_ptr() != b.mesh_ptr() ||
                !a.get_matrix().isApprox(b.get_matrix()) || !(a.config.get() == b.config.get()) ||
                (same_paint && !a.seam_facets.equals(b.seam_facets)))
                return nullptr;
        }
        if (!pi->model_instance->get_matrix().isApprox(mi->get_matrix()))
            return nullptr;
        if (print_out != nullptr)
            *print_out = print;
        return po;
    }
    return nullptr;
}

std::unique_ptr<Model> SeamAutoPaintJob::private_model(Plater *plater, int obj_idx, int instance_idx, bool with_neighbours,
                                                       DynamicPrintConfig &config_out)
{
    const Model &model = plater->model();
    if (obj_idx < 0 || size_t(obj_idx) >= model.objects.size() || instance_idx < 0 ||
        size_t(instance_idx) >= model.objects[size_t(obj_idx)]->instances.size())
        return nullptr;

    PartPlateList &plates    = plater->get_partplate_list();
    const int      plate_idx = plates.find_instance(obj_idx, instance_idx);

    // The config the plate would be sliced with (BackgroundSlicingProcess::apply()).
    config_out = wxGetApp().preset_bundle->full_config();
    if (plate_idx >= 0)
        if (PartPlate *plate = plates.get_plate(plate_idx))
            config_out.apply(*plate->config());

    // The instances to keep, per object: the object's instances on the plate (the slicer shares one PrintObject
    // between instances placed alike, and then skips joints with other objects), and the instances of other objects
    // on the same plate that come within reach of the target instance (SeamPlacer only looks at those for joints
    // between objects).
    BoundingBoxf3 reach = model.objects[size_t(obj_idx)]->instance_bounding_box(size_t(instance_idx));
    reach.min -= Vec3d::Constant(1.);
    reach.max += Vec3d::Constant(1.);
    std::vector<std::vector<size_t>> keep(model.objects.size());
    for (size_t i = 0; i < model.objects[size_t(obj_idx)]->instances.size(); ++i)
        if (int(i) == instance_idx || (plate_idx >= 0 && plates.find_instance(obj_idx, int(i)) == plate_idx))
            keep[size_t(obj_idx)].push_back(i);
    if (with_neighbours)
        for (size_t o = 0; o < model.objects.size(); ++o) {
            if (int(o) == obj_idx)
                continue;
            for (size_t i = 0; i < model.objects[o]->instances.size(); ++i)
                if ((plate_idx < 0 || plates.find_instance(int(o), int(i)) == plate_idx) &&
                    model.objects[o]->instance_bounding_box(i).intersects(reach))
                    keep[o].push_back(i);
        }

    // A copy keeps the object ids, so the private Print's objects can be matched to the model's.
    auto copy = std::make_unique<Model>(model);
    for (size_t o = copy->objects.size(); o-- > 0;) {
        if (keep[o].empty()) {
            copy->delete_object(o);
            continue;
        }
        ModelObject *object = copy->objects[o];
        for (size_t i = object->instances.size(); i-- > 0;)
            if (std::find(keep[o].begin(), keep[o].end(), i) == keep[o].end())
                object->delete_instance(i);
        // Print::apply() drops what is not printable or not inside the bed; the seams are wanted regardless.
        object->printable = true;
        for (ModelInstance *instance : object->instances) {
            instance->printable          = true;
            instance->print_volume_state = ModelInstancePVS_Inside;
        }
    }
    return copy;
}

void SeamAutoPaintJob::process(Ctl &ctl)
{
    const std::string title = _u8L("Auto-paint seam");
    ctl.update_status(0, title);
    auto throw_if_canceled = [&ctl]() {
        if (ctl.was_canceled())
            throw CanceledException();
    };

    try {
        const Print       *print = m_plate_print;
        const PrintObject *po    = m_plate_object;
        std::unique_ptr<Print> own_print;
        if (po != nullptr) {
            m_result.from_sliced_plate = true;
        } else {
            if (!m_model) {
                m_error = _u8L("Nothing to slice.");
                return;
            }
            own_print = std::make_unique<Print>();
            own_print->set_status_callback([&ctl, &title](const PrintBase::SlicingStatus &status) {
                if (status.percent >= 0)
                    ctl.update_status(std::clamp(status.percent, 0, 100) * 6 / 10,
                                      status.text.empty() ? title : title + ": " + status.text);
            });
            // Slicing checks its own cancel flag; mirror the job's into it.
            std::atomic<bool> sliced { false };
            Print            *raw = own_print.get();
            std::thread       watcher([&ctl, &sliced, raw]() {
                while (!sliced.load()) {
                    if (ctl.was_canceled()) {
                        raw->cancel();
                        return;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            });
            try {
                own_print->apply(*m_model, m_config);
                PrintObject *target = nullptr;
                for (size_t i = 0; i < own_print->objects().size() && target == nullptr; ++i) {
                    PrintObject *candidate = own_print->get_object(i);
                    if (candidate->model_object()->id() != m_request.object_id)
                        continue;
                    for (const PrintInstance &instance : candidate->instances())
                        if (instance.model_instance != nullptr && instance.model_instance->id() == m_request.instance_id)
                            target = candidate;
                }
                if (target == nullptr)
                    throw Slic3r::RuntimeError(_u8L("The object has nothing to print."));
                own_print->process_perimeters_only(*target);
                po = target;
            } catch (...) {
                sliced = true;
                watcher.join();
                throw;
            }
            sliced = true;
            watcher.join();
            print = own_print.get();
        }
        throw_if_canceled();

        ctl.update_status(60, title + ": " + _u8L("Finding the seams"));
        const std::vector<std::vector<SeamPlacer::PlannedSeam>> planned =
            SeamPlacer::plan_object_seams(*print, *po, m_request.mode, m_request.prefer_part_joints, !m_request.replace,
                                          throw_if_canceled);

        // The planned seams are in the print object's centred coordinates; each part's mesh gets there through the
        // print object's own copy of the part's matrix.
        const std::vector<const ModelVolume *> parts = model_parts(*po->model_object());
        if (parts.size() != m_request.parts.size()) {
            m_error = _u8L("The object changed while the seams were computed.");
            return;
        }
        for (size_t i = 0; i < parts.size(); ++i)
            if (parts[i]->id() != m_request.parts[i].volume_id) {
                m_error = _u8L("The object changed while the seams were computed.");
                return;
            }

        ctl.update_status(80, title + ": " + _u8L("Painting"));
        std::vector<std::unique_ptr<TriangleSelector>> selectors;
        std::vector<SeamAutoPaint::Volume>             volumes;
        const Transform3d                              object_trafo = po->trafo_centered();
        for (size_t i = 0; i < parts.size(); ++i) {
            const SeamAutoPaintRequest::Part &part = m_request.parts[i];
            selectors.emplace_back(std::make_unique<TriangleSelector>(*part.mesh));
            if (!m_request.replace)
                selectors.back()->deserialize(part.paint);
            volumes.push_back({ &part.mesh->its, object_trafo * parts[i]->get_matrix(), selectors.back().get() });
        }
        SeamAutoPaint::Params params;
        params.radius = m_request.strip_width > 0.f ? 0.5f * m_request.strip_width : 0.f;
        const SeamAutoPaint::Stats stats = SeamAutoPaint::paint(planned, volumes, params, throw_if_canceled);

        m_result.object_id = m_request.object_id;
        m_result.seams     = stats.seams;
        for (size_t i = 0; i < parts.size(); ++i) {
            m_result.volume_ids.push_back(m_request.parts[i].volume_id);
            m_result.paint.push_back(selectors[i]->serialize());
        }
        BOOST_LOG_TRIVIAL(info) << "Auto-paint seam: " << stats.seams << " seams, " << stats.links << " links, "
                                << stats.strokes << " strokes, walls from "
                                << (m_result.from_sliced_plate ? "the sliced plate" : "a private slice");
        ctl.update_status(100, title);
    } catch (const CanceledException &) {
        m_result = {};
    }
}

void SeamAutoPaintJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (eptr) {
        try {
            std::rethrow_exception(eptr);
        } catch (const CanceledException &) {
            canceled = true;
        } catch (const std::exception &e) {
            m_error = e.what();
        }
        eptr = nullptr;
    }
    if (canceled || m_result.object_id.invalid()) {
        if (!m_error.empty())
            wxGetApp().notification_manager()->push_plater_warning_notification(
                (boost::format(_u8L("Auto-paint seam failed: %1%")) % m_error).str());
        return;
    }
    if (m_finish)
        m_finish(std::move(m_result));
}

}} // namespace Slic3r::GUI
