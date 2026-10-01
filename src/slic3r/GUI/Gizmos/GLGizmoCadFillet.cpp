#include "GLGizmoCadFillet.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/Jobs/Worker.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/LocalesUtils.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <glad/gl.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <sstream>

namespace Slic3r::GUI {

// Edges and highlights are drawn as thin tubes, a fraction of the part's size thick, so they
// read the same on a 5 mm part and a 300 mm one (the Edit gizmo's chain tubes do the same).
static constexpr float CadTubeRadiusFraction = 0.0018f;
// How close (screen pixels, before DPI scaling) the cursor must come to an edge to pick it.
static constexpr double CadPickPixels = 9.;

struct GLGizmoCadFillet::JobOutput
{
    std::shared_ptr<const BRep::CadBody> body;
    std::unique_ptr<BRep::CadTopology>   topo;
    std::shared_ptr<BRep::CadOpResult>   op;
    BRep::MeshConversionReport           conversion;
    bool                                 converted = false;
    std::string                          error;
};

// A warning that wraps at the panel's width: OCCT's reasons can be long, and ImGuiWrapper's
// warning_text() draws one unwrapped line that the fixed-width panel cuts off.
static void warning_wrapped(const wxString &text, float wrap_width)
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGuiWrapper::to_ImVec4(ColorRGB::WARNING()));
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);
    ImGui::TextUnformatted(into_u8(text).c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

GLGizmoCadFillet::GLGizmoCadFillet(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

GLGizmoCadFillet::~GLGizmoCadFillet() = default;

bool GLGizmoCadFillet::on_init()
{
    m_shortcut_key = 0;

    m_desc["mode"]            = _L("Operation");
    m_desc["fillet"]          = _L("Fillet");
    m_desc["chamfer"]         = _L("Chamfer");
    m_desc["shell"]           = _L("Shell");
    m_desc["radius"]          = _L("Radius");
    m_desc["distance"]        = _L("Distance");
    m_desc["thickness"]       = _L("Wall");
    m_desc["tangent"]         = _L("Select tangent chain");
    m_desc["no_part"]         = _L("Select a single part to fillet, chamfer or shell it.");
    m_desc["loading"]         = _L("Reading the CAD body...");
    m_desc["working"]         = _L("Working...");
    m_desc["src_attached"]    = _L("Exact CAD body: %1% faces, %2% edges.");
    m_desc["src_step"]        = _L("Exact shape from the STEP file: %1% faces, %2% edges.");
    m_desc["ops_done"]        = _L("%1% exact operation(s) applied so far.");
    m_desc["needs_convert"]   = _L("This part is a triangle mesh. Exact fillets need a CAD body: convert the mesh first. "
                                   "Flat areas become faces; curved areas stay faceted, so fillets there follow the facets.");
    m_desc["convert"]         = _L("Convert to CAD body");
    m_desc["convert_big"]     = _L("This mesh has %1% triangles. Converting it can take a while, and a faceted body "
                                   "with many faces is slow to fillet.");
    m_desc["convert_limit"]   = _L("This mesh has %1% triangles, more than the %2% that can be converted. Simplify it first.");
    m_desc["hint_edges"]      = _L("Click edges to select them, click again to deselect. Shift+click a face to select all of its edges.");
    m_desc["hint_faces"]      = _L("Click the faces to leave open. The wall is built inward, so the outside keeps its size.");
    m_desc["sel_edges"]       = _L("Selected: %1% edge(s)");
    m_desc["sel_faces"]       = _L("Open faces: %1%");
    m_desc["clear"]           = _L("Clear selection");
    m_desc["preview"]         = _L("Preview");
    m_desc["apply"]           = _L("Apply");
    m_desc["discard"]         = _L("Discard preview");
    m_desc["previewing"]      = _L("Previewing: %1% faces (was %2%), volume %3% mm3 (was %4%).");
    m_desc["paint"]           = _L("Applying replaces the part's triangles, so painted supports, seams, colors and fuzzy skin are cleared.");
    m_desc["busy"]            = _L("Another background task is running. Wait for it to finish, then try again.");
    m_desc["retry"]           = _L("Try again");
    m_desc["step_failed"]     = _L("The exact shape could not be recovered from the STEP file (%1%).");
    m_desc["scaled"]          = _L("Sizes are in the part's displayed (scaled) millimetres.");

    return true;
}

std::string GLGizmoCadFillet::on_get_name() const { return _u8L("Fillet / chamfer (CAD)"); }
std::string GLGizmoCadFillet::get_gizmo_entering_text() const { return _u8L("Entering CAD fillet gizmo"); }
std::string GLGizmoCadFillet::get_gizmo_leaving_text() const { return _u8L("Leaving CAD fillet gizmo"); }

CommonGizmosDataID GLGizmoCadFillet::on_get_requirements() const { return CommonGizmosDataID::SelectionInfo; }

bool GLGizmoCadFillet::on_is_activable() const
{
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return false;
    int o = -1, v = -1;
    return selected_volume(o, v) != nullptr;
}

// ------------------------------------------------------------------------------------------------
// session
// ------------------------------------------------------------------------------------------------

ModelVolume *GLGizmoCadFillet::selected_volume(int &object_idx, int &volume_idx) const
{
    object_idx = volume_idx = -1;
    const Selection              &selection = m_parent.get_selection();
    const Selection::IndicesList &idxs      = selection.get_volume_idxs();
    if (idxs.size() != 1)
        return nullptr;
    const GLVolume *gl_volume = selection.get_volume(*idxs.begin());
    if (gl_volume == nullptr)
        return nullptr;
    const GLVolume::CompositeID &cid     = gl_volume->composite_id;
    const ModelObjectPtrs       &objects = wxGetApp().model().objects;
    if (cid.object_id < 0 || objects.size() <= size_t(cid.object_id))
        return nullptr;
    ModelObject *object = objects[cid.object_id];
    if (cid.volume_id < 0 || object->volumes.size() <= size_t(cid.volume_id))
        return nullptr;
    ModelVolume *mv = object->volumes[cid.volume_id];
    if (!mv->is_model_part() || mv->mesh().empty())
        return nullptr;
    object_idx = cid.object_id;
    volume_idx = cid.volume_id;
    return mv;
}

bool GLGizmoCadFillet::volume_alive() const
{
    // After an undo / redo the Model's volumes may have been rebuilt: never dereference a
    // pointer that is no longer in the model.
    if (m_volume == nullptr)
        return false;
    for (const ModelObject *o : wxGetApp().model().objects)
        for (const ModelVolume *v : o->volumes)
            if (v == m_volume)
                return true;
    return false;
}

void GLGizmoCadFillet::detach()
{
    if (!volume_alive())
        m_volume = nullptr;
    clear_preview();
    ++m_token; // any job still running for the old session is dropped when it finishes
    m_volume     = nullptr;
    m_volume_id  = ObjectID();
    m_object_idx = m_volume_idx = -1;
    m_stage      = Stage::NoPart;
    m_body.reset();
    m_body_from_step = false;
    m_topo.reset();
    m_raycaster.reset();
    m_load_error.clear();
    m_job_running = false;
    m_last_error.clear();
    m_last_info.clear();
    clear_selection();
}

void GLGizmoCadFillet::attach_to_selection()
{
    int          object_idx = -1, volume_idx = -1;
    ModelVolume *mv         = selected_volume(object_idx, volume_idx);
    if (mv == nullptr) {
        detach();
        return;
    }
    if (mv == m_volume && mv->id() == m_volume_id && m_stage != Stage::NoPart)
        return;

    detach();
    m_volume         = mv;
    m_volume_id      = mv->id();
    m_object_idx     = object_idx;
    m_volume_idx     = volume_idx;
    m_mesh_triangles = int(mv->mesh().facets_count());

    if (std::shared_ptr<const BRep::CadBody> body = BRep::attached_cad_body(*mv))
        load_body_async(std::move(body), false);
    else if (BRep::is_step_file(mv->source.input_file))
        load_body_async(nullptr, true);
    else
        m_stage = Stage::NeedsConversion;
}

void GLGizmoCadFillet::data_changed(bool /*is_serializing*/)
{
    if (m_state != On)
        return;
    attach_to_selection();
}

void GLGizmoCadFillet::on_set_state()
{
    if (m_state == On)
        attach_to_selection();
    else
        detach();
}

Transform3d GLGizmoCadFillet::volume_trafo() const
{
    if (m_volume == nullptr || m_volume->get_object() == nullptr)
        return Transform3d::Identity();
    const ModelObject *mo           = m_volume->get_object();
    int                instance_idx = m_parent.get_selection().get_instance_idx();
    if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
        instance_idx = 0;
    if (mo->instances.empty())
        return m_volume->get_matrix();
    return mo->instances[size_t(instance_idx)]->get_transformation().get_matrix() * m_volume->get_matrix();
}

double GLGizmoCadFillet::mesh_scale() const
{
    // Sizes are typed in world millimetres; the body lives in the volume's mesh space. As the
    // Edit gizmo does, a roughly uniform scale is assumed and the mean factor used.
    return BRep::mean_scale(volume_trafo());
}

BRep::TessellationParams GLGizmoCadFillet::apply_tessellation() const
{
    // STEP import's own precision, so an exact edit re-tessellates exactly as finely as the
    // imported part was.
    BRep::TessellationParams p;
    if (AppConfig *cfg = wxGetApp().app_config; cfg != nullptr) {
        const std::string lin = cfg->get("linear_defletion");
        const std::string ang = cfg->get("angle_defletion");
        if (!lin.empty()) {
            const double v = string_to_double_decimal_point(lin);
            if (v > 0. && std::isfinite(v))
                p.linear_deflection = v;
        }
        if (!ang.empty()) {
            const double v = string_to_double_decimal_point(ang);
            if (v > 0. && std::isfinite(v))
                p.angular_deflection = v;
        }
    }
    return p;
}

// ------------------------------------------------------------------------------------------------
// worker
// ------------------------------------------------------------------------------------------------

bool GLGizmoCadFillet::run_job(const std::string &status, std::function<void(JobOutput &)> work, std::function<void(JobOutput &)> done)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return false;
    Worker &worker = plater->get_ui_job_worker();
    if (m_job_running || !worker.is_idle()) {
        m_last_error = into_u8(m_desc.at("busy"));
        return false;
    }
    auto           out   = std::make_shared<JobOutput>();
    const uint64_t token = m_token;
    m_job_running        = true;
    m_job_status         = status;
    queue_job(
        worker,
        [out, work = std::move(work), status](Job::Ctl &ctl) {
            ctl.update_status(0, status);
            try {
                work(*out);
            } catch (const std::exception &e) {
                out->error = e.what();
            } catch (...) {
                out->error = "unknown error";
            }
            ctl.update_status(100, status);
        },
        [this, out, done = std::move(done), token](bool canceled, std::exception_ptr &eptr) {
            if (eptr) {
                try {
                    std::rethrow_exception(eptr);
                } catch (const std::exception &e) {
                    out->error = e.what();
                } catch (...) {
                    out->error = "unknown error";
                }
                eptr = nullptr;
            }
            if (token != m_token)
                return; // the gizmo moved on to another part, or closed
            m_job_running = false;
            m_job_status.clear();
            if (!canceled)
                done(*out);
            m_parent.set_as_dirty();
        });
    return true;
}

void GLGizmoCadFillet::load_body_async(std::shared_ptr<const BRep::CadBody> body, bool from_step_source)
{
    if (m_volume == nullptr)
        return;
    m_stage          = Stage::Loading;
    m_body_from_step = from_step_source;
    m_load_error.clear();

    // The worker gets copies: the source reference, the (immutable, shared) mesh, and the body.
    const BRep::StepSourceRef                 source = BRep::step_source_ref(*m_volume);
    const std::shared_ptr<const TriangleMesh> mesh   = m_volume->mesh_ptr();
    const double diag = std::max(1., m_volume->mesh().bounding_box().size().norm());
    // The pick mesh and edge polylines need no more than a fraction of the part's size.
    const double lin = std::clamp(diag * 1e-3, 0.01, 0.2);

    const bool queued = run_job(
        from_step_source ? _u8L("Reading the part's STEP file") : _u8L("Reading the CAD body"),
        [body, from_step_source, source, mesh, lin](JobOutput &out) {
            std::shared_ptr<const BRep::CadBody> b = body;
            if (!b && from_step_source) {
                std::string why;
                b = BRep::cad_body_from_step_source(source, mesh->its, &why);
                if (!b) {
                    out.error = why;
                    return;
                }
            }
            if (!b)
                return;
            out.body = b;
            out.topo = std::make_unique<BRep::CadTopology>(BRep::cad_topology(*b, lin, 0.35));
        },
        [this](JobOutput &out) { on_body_loaded(out); });
    if (!queued) {
        m_stage      = Stage::Failed;
        m_load_error = into_u8(m_desc.at("busy"));
    }
}

void GLGizmoCadFillet::on_body_loaded(JobOutput &out)
{
    if (!out.body || !out.topo) {
        if (m_body_from_step) {
            // Not the STEP file's tessellation any more (or the file is gone): offer conversion.
            m_stage      = Stage::NeedsConversion;
            m_load_error = into_u8(GUI::format_wxstr(m_desc.at("step_failed"), out.error));
        } else {
            m_stage      = Stage::Failed;
            m_load_error = out.error.empty() ? _u8L("The CAD body could not be read.") : out.error;
        }
        return;
    }
    m_body      = out.body;
    m_topo      = std::move(out.topo);
    m_edges_model.reset(); // built from the topology it had
    m_raycaster = std::make_unique<MeshRaycaster>(std::make_shared<const TriangleMesh>(m_topo->mesh));
    const double diag = m_topo->bbox.defined ? m_topo->bbox.size().norm() : 10.;
    m_tube_radius     = std::max(0.004f, float(diag) * CadTubeRadiusFraction);
    m_stage           = Stage::Ready;
    clear_selection();
    m_models_dirty = true;
}

void GLGizmoCadFillet::convert_async()
{
    if (m_volume == nullptr || m_mesh_triangles > BRep::ConvertMaxTriangles)
        return;
    const std::shared_ptr<const TriangleMesh> mesh = m_volume->mesh_ptr();
    const double diag = std::max(1., m_volume->mesh().bounding_box().size().norm());
    const double lin  = std::clamp(diag * 1e-3, 0.01, 0.2);
    m_last_error.clear();
    const Stage previous = m_stage;
    m_stage              = Stage::Loading;
    const bool queued = run_job(
        _u8L("Converting the part to a CAD body"),
        [mesh, lin](JobOutput &out) {
            out.body = BRep::cad_body_from_mesh(mesh->its, out.conversion);
            if (!out.body) {
                out.error = out.conversion.error;
                return;
            }
            out.converted = true;
            out.topo      = std::make_unique<BRep::CadTopology>(BRep::cad_topology(*out.body, lin, 0.35));
        },
        [this](JobOutput &out) {
            if (!out.body || !out.topo) {
                m_stage      = Stage::NeedsConversion;
                m_load_error = out.error.empty() ? _u8L("The conversion failed.") : out.error;
                return;
            }
            commit_conversion(out.body);
            m_body_from_step = false;
            on_body_loaded(out);
            m_last_info = GUI::format(_u8L("Converted %1% triangles to %2% faces in %3% s."), out.conversion.triangles,
                                      out.conversion.faces, GUI::format("%.1f", out.conversion.seconds));
        });
    if (!queued)
        m_stage = previous;
}

void GLGizmoCadFillet::commit_conversion(std::shared_ptr<const BRep::CadBody> body)
{
    if (m_volume == nullptr || !body)
        return;
    // The mesh does not change, only the body joins it - but as its own undo step, so that
    // Undo returns to the plain mesh.
    Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Convert to CAD body"), UndoRedo::SnapshotType::GizmoAction);
    m_volume->cad_body = std::move(body);
}

// ------------------------------------------------------------------------------------------------
// the operation
// ------------------------------------------------------------------------------------------------

std::string GLGizmoCadFillet::operation_key() const
{
    std::ostringstream key;
    key << int(m_mode) << ':';
    if (m_mode == Mode::Shell) {
        key << m_thickness << ':';
        for (int f : m_sel_faces)
            key << f << ',';
    } else {
        key << (m_mode == Mode::Fillet ? m_radius : m_distance) << ':';
        for (int e : m_sel_edges)
            key << e << ',';
    }
    key << ':' << mesh_scale();
    return key.str();
}

void GLGizmoCadFillet::show_preview_mesh(const indexed_triangle_set &its)
{
    if (m_object_idx < 0 || m_volume_idx < 0)
        return;
    for (GLVolume *v : m_parent.get_volumes().volumes) {
        if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
            continue;
        v->model.reset();
        v->model.init_from(its);
    }
    m_preview_shown = true;
    m_parent.set_as_dirty();
}

void GLGizmoCadFillet::clear_preview()
{
    m_preview.reset();
    m_preview_topo.reset();
    m_preview_key.clear();
    if (!m_preview_shown)
        return;
    m_preview_shown = false;
    if (m_volume != nullptr && m_object_idx >= 0 && m_volume_idx >= 0)
        for (GLVolume *v : m_parent.get_volumes().volumes) {
            if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
                continue;
            v->model.reset();
            v->model.init_from(m_volume->mesh().its);
        }
    m_parent.set_as_dirty();
}

void GLGizmoCadFillet::run_operation_async(bool apply)
{
    if (m_stage != Stage::Ready || !m_body || m_job_running)
        return;
    const std::string key = operation_key();
    if (apply && m_preview && m_preview_key == key && m_preview->ok()) {
        // Apply what the user is looking at.
        const std::shared_ptr<BRep::CadOpResult> result = m_preview;
        std::unique_ptr<BRep::CadTopology>       topo;
        if (m_preview_topo)
            topo = std::make_unique<BRep::CadTopology>(*m_preview_topo);
        commit(*result, std::move(topo));
        return;
    }

    const Mode                           mode  = m_mode;
    const double                         scale = mesh_scale();
    const double                         size  = double(mode == Mode::Fillet ? m_radius : mode == Mode::Chamfer ? m_distance : m_thickness) / scale;
    const std::vector<int>               edges(m_sel_edges.begin(), m_sel_edges.end());
    const std::vector<int>               faces(m_sel_faces.begin(), m_sel_faces.end());
    const BRep::TessellationParams       tess = apply_tessellation();
    const std::shared_ptr<const BRep::CadBody> body = m_body;
    const double topo_lin = m_topo && m_topo->bbox.defined ? std::clamp(m_topo->bbox.size().norm() * 1e-3, 0.01, 0.2) : 0.05;
    m_last_error.clear();

    const std::string status = apply ? (mode == Mode::Shell ? _u8L("Shelling the part") : _u8L("Applying the exact edge feature"))
                                     : _u8L("Previewing the exact edit");
    run_job(
        status,
        [mode, size, edges, faces, tess, body, topo_lin](JobOutput &out) {
            if (mode == Mode::Shell)
                out.op = std::make_shared<BRep::CadOpResult>(BRep::shell_solid(*body, faces, size, tess));
            else
                out.op = std::make_shared<BRep::CadOpResult>(BRep::fillet_edges(
                    *body, mode == Mode::Fillet ? BRep::EdgeFeature::Fillet : BRep::EdgeFeature::Chamfer, size, edges, tess));
            // The result's own topology, for picking the next operation, while still on the worker.
            if (out.op->ok())
                out.topo = std::make_unique<BRep::CadTopology>(BRep::cad_topology(*out.op->body, topo_lin, 0.35));
        },
        [this, apply, key, mode, scale](JobOutput &out) {
            if (!out.op || !out.op->ok()) {
                clear_preview();
                m_last_error = out.op ? out.op->error : out.error;
                if (m_last_error.empty())
                    m_last_error = _u8L("The operation failed.");
                // Sentence case for a message that starts mid-sentence in libslic3r.
                m_last_error[0] = char(std::toupper(static_cast<unsigned char>(m_last_error[0])));
                // What would fit, in the sizes the panel shows (world millimetres).
                if (out.op && (mode == Mode::Fillet || mode == Mode::Chamfer)) {
                    if (out.op->largest_size > 0.)
                        m_last_error += " " + GUI::format(mode == Mode::Fillet ?
                                                              _u8L("The largest radius that works for this selection is about %1% mm.") :
                                                              _u8L("The largest distance that works for this selection is about %1% mm."),
                                                          GUI::format("%.2f", out.op->largest_size * scale));
                    else if (out.op->shortest_edge > 0. && out.op->status == BRep::CadOpStatus::Failed)
                        m_last_error += " " + GUI::format(_u8L("Even much smaller sizes fail on this selection; its shortest edge is %1% mm."),
                                                          GUI::format("%.2f", out.op->shortest_edge * scale));
                }
                m_last_error += " " + _u8L("Nothing was changed.");
                BOOST_LOG_TRIVIAL(error) << "CAD gizmo: " << m_last_error;
                return;
            }
            if (apply) {
                commit(*out.op, std::move(out.topo));
                return;
            }
            m_preview      = out.op;
            m_preview_topo = std::shared_ptr<BRep::CadTopology>(std::move(out.topo));
            m_preview_key  = key;
            show_preview_mesh(m_preview->mesh);
        });
}

void GLGizmoCadFillet::commit(const BRep::CadOpResult &result, std::unique_ptr<BRep::CadTopology> topo)
{
    if (m_volume == nullptr || !result.ok())
        return;
    // Put the real mesh back under the renderer first; the scene is rebuilt below anyway.
    const std::shared_ptr<BRep::CadOpResult> keep = m_preview; // `result` may live in it
    clear_preview();

    Plater      *plater = wxGetApp().plater();
    ModelVolume *volume = m_volume;
    ModelObject *object = volume->get_object();
    std::string  label;
    size_t       count = 0;
    switch (m_mode) {
    case Mode::Fillet:  label = _u8L("Fillet edges (CAD)"); count = m_sel_edges.size(); break;
    case Mode::Chamfer: label = _u8L("Chamfer edges (CAD)"); count = m_sel_edges.size(); break;
    case Mode::Shell:   label = _u8L("Shell part (CAD)"); count = m_sel_faces.size(); break;
    }

    bool had_paint = false;
    {
        // ONE undo step per Apply.
        Plater::TakeSnapshot snapshot(plater, label, UndoRedo::SnapshotType::GizmoAction);
        const bool sinking = object->min_z() < SINKING_Z_THRESHOLD;
        had_paint          = BRep::apply_cad_result(*volume, result);
        object->invalidate_bounding_box();
        if (!sinking)
            object->ensure_on_bed();
        // Adopt the new identity BEFORE the plater's update reaches data_changed(), so that it
        // does not look like a different part.
        m_volume_id      = volume->id();
        m_mesh_triangles = int(volume->mesh().facets_count());
        plater->changed_mesh(m_object_idx);
        wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);
    }

    std::string message;
    switch (m_mode) {
    case Mode::Fillet:  message = GUI::format(_u8L("Filleted %1% edge(s) of \"%2%\" exactly."), count, volume->name); break;
    case Mode::Chamfer: message = GUI::format(_u8L("Chamfered %1% edge(s) of \"%2%\" exactly."), count, volume->name); break;
    case Mode::Shell:   message = GUI::format(_u8L("Shelled \"%1%\" with %2% open face(s)."), volume->name, count); break;
    }
    if (had_paint)
        message += " " + _u8L("Its painted supports, seams and colors were removed because the mesh changed.");
    plater->get_notification_manager()->push_notification(NotificationType::CustomNotification,
                                                           had_paint ? NotificationManager::NotificationLevel::WarningNotificationLevel :
                                                                       NotificationManager::NotificationLevel::RegularNotificationLevel,
                                                           message);

    m_last_error.clear();
    m_last_info = GUI::format(_u8L("Applied: %1% faces (was %2%)."), result.faces_after, result.faces_before);
    // The edited body's topology is new (edge numbers changed), so the selection goes.
    clear_selection();
    std::shared_ptr<const BRep::CadBody> body = BRep::attached_cad_body(*volume);
    if (body && topo) {
        JobOutput out;
        out.body = std::move(body);
        out.topo = std::move(topo);
        m_body_from_step = false;
        on_body_loaded(out);
    } else if (body) {
        load_body_async(std::move(body), false);
    } else {
        m_stage      = Stage::Failed;
        m_load_error = _u8L("The CAD body was not kept with the part.");
    }
}

