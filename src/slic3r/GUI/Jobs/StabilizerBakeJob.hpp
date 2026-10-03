#ifndef slic3r_GUI_StabilizerBakeJob_hpp_
#define slic3r_GUI_StabilizerBakeJob_hpp_

// "Bake stabilizers": the background half of the action (ObjectList::bake_stabilizers).
//
// Study: tests/research_stabilizer_bake.md (phase 1).
//
// The struts are planned and meshed off the UI thread from the SLICED PrintObject, which the worker
// may read because the plate is sliced and the background process is idle (the caller checks both
// before queueing; the plate is invalidated only in finalize(), on the main thread). Everything that
// touches the Model happens in finalize(), under one undo snapshot.

#include "Job.hpp"

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/Support/StabilizerBake.hpp"

#include <string>

namespace Slic3r {
class PrintObject;
namespace GUI {

class Plater;

class StabilizerBakeJob : public Job
{
public:
    // `print_object` is only read on the worker thread; finalize() re-finds the source ModelObject by
    // id, so a model edited while the bake ran is never written through a stale pointer.
    StabilizerBakeJob(Plater                      *plater,
                      const PrintObject           *print_object,
                      ObjectID                     model_object_id,
                      const StabilizerBakeOptions &options,
                      const std::string           &object_name);

    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;

private:
    Plater               *m_plater       = nullptr;
    const PrintObject    *m_print_object = nullptr;
    ObjectID              m_object_id;
    StabilizerBakeOptions m_options;
    std::string           m_name;

    StabilizerBakeResult  m_result;
    std::string           m_error;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_StabilizerBakeJob_hpp_
