#pragma once

// The AMS mapping a Bambu print job carries, worked out ONCE for every sender: the desktop's send
// dialog (SelectMachineDialog), the phone's plate send and the phone's reprint of an archived job
// (RemoteSend). Before this file each of them had its own copy, and the phone's had drifted: it
// matched a two-extruder job against every AMS of the printer, where the dialog only offers a
// filament the AMS units feeding the extruder its G-code prints it with.
//
// Nothing here reads the plater or the presets: the caller says what the job is (its filaments,
// the filament -> extruder map it was sliced with, the printer profile's physical_extruder_map),
// so a job read back from a .gcode.3mf maps exactly like the plate that is on screen.

#include "libslic3r/ProjectTask.hpp" // FilamentInfo

#include <string>
#include <vector>

namespace Slic3r {
class MachineObject;
namespace GUI {
namespace BambuSendMapping {

// SelectMachineDialog::do_ams_mapping's matching step. `filament_map` is the 1-based logical
// extruder per project filament as sliced (1 = left, 2 = right), empty for a one-extruder job:
// then every filament is matched against every AMS, as before. Otherwise each side's filaments are
// matched only against the AMS units that feed that side's physical extruder
// (BambuExtruderMap::logical_to_physical over `physical_extruder_map`), and the result is sorted by
// filament id. Returns MachineObject::ams_filament_mapping's code (0 = mapped, 1 = order mapping on
// a printer without colour mapping, < 0 = nothing to map); the last non-zero side wins.
int auto_map(MachineObject* obj, const std::vector<FilamentInfo>& filaments, const std::vector<int>& filament_map,
             const std::vector<int>& physical_extruder_map, std::vector<FilamentInfo>& result);

// SelectMachineDialog::filaments_mapped_to_wrong_extruder: the 0-based filament ids whose mapped
// tray sits in an AMS that feeds another extruder than the sliced one. External spools and unknown
// units are not judged. Empty for a one-extruder job.
std::vector<int> wrong_extruder(MachineObject* obj, const std::vector<FilamentInfo>& result, const std::vector<int>& filament_map,
                                const std::vector<int>& physical_extruder_map);

// What the three mapping strings need to know about the job besides the mapping itself.
struct ComposeInput
{
    // One entry per project filament (the arrays are that long, unused filaments included).
    size_t                   project_filament_count { 0 };
    // The filament preset id of each project filament ("GFA00"), "" when unknown.
    std::vector<std::string> filament_ids;
    // 1-based extruder per project filament, for "nozzleId"; empty on a one-nozzle printer, which
    // then gets no nozzleId at all (its payload stays exactly what it always was).
    std::vector<int>         nozzle_filament_map;
};

// SelectMachineDialog::get_ams_mapping_result: the v0 tray list, the v1 [{ams_id, slot_id}] list and
// the per-filament info PrintJob forwards. `filaments` are the job's filaments in the order the
// result was made from (the dialog's m_filaments). Returns false and leaves the strings alone when
// the result is empty or maps nothing; otherwise fills all three and returns whether every
// filament got a tray.
bool compose(const std::vector<FilamentInfo>& result, const std::vector<FilamentInfo>& filaments, const ComposeInput& in,
             std::string& v0, std::string& v1, std::string& info);

// SelectMachineDialog::build_nozzles_info: [{id, type:null, flowSize, diameter}] for a two-nozzle
// printer (id 1 = left, 0 = right), "[]" for anything else. `volume_types` are NozzleVolumeType
// values per nozzle (missing = standard flow).
std::string nozzles_info(const std::vector<double>& diameters, const std::vector<int>& volume_types);

} // namespace BambuSendMapping
} // namespace GUI
} // namespace Slic3r