// ------------------------------------------------------------------------------------------------
// picking
// ------------------------------------------------------------------------------------------------

int GLGizmoCadFillet::pick_edge(const Vec2d &mouse_pos, bool has_hit, const Vec3d &hit_world) const
{
    const Camera            &camera = wxGetApp().plater()->get_camera();
    const std::array<int, 4> &vp    = camera.get_viewport();
    const Transform3d         trafo = volume_trafo();
    const Matrix4d            view  = camera.get_view_matrix().matrix();
    const Matrix4d            mvp   = camera.get_projection_matrix().matrix() * view * trafo.matrix();

    auto to_screen = [&](const Vec3f &p, Vec2d &s) {
        const Eigen::Vector4d c = mvp * Eigen::Vector4d(p.x(), p.y(), p.z(), 1.);
        if (c.w() <= 1e-12)
            return false;
        const double x = c.x() / c.w(), y = c.y() / c.w();
        s = Vec2d(double(vp[0]) + double(vp[2]) * (x + 1.) * 0.5, double(vp[3]) - (double(vp[1]) + double(vp[3]) * (y + 1.) * 0.5));
        return true;
    };
    auto depth = [&](const Vec3d &world) { return -(view * Eigen::Vector4d(world.x(), world.y(), world.z(), 1.)).z(); };

    const double diag      = m_topo->bbox.defined ? (trafo.linear() * m_topo->bbox.size()).norm() : 10.;
    const double depth_tol = 0.01 * diag + 0.05;
    const double hit_depth = has_hit ? depth(hit_world) : DBL_MAX;
    const double max_px    = CadPickPixels * double(m_imgui ? m_imgui->get_style_scaling() : 1.f);

    int    best       = -1;
    double best_px    = max_px;
    double best_depth = DBL_MAX;
    for (int e = 0; e < m_topo->num_edges; ++e) {
        if (!m_topo->edge_selectable[size_t(e)])
            continue;
        const std::vector<Vec3f> &pl = m_topo->edge_polylines[size_t(e)];
        Vec2d                     prev_s;
        bool                      prev_ok = false;
        for (size_t i = 0; i < pl.size(); ++i) {
            Vec2d      s;
            const bool ok = to_screen(pl[i], s);
            if (ok && prev_ok) {
                // Closest point of the screen segment to the cursor.
                const Vec2d  d  = s - prev_s;
                const double l2 = d.squaredNorm();
                const double t  = l2 > 1e-12 ? std::clamp((mouse_pos - prev_s).dot(d) / l2, 0., 1.) : 0.;
                const double px = (prev_s + t * d - mouse_pos).norm();
                if (px <= best_px + 2.) {
                    const Vec3d  w  = trafo * ((1. - t) * pl[i - 1].cast<double>() + t * pl[i].cast<double>());
                    const double dz = depth(w);
                    // Hidden behind the surface the cursor is on: not this one.
                    if (dz <= hit_depth + depth_tol && (px < best_px - 2. || dz < best_depth)) {
                        best       = e;
                        best_px    = std::min(best_px, px);
                        best_depth = dz;
                    }
                }
            }
            prev_s  = s;
            prev_ok = ok;
        }
    }
    return best;
}

