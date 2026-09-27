#ifndef slic3r_GUI_SeamAutoPaintJob_hpp_
#define slic3r_GUI_SeamAutoPaintJob_hpp_

// "Auto-paint seam" in the seam painting gizmo: the background half.
//
// The worker asks SeamPlacer::plan_object_seams() where the slicer would put the seams for the chosen mode and joint
// preference, and paints them with SeamAutoPaint::paint() into private TriangleSelectors built over the parts'
// meshes. What it hands back is plain serialized paint per part; the gizmo applies it on the main thread (one undo
// snapshot) in finalize().
//
// The walls the planner needs come from one of two places:
//  * the plate's own sliced PrintObject, when its perimeters are done and the background slicing process is idle
//    (the same rule as SliceBakeJob; while a UI job runs, Plater::priv::restart_background_process() refuses to
//    start a slice, so the object is not re-sliced under the worker's feet);
//  * otherwise a private Print over a copy of the object (plus the objects touching it, for joints between
//    objects), sliced up to posPerimeters and nothing more (Print::process_perimeters_only()). The user's plate
//    and its slicing state are not touched.

#include "Job.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleSelector.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
class Model;
class ModelObject;
class Print;
class PrintObject;
class TriangleMesh;

namespace GUI {

class Plater;

struct SeamAutoPaintRequest
{
    // The object being painted and the instance whose placement on the bed decides where front, back, left and
    // right are.
    ObjectID     object_id;
    ObjectID     instance_id;
    SeamPosition mode               = spAligned;
    bool         prefer_part_joints = true;
    // Replace all existing seam paint (enforcers and blockers) instead of adding to it. The plan then ignores the
    // existing paint too.
    bool         replace            = true;
    // Width of the painted strip in mm; zero or less: twice the outer wall line width.
    float        strip_width        = 0.f;

    // The model parts in ModelObject::volumes order, the order of the gizmo's triangle selectors.
    struct Part
    {
        ObjectID                                volume_id;
        std::shared_ptr<const TriangleMesh>     mesh;
        TriangleSelector::TriangleSplittingData paint;
    };
    std::vector<Part> parts;
};

struct SeamAutoPaintResult
{
    ObjectID                                             object_id;
    std::vector<ObjectID>                                volume_ids;
    std::vector<TriangleSelector::TriangleSplittingData> paint;
    size_t                                               seams             = 0;
    bool                                                 from_sliced_plate = false;
};

class SeamAutoPaintJob : public Job
{
public:
    using Finish = std::function<void(SeamAutoPaintResult &&)>;

    // Plans on the plate's sliced PrintObject `po`, owned by `print`.
    SeamAutoPaintJob(SeamAutoPaintRequest request, const Print *print, const PrintObject *po, Finish finish);
    // Plans on a private Print of `model` (a copy) with `config`.
    SeamAutoPaintJob(SeamAutoPaintRequest request, std::unique_ptr<Model> model, DynamicPrintConfig config, Finish finish);
    ~SeamAutoPaintJob() override;

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

    // The sliced PrintObject of the plate that holds instance `instance_idx` of model object `obj_idx`, if its
    // walls are up to date for the model as it is now and the background process is idle; nullptr otherwise.
    // With `same_paint`, the Print's copy must also have the seam paint the model has now (the plan honours it).
    static const PrintObject *sliced_print_object(Plater *plater, int obj_idx, int instance_idx, bool same_paint,
                                                  const Print **print_out);
    // A copy of the model holding only that instance of the object, and the instances of other objects on the same
    // plate that touch it (only when `with_neighbours`), and the config the plate would be sliced with.
    static std::unique_ptr<Model> private_model(Plater *plater, int obj_idx, int instance_idx, bool with_neighbours,
                                                DynamicPrintConfig &config_out);

private:
    SeamAutoPaintRequest   m_request;
    const Print           *m_plate_print  = nullptr;
    const PrintObject     *m_plate_object = nullptr;
    std::unique_ptr<Model> m_model;
    DynamicPrintConfig     m_config;
    Finish                 m_finish;

    SeamAutoPaintResult m_result;
    std::string         m_error;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SeamAutoPaintJob_hpp_
