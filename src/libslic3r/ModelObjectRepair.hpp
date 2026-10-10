#ifndef slic3r_ModelObjectRepair_hpp_
#define slic3r_ModelObjectRepair_hpp_

#include <functional>

namespace Slic3r {

class ModelObject;

// Orca: Repairs the volumes of a model object with CGAL ("Fix model"): every volume when volume_idx is -1, otherwise that one.
// A volume made of disconnected shells is split into parts first. Parts that enclose no volume are dropped and parts with
// open edges are repaired. on_progress receives an untranslated message and a percentage. throw_on_cancel is called
// before each volume and aborts the repair by throwing. Throws Slic3r::RuntimeError when a part cannot be repaired.
void repair_model_object(ModelObject                                               &object,
                         int                                                        volume_idx,
                         const std::function<void(const char *message, int percent)> &on_progress,
                         const std::function<void()>                               &throw_on_cancel);

} // namespace Slic3r

#endif // slic3r_ModelObjectRepair_hpp_