void GLGizmoCadFillet::update_hover(const Vec2d &mouse_pos, bool shift_down)
{
    m_last_mouse  = mouse_pos;
    const int old_edge = m_hover_edge, old_face = m_hover_face;
    const bool old_shift = m_hover_shift;
    m_hover_edge  = -1;
    m_hover_face  = -1;
    m_hover_shift = shift_down;
    m_hover_edges.clear();
    if (m_stage != Stage::Ready || !m_topo || !m_raycaster || m_job_running) {
        if (old_edge != -1 || old_face != -1)
            m_models_dirty = true;
        return;
    }

    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d trafo  = volume_trafo();
    Vec3f             hit    = Vec3f::Zero(), normal = Vec3f::Zero();
    size_t            facet  = 0;
    const bool        has_hit = m_raycaster->unproject_on_mesh(mouse_pos, trafo, camera, hit, normal, nullptr, &facet, false);
    const Vec3d       hit_world = has_hit ? Vec3d(trafo * hit.cast<double>()) : Vec3d::Zero();
    const int         face      = has_hit && facet < m_topo->triangle_face.size() ? m_topo->triangle_face[facet] : -1;

    if (m_mode == Mode::Shell) {
        m_hover_face = face;
    } else {
        m_hover_edge = pick_edge(mouse_pos, has_hit, hit_world);
        if (m_hover_edge >= 0)
            m_hover_edges = m_tangent_chain ? BRep::tangent_chain(*m_topo, m_hover_edge) : std::vector<int>{m_hover_edge};
        else if (shift_down && face >= 0) {
            m_hover_face  = face;
            m_hover_edges = BRep::face_selectable_edges(*m_topo, face);
        }
    }
    if (m_hover_edge != old_edge || m_hover_face != old_face || m_hover_shift != old_shift)
        m_models_dirty = true;
}

void GLGizmoCadFillet::toggle_edges(const std::vector<int> &edges)
{
    if (edges.empty())
        return;
    // A group toggles as one: if all of it is selected it goes, otherwise all of it comes in.
    const bool all_in = std::all_of(edges.begin(), edges.end(), [this](int e) { return m_sel_edges.count(e) != 0; });
    for (int e : edges)
        if (all_in)
            m_sel_edges.erase(e);
        else
            m_sel_edges.insert(e);
    m_models_dirty = true;
}

void GLGizmoCadFillet::clear_selection()
{
    m_sel_edges.clear();
    m_sel_faces.clear();
    m_hover_edge = m_hover_face = -1;
    m_hover_edges.clear();
    m_models_dirty = true;
}

bool GLGizmoCadFillet::on_mouse(const wxMouseEvent &mouse_event)
{
    if (m_volume == nullptr)
        return false;
    const Vec2d mouse_pos(double(mouse_event.GetX()), double(mouse_event.GetY()));

    if (mouse_event.Moving()) {
        update_hover(mouse_pos, mouse_event.ShiftDown());
        m_parent.set_as_dirty();
        return false;
    }
    if (mouse_event.LeftDown()) {
        update_hover(mouse_pos, mouse_event.ShiftDown());
        if (m_mode == Mode::Shell) {
            if (m_hover_face < 0)
                return false;
            if (!m_sel_faces.erase(m_hover_face))
                m_sel_faces.insert(m_hover_face);
            m_models_dirty = true;
        } else {
            if (m_hover_edges.empty())
                return false;
            toggle_edges(m_hover_edges);
        }
        m_parent.set_as_dirty();
        return true;
    }
    if (mouse_event.Leaving()) {
        m_hover_edge = m_hover_face = -1;
        m_hover_edges.clear();
        m_models_dirty = true;
        m_parent.set_as_dirty();
    }
    return false;
}

// ------------------------------------------------------------------------------------------------
// rendering
// ------------------------------------------------------------------------------------------------

void GLGizmoCadFillet::build_tubes(const std::vector<int> &edges, float radius, GLModel &model) const
{
    model.reset();
    if (!m_topo || edges.empty())
        return;
    GLModel::Geometry data;
    data.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
    constexpr int Sides = 6;
    unsigned int  base  = 0;
    for (int e : edges) {
        if (e < 0 || e >= m_topo->num_edges)
            continue;
        const std::vector<Vec3f> &pl = m_topo->edge_polylines[size_t(e)];
        for (size_t i = 1; i < pl.size(); ++i) {
            const Vec3f a = pl[i - 1], b = pl[i];
            Vec3f       axis = b - a;
            const float len  = axis.norm();
            if (len < 1e-9f)
                continue;
            axis /= len;
            Vec3f u = std::abs(axis.z()) < 0.9f ? axis.cross(Vec3f::UnitZ()) : axis.cross(Vec3f::UnitX());
            u.normalize();
            const Vec3f v = axis.cross(u);
            for (int k = 0; k < Sides; ++k) {
                const float ang = 2.f * float(M_PI) * float(k) / float(Sides);
                const Vec3f n   = u * std::cos(ang) + v * std::sin(ang);
                data.add_vertex(Vec3f(a + n * radius), n);
                data.add_vertex(Vec3f(b + n * radius), n);
            }
            for (int k = 0; k < Sides; ++k) {
                const unsigned int a0 = base + unsigned(2 * k), b0 = a0 + 1;
                const unsigned int a1 = base + unsigned(2 * ((k + 1) % Sides)), b1 = a1 + 1;
                data.add_triangle(a0, b0, b1);
                data.add_triangle(a0, b1, a1);
            }
            base += unsigned(2 * Sides);
        }
    }
    if (!data.is_empty())
        model.init_from(std::move(data));
}

void GLGizmoCadFillet::build_faces(const std::vector<int> &faces, GLModel &model) const
{
    model.reset();
    if (!m_topo || faces.empty())
        return;
    std::vector<int> triangles;
    for (size_t t = 0; t < m_topo->triangle_face.size(); ++t)
        if (std::find(faces.begin(), faces.end(), m_topo->triangle_face[t]) != faces.end())
            triangles.push_back(int(t));
    if (triangles.empty())
        return;
    // The Measure / Edit gizmos' plane overlay, lifted off the surface against z-fighting.
    model.init_from(init_plane_data(m_topo->mesh, triangles, m_tube_radius));
}

void GLGizmoCadFillet::rebuild_edge_models()
{
    if (!m_models_dirty)
        return;
    m_models_dirty = false;
    if (!m_topo) {
        m_edges_model.reset();
        m_hover_model.reset();
        m_selected_model.reset();
        m_hover_face_model.reset();
        m_selected_faces_model.reset();
        return;
    }
    if (!m_edges_model.is_initialized()) {
        std::vector<int> all;
        for (int e = 0; e < m_topo->num_edges; ++e)
            if (m_topo->edge_selectable[size_t(e)])
                all.push_back(e);
        build_tubes(all, m_tube_radius, m_edges_model);
    }
    build_tubes(std::vector<int>(m_sel_edges.begin(), m_sel_edges.end()), m_tube_radius * 1.9f, m_selected_model);
    build_tubes(m_hover_edges, m_tube_radius * 2.2f, m_hover_model);
    build_faces(m_hover_face >= 0 ? std::vector<int>{m_hover_face} : std::vector<int>{}, m_hover_face_model);
    build_faces(std::vector<int>(m_sel_faces.begin(), m_sel_faces.end()), m_selected_faces_model);
}

void GLGizmoCadFillet::render_overlays()
{
    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();
    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d trafo  = volume_trafo();
    const Transform3d view   = camera.get_view_matrix();
    shader->set_uniform("view_model_matrix", view * trafo);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix",
                        (Matrix3d) (view.matrix().block(0, 0, 3, 3) * trafo.matrix().block(0, 0, 3, 3).inverse().transpose()));
    shader->set_uniform("emission_factor", 0.25f);

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));

    auto draw = [](GLModel &m, const ColorRGBA &c) {
        if (!m.is_initialized())
            return;
        m.set_color(c);
        m.render();
    };
    draw(m_selected_faces_model, ColorRGBA(1.00f, 0.60f, 0.10f, 0.55f));
    draw(m_hover_face_model, ColorRGBA(0.30f, 0.70f, 0.95f, 0.35f));
    if (m_mode != Mode::Shell) {
        draw(m_edges_model, ColorRGBA(0.15f, 0.15f, 0.18f, 0.85f));
        draw(m_selected_model, ColorRGBA(1.00f, 0.55f, 0.05f, 1.00f));
        draw(m_hover_model, ColorRGBA(0.25f, 0.75f, 1.00f, 0.95f));
    }

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoCadFillet::on_render()
{
    if (m_volume == nullptr || m_stage != Stage::Ready || !m_topo || m_preview_shown)
        return;
    glsafe(::glEnable(GL_DEPTH_TEST));
    rebuild_edge_models();
    render_overlays();
}

// ------------------------------------------------------------------------------------------------
// panel
// ------------------------------------------------------------------------------------------------

void GLGizmoCadFillet::on_render_input_window(float x, float y, float bottom_limit)
{
    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());

    float label_col = 0.f;
    for (const char *k : {"mode", "radius", "distance", "thickness"})
        label_col = std::max(label_col, ImGuiWrapper::calc_text_size(m_desc.at(k)).x);
    label_col += m_imgui->scaled(1.f);
    const float window_width = std::max(m_imgui->scaled(22.f), label_col + m_imgui->scaled(14.f));
    y                        = std::min(y, bottom_limit - m_imgui->scaled(18.f));
    dock_setup_next_window(x, y, bottom_limit, window_width);
    GizmoImguiBegin(get_name(), dock_window_flags(ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                                  ImGuiWindowFlags_NoTitleBar));
    if (!dock_render_titlebar(get_name())) {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }
    const float wrap_width = ImGui::GetContentRegionAvail().x;
    auto        finish     = [this]() {
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
    };

    if (m_volume == nullptr || m_stage == Stage::NoPart) {
        m_imgui->text_wrapped(m_desc.at("no_part"), wrap_width);
        finish();
        return;
    }

    if (m_job_running) {
        m_imgui->text_wrapped(m_job_status.empty() ? m_desc.at("working") : from_u8(m_job_status + "..."), wrap_width);
        if (m_stage == Stage::Loading) {
            finish();
            return;
        }
    }

    // --- sourcing ---
    if (m_stage == Stage::Loading) {
        m_imgui->text_wrapped(m_desc.at("loading"), wrap_width);
        finish();
        return;
    }
    if (m_stage == Stage::Failed) {
        warning_wrapped(from_u8(m_load_error), wrap_width);
        if (m_imgui->button(m_desc.at("retry"))) {
            m_stage = Stage::NoPart; // makes attach_to_selection() start over
            attach_to_selection();
        }
        finish();
        return;
    }
    if (m_stage == Stage::NeedsConversion) {
        if (!m_load_error.empty())
            warning_wrapped(from_u8(m_load_error), wrap_width);
        m_imgui->text_wrapped(m_desc.at("needs_convert"), wrap_width);
        const bool too_big = m_mesh_triangles > BRep::ConvertMaxTriangles;
        if (too_big)
            warning_wrapped(GUI::format_wxstr(m_desc.at("convert_limit"), m_mesh_triangles, BRep::ConvertMaxTriangles), wrap_width);
        else if (m_mesh_triangles > BRep::ConvertWarnTriangles)
            warning_wrapped(GUI::format_wxstr(m_desc.at("convert_big"), m_mesh_triangles), wrap_width);
        if (m_imgui->button(m_desc.at("convert"), ImVec2(0.f, 0.f), !too_big && !m_job_running))
            convert_async();
        if (!m_last_error.empty())
            warning_wrapped(from_u8(m_last_error), wrap_width);
        finish();
        return;
    }

    // --- Ready ---
    m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at(m_body_from_step ? "src_step" : "src_attached"), m_topo->num_faces, m_topo->num_edges),
                          wrap_width);
    if (m_body && m_body->operations > 0)
        m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at("ops_done"), m_body->operations), wrap_width);
    ImGui::Separator();

    {
        const std::vector<std::string> labels = {into_u8(m_desc.at("fillet")), into_u8(m_desc.at("chamfer")), into_u8(m_desc.at("shell"))};
        int                            mode   = int(m_mode);
        if (render_combo(into_u8(m_desc.at("mode")), labels, mode, label_col, std::max(m_imgui->scaled(6.f), wrap_width - label_col)) &&
            mode != int(m_mode)) {
            m_mode = Mode(mode);
            clear_preview();
            m_last_error.clear();
            update_hover(m_last_mouse, false);
            m_models_dirty = true;
        }
    }

    auto size_row = [&](const wxString &label, const char *id, float *value, float slider_max) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(label_col);
        const float field_w  = m_imgui->scaled(4.f);
        const float slider_w = std::max(m_imgui->scaled(3.f), wrap_width - label_col - field_w - m_imgui->scaled(0.5f));
        ImGui::PushItemWidth(slider_w);
        bool changed = m_imgui->bbl_slider_float_style(id, value, 0.05f, slider_max, "%.2f mm", 1.0f, true);
        ImGui::SameLine();
        ImGui::PushItemWidth(field_w);
        changed |= ImGui::BBLDragFloat((std::string(id) + "_input").c_str(), value, 0.05f, 0.01f, 500.f, "%.2f");
        *value = std::clamp(*value, 0.01f, 500.f);
        return changed;
    };

    if (m_mode == Mode::Fillet)
        size_row(m_desc.at("radius"), "##cad_radius", &m_radius, 10.f);
    else if (m_mode == Mode::Chamfer)
        size_row(m_desc.at("distance"), "##cad_distance", &m_distance, 10.f);
    else
        size_row(m_desc.at("thickness"), "##cad_thickness", &m_thickness, 10.f);
    const Vec3d s = Geometry::Transformation(volume_trafo()).get_scaling_factor();
    if (!s.isApprox(Vec3d::Ones(), 1e-6))
        m_imgui->text_wrapped(m_desc.at("scaled"), wrap_width);

    if (m_mode != Mode::Shell) {
        if (m_imgui->bbl_checkbox(m_desc.at("tangent"), m_tangent_chain))
            update_hover(m_last_mouse, false);
        m_imgui->text_wrapped(m_desc.at("hint_edges"), wrap_width);
        m_imgui->text(GUI::format_wxstr(m_desc.at("sel_edges"), m_sel_edges.size()));
    } else {
        m_imgui->text_wrapped(m_desc.at("hint_faces"), wrap_width);
        m_imgui->text(GUI::format_wxstr(m_desc.at("sel_faces"), m_sel_faces.size()));
    }

    // A preview belongs to the exact selection and size it was built from.
    if (m_preview && m_preview_key != operation_key())
        clear_preview();

    ImGui::Separator();
    const bool has_selection = m_mode == Mode::Shell ? !m_sel_faces.empty() : !m_sel_edges.empty();
    const bool can_run       = has_selection && !m_job_running;
    if (m_preview_shown) {
        if (m_imgui->button(m_desc.at("apply"), ImVec2(0.f, 0.f), can_run))
            run_operation_async(true);
        ImGui::SameLine();
        if (m_imgui->button(m_desc.at("discard"), ImVec2(0.f, 0.f), !m_job_running))
            clear_preview();
        if (m_preview)
            m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at("previewing"), m_preview->faces_after, m_preview->faces_before,
                                                    wxString::Format("%.1f", m_preview->volume_after),
                                                    wxString::Format("%.1f", m_preview->volume_before)),
                                  wrap_width);
    } else {
        if (m_imgui->button(m_desc.at("preview"), ImVec2(0.f, 0.f), can_run))
            run_operation_async(false);
        ImGui::SameLine();
        if (m_imgui->button(m_desc.at("apply"), ImVec2(0.f, 0.f), can_run))
            run_operation_async(true);
        ImGui::SameLine();
        if (m_imgui->button(m_desc.at("clear"), ImVec2(0.f, 0.f), has_selection && !m_job_running)) {
            clear_selection();
            m_last_error.clear();
        }
    }

    if (!m_last_error.empty())
        warning_wrapped(from_u8(m_last_error), wrap_width);
    else if (!m_last_info.empty())
        m_imgui->text_wrapped(from_u8(m_last_info), wrap_width);

    ImGui::Separator();
    m_imgui->text_wrapped(m_desc.at("paint"), wrap_width);

    finish();
}

} // namespace Slic3r::GUI
