#include "RemoteSend.hpp"

#include "BambuReprint.hpp"
#include "BambuSendMapping.hpp"
#include "BitmapCache.hpp"
#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "I18N.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "RemoteAccess.hpp"
#include "SelectMachine.hpp" // CloudTaskNozzleId
#include "SpoolmanDialog.hpp" // deduct_after_send_async
#include "Jobs/PrintJob.hpp" // PrintPrepareData
#include "libslic3r/AppConfig.hpp" // DeviceInfo
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ProjectTask.hpp" // FilamentInfo
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/MoonRaker.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/PrintHostDevices.hpp"
#include "slic3r/Utils/Spoolman.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <future>
#include <mutex>
#include <thread>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace RemoteSend {

namespace fs = boost::filesystem;
using nlohmann::json;

// ---------------------------------------------------------------- helpers ----

// From the worker thread: run fn on the GUI thread and wait for it (bounded).
static bool on_main(std::function<void()> fn, int timeout_ms = 10000)
{
    // Through the gate (MainThreadGate.hpp): once the main window is closing this is false at once
    // and fn never runs against a Plater that is going away.
    return RemoteAccess::call_on_main(std::move(fn), timeout_ms) == MainCallResult::Done;
}

static bool env_flag(const char* name)
{
    wxString v;
    return wxGetEnv(name, &v) && v == "1";
}

// The send dialog remembers its checkboxes in AppConfig section "print": unset or "1" means on.
static bool remembered(const char* key)
{
    AppConfig* cfg = wxGetApp().app_config;
    return !(cfg && cfg->get("print", key) == "0");
}

static bool flag(int requested, bool def) { return requested < 0 ? def : requested != 0; }

// PrintJob::truncate_string: at most `max_bytes` of UTF-8 without cutting a character in half.
static std::string truncate_utf8(std::string s, size_t max_bytes)
{
    if (s.size() <= max_bytes) return s;
    size_t n = max_bytes;
    while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

static std::string drop_characters(std::string s, const char* bad)
{
    std::string out;
    for (char c : s)
        if (!std::strchr(bad, c)) out += c;
    return out;
}

static MachineObject* find_machine(DeviceManager* dm, const std::string& id)
{
    std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
    for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
    for (const auto& kv : all)
        if (kv.second && kv.second->dev_id == id) return kv.second;
    return nullptr;
}

// SelectMachineDialog::is_same_printer_model without the dialog.
static bool same_printer_model(MachineObject* obj, std::string* profile_model)
{
    PresetBundle*     bundle = wxGetApp().preset_bundle;
    const std::string source = bundle->printers.get_edited_preset().get_printer_type(bundle);
    const std::string target = obj->printer_type;
    if (profile_model) *profile_model = source;
    const bool p1p_with_kit = target == "C11" && obj->is_support_upgrade_kit && obj->installed_upgrade_kit;
    if (source != target)
        return (source == "C12" && target == "C11") || (source == "C11" && target == "C12") || (p1p_with_kit && source == "C12");
    return !p1p_with_kit;
}

// The name the desktop shows in its send dialog (SelectMachineDialog::set_default): the export
// name of the plate, or the plate's object names when the project is untitled.
static std::string print_name_for(Plater* plater, PartPlate* plate)
{
    wxString filename = plater->get_export_gcode_filename("", true);
    if (filename.empty()) filename = "Untitled";
    std::string name = fs::path(filename.ToUTF8().data()).filename().string();
    if (name.find("Untitled") != std::string::npos) {
        const ModelObjectPtrs objects = plate->get_objects_on_this_plate();
        if (!objects.empty()) {
            name = objects[0]->name;
            for (size_t i = 1; i < objects.size(); ++i) name += " + " + objects[i]->name;
            if (name.size() > 100) name = name.substr(0, 97) + "...";
        }
    }
    return drop_characters(name, "<>[]:/\\|?*\"");
}

// SelectMachineDialog::reset_and_sync_ams_list + set_default_normal: the plate's filaments as the
// AMS mapping wants them (type, brand, id, colour per used extruder).
static std::vector<FilamentInfo> plate_filaments(PartPlate* plate)
{
    PresetBundle*            bundle = wxGetApp().preset_bundle;
    std::vector<std::string> types, brands, ids;
    for (const std::string& name : bundle->filament_presets) {
        Preset*     preset = bundle->filaments.find_preset(name, false);
        std::string display, type, id, brand;
        if (preset) {
            type = preset->config.get_filament_type(display);
            id   = preset->filament_id;
            if (auto* v = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_vendor")); v && !v->values.empty())
                brand = v->values[0];
        }
        types.push_back(type);
        ids.push_back(id);
        brands.push_back(brand);
    }
    std::vector<FilamentInfo> out;
    for (int e : plate->get_used_extruders()) {
        const int extruder = e - 1;
        if (extruder < 0 || extruder >= (int) types.size()) continue;
        const std::string colour = bundle->project_config.opt_string("filament_colour", (unsigned int) extruder);
        unsigned char     rgb[4] = { 0, 0, 0, 255 };
        BitmapCache::parse_color4(colour, rgb);
        char buf[16];
        std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", rgb[0], rgb[1], rgb[2], rgb[3]);
        FilamentInfo info;
        info.id          = extruder;
        info.type        = types[extruder];
        info.brand       = brands[extruder];
        info.filament_id = ids[extruder];
        info.color       = buf;
        info.used_m = info.used_g = 0.f;
        info.tray_id     = -1;
        info.distance    = 0.f;
        out.push_back(info);
    }
    return out;
}

// SelectMachineDialog::sliced_filament_map: the 1-based extruder per filament the plate's G-code
// was generated with (the plate's own Print config, the project's filament_map as the fallback).
// Empty unless the printer preset really has two nozzles, which is what keeps a single-nozzle
// payload unchanged.
static std::vector<int> plate_filament_map()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    const auto*   diam   = bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (!diam || diam->size() != 2) return {};
    std::vector<int> fm = wxGetApp().plater()->get_partplate_list().get_current_fff_print().config().filament_map.values;
    if (fm.size() < bundle->filament_presets.size())
        if (const auto* pfm = bundle->project_config.option<ConfigOptionInts>("filament_map")) fm = pfm->values;
    return fm;
}

static std::vector<int> preset_physical_extruder_map()
{
    if (const auto* pem = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionInts>("physical_extruder_map"))
        return pem->values;
    return {};
}

// SelectMachineDialog::do_ams_mapping + get_ams_mapping_result, through the code they share
// (BambuSendMapping): the three JSON strings PrintJob forwards (v0 tray list, v1 ams/slot list,
// per-filament info). A two-nozzle plate maps each side against its own AMS units. Empty when
// nothing maps.
static void ams_mapping(MachineObject* obj, const std::vector<FilamentInfo>& filaments, std::string& v0, std::string& v1, std::string& info)
{
    const std::vector<int>    fil_map = plate_filament_map();
    std::vector<FilamentInfo> result;
    const int                 rc = BambuSendMapping::auto_map(obj, filaments, fil_map, preset_physical_extruder_map(), result);
    if (rc != 0 && rc != 1 && !obj->is_valid_mapping_result(result))
        for (FilamentInfo& r : result) { r.tray_id = -1; r.distance = 99999; }

    PresetBundle*                  bundle = wxGetApp().preset_bundle;
    BambuSendMapping::ComposeInput in;
    in.project_filament_count = bundle->filament_presets.size();
    for (const std::string& name : bundle->filament_presets) {
        const Preset* pr = bundle->filaments.find_preset(name);
        in.filament_ids.push_back(pr ? pr->filament_id : std::string());
    }
    in.nozzle_filament_map = fil_map;
    BambuSendMapping::compose(result, filaments, in, v0, v1, info);
}

// SelectMachineDialog::build_nozzles_info: only the two-nozzle printers carry this. Per-nozzle
// flow variant from the project config: an H2C's high-flow nozzle is not "standard_flow".
static std::string nozzles_info()
{
    const auto* diam = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    if (!diam) return "[]";
    std::vector<int> volume_types;
    if (const auto* vol = wxGetApp().preset_bundle->project_config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
        for (size_t i = 0; i < vol->size(); ++i) volume_types.push_back(vol->get_at(i));
    return BambuSendMapping::nozzles_info(diam->values, volume_types);
}

static json params_json(const BBL::PrintParams& p)
{
    json j;
    j["dev_id"]           = p.dev_id;
    j["dev_ip"]           = p.dev_ip;
    j["project_name"]     = p.project_name;
    j["preset_name"]      = p.preset_name;
    j["filename"]         = p.filename;
    j["config_filename"]  = p.config_filename;
    j["plate_index"]      = p.plate_index;
    j["ftp_folder"]       = p.ftp_folder;
    j["ams_mapping"]      = p.ams_mapping;
    j["ams_mapping2"]     = p.ams_mapping2;
    j["ams_mapping_info"] = p.ams_mapping_info;
    j["nozzles_info"]     = p.nozzles_info;
    j["connection_type"]  = p.connection_type;
    j["print_type"]       = p.print_type;
    j["use_ssl_for_ftp"]  = p.use_ssl_for_ftp;
    j["use_ssl_for_mqtt"] = p.use_ssl_for_mqtt;
    j["username"]         = p.username;
    j["password_set"]     = !p.password.empty(); // the access code itself never leaves the PC
    j["task_bed_leveling"]     = p.task_bed_leveling;
    j["task_flow_cali"]        = p.task_flow_cali;
    j["task_vibration_cali"]   = p.task_vibration_cali;
    j["task_layer_inspect"]    = p.task_layer_inspect;
    j["task_record_timelapse"] = p.task_record_timelapse;
    j["task_use_ams"]          = p.task_use_ams;
    j["task_bed_type"]         = p.task_bed_type;
    j["auto_offset_cali"]      = p.auto_offset_cali;
    j["extruder_cali_manual_mode"] = p.extruder_cali_manual_mode;
    j["nozzle_mapping_request"]    = p.nozzle_mapping_request;
    j["dst_file"]              = p.dst_file;
    return j;
}

// PrintJob / SendJob: the stage the plugin reports -> the percentage the desktop shows.
static int stage_percent(int stage, int code, bool upload_only)
{
    static const int print_pts[7] = { 20, 30, 70, 75, 97, 100, 100 };
    static const int send_pts[7]  = { 20, 30, 99, 99, 99, 100, 100 };
    const int*       pts          = upload_only ? send_pts : print_pts;
    if (stage < 0) return -1;
    const int s   = std::min(stage, 6);
    int       pct = pts[s];
    if ((stage == BBL::PrintingStageUpload || stage == BBL::PrintingStageRecord) && code > 0 && code <= 100 && s < 6)
        pct = (pts[s + 1] - pts[s]) * code / 100 + pts[s];
    return pct;
}

static const char* stage_name(int stage)
{
    switch (stage) {
    case BBL::PrintingStageCreate:      return "preparing";
    case BBL::PrintingStageUpload:      return "uploading";
    case BBL::PrintingStageWaiting:     return "waiting for the printer";
    case BBL::PrintingStageSending:     return "sending the print command";
    case BBL::PrintingStageRecord:      return "sending the print configuration";
    case BBL::PrintingStageWaitPrinter: return "waiting for the printer";
    case BBL::PrintingStageFinished:    return "sent";
    default:                            return "working";
    }
}

// PrintJob::process: the desktop's words for the plugin's result codes.
static std::string result_text(int result)
{
    switch (result) {
    case BAMBU_NETWORK_ERR_PRINT_WR_FILE_NOT_EXIST:
    case BAMBU_NETWORK_ERR_PRINT_SP_FILE_NOT_EXIST:            return "Print file not found. Please slice again.";
    case BAMBU_NETWORK_ERR_PRINT_SP_FILE_OVER_SIZE:
    case BAMBU_NETWORK_ERR_PRINT_WR_FILE_OVER_SIZE:            return "The print file exceeds the maximum allowable size (1GB).";
    case BAMBU_NETWORK_ERR_PRINT_WR_CHECK_MD5_FAILED:
    case BAMBU_NETWORK_ERR_PRINT_SP_CHECK_MD5_FAILED:          return "Cloud service connection failed. Please try again.";
    case BAMBU_NETWORK_ERR_PRINT_WR_GET_NOTIFICATION_TIMEOUT:
    case BAMBU_NETWORK_ERR_PRINT_SP_GET_NOTIFICATION_TIMEOUT:  return "Upload task timed out. Please check the network status and try again.";
    case BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED:
    case BAMBU_NETWORK_ERR_PRINT_SG_UPLOAD_FTP_FAILED:         return "Failed to upload file to ftp. Please try again.";
    case BAMBU_NETWORK_ERR_CANCELED:                           return "Task canceled.";
    default:                                                   return "Failed to send the print job (code " + std::to_string(result) + ")";
    }
}

// ---------------------------------------------------------------- prepare ----

static std::pair<int, std::string> prepare_bambu(const Request& req, PartPlate* plate, std::shared_ptr<Prepared> p, std::shared_ptr<Prepared>& out)
{
    Plater*        plater = wxGetApp().plater();
    DeviceManager* dm     = wxGetApp().getDeviceManager();
    if (!dm) return { 503, "no device manager" };
    if (!wxGetApp().getAgent()) return { 503, "the network plugin is not loaded" };
    MachineObject* obj = find_machine(dm, req.printer);
    if (!obj) return { 404, "no such printer: " + req.printer };
    if (!obj->is_online()) return { 409, obj->dev_name + " is offline" };
    if (obj->is_lan_mode_printer() && !obj->has_access_right()) return { 409, obj->dev_name + " needs its access code entered on the PC first" };
    if (obj->is_in_printing()) return { 409, obj->dev_name + " is busy printing" };
    std::string profile_model;
    if (!same_printer_model(obj, &profile_model) && !req.force)
        return { 409, "the plate was sliced for printer model " + profile_model + " but " + obj->dev_name + " reports " + obj->printer_type +
                          "; send with force=1 to ignore" };

    // preselect() made this the selected (connected) printer; a late caller still gets that here.
    if (!dm->get_selected_machine() || dm->get_selected_machine()->dev_id != obj->dev_id)
        dm->set_selected_machine(obj->dev_id, true);

    // The files the plugin uploads: the gcode 3mf, plus the configuration 3mf for cloud printers.
    if (plater->send_gcode(req.plate, nullptr) < 0) return { 500, "Abnormal print file data. Please slice again" };
    if (!obj->is_lan_mode_printer() && plater->export_config_3mf(req.plate) < 0) return { 500, "exporting the configuration 3mf failed" };
    PrintPrepareData jd;
    plater->get_print_job_data(&jd);

    p->kind               = "bambu";
    p->printer_name       = obj->dev_name;
    p->print_error_before = obj->print_error;
    BBL::PrintParams& ps  = p->params;
    ps.dev_id     = obj->dev_id;
    ps.dev_ip     = obj->dev_ip;
    ps.ftp_folder = obj->get_ftp_folder();
    ps.username   = "bblp";
    ps.password   = obj->get_access_code();
#if !BBL_RELEASE_TO_PUBLIC
    ps.use_ssl_for_ftp  = wxGetApp().app_config->get("enable_ssl_for_ftp") == "true";
    ps.use_ssl_for_mqtt = wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true";
#else
    ps.use_ssl_for_ftp  = obj->local_use_ssl_for_ftp;
    ps.use_ssl_for_mqtt = obj->local_use_ssl_for_mqtt;
#endif
    ps.connection_type = obj->connection_type();
    ps.filename        = jd._3mf_path.string();
    ps.config_filename = jd._3mf_config_path.string();
    ps.plate_index     = req.plate + 1;
    const bool        has_sdcard = obj->get_sdcard_state() == MachineObject::SdcardState::HAS_SDCARD_NORMAL;
    const std::string name       = print_name_for(plater, plate);

    if (req.mode == "print") {
        // SelectMachineDialog::on_send_print + PrintJob::process
        ps.print_type            = "from_normal";
        ps.project_name          = truncate_utf8(name, 100);
        ps.preset_name           = name + "_plate_" + std::to_string(req.plate + 1);
        ps.task_bed_type         = bed_type_to_gcode_string(plate->get_bed_type(true));
        ps.task_bed_leveling     = flag(req.bed_leveling, remembered("bed_leveling"));
        ps.task_flow_cali        = flag(req.flow_cali, remembered("flow_cali"));
        ps.task_vibration_cali   = flag(req.vibration_cali, false); // the desktop passes false here
        ps.task_record_timelapse = flag(req.timelapse, remembered("timelapse"));
        ps.task_layer_inspect    = true;
        ps.task_use_ams          = obj->has_ams() && flag(req.use_ams, true);
        ps.nozzles_info          = nozzles_info();
        // Nozzle offset calibration, the tri-state the H2 series grew: 0 = off, 1 = on, 2 = auto.
        // Only a two-nozzle machine has it - nozzles_info is non-empty for exactly those - and it
        // follows the same [print] AppConfig key the send dialog's checkbox writes, so a phone
        // send honours what the desktop was last told. Zero elsewhere leaves those payloads alone.
        if (!ps.nozzles_info.empty() && ps.nozzles_info != "[]")
            ps.auto_offset_cali = remembered("nozzle_offset_cali") ? 2 : 0;
        if (obj->is_support_ams_mapping() && ps.task_use_ams) {
            ams_mapping(obj, plate_filaments(plate), ps.ams_mapping, ps.ams_mapping2, ps.ams_mapping_info);
        } else if (!ps.task_use_ams) {
            const std::vector<FilamentInfo> fils = plate_filaments(plate);
            if (!fils.empty()) {
                json a = json::array(), it;
                it["sourceColor"]  = fils[0].color.size() >= 9 ? fils[0].color.substr(1, 8) : fils[0].color;
                it["filamentType"] = fils[0].type;
                a.push_back(it);
                ps.ams_mapping_info = a.dump();
            }
        }
        if (ps.connection_type == "lan") {
            if (!has_sdcard) return { 409, "An SD card needs to be inserted before printing via LAN." };
            p->call               = "start_local_print";
            p->verify_access_code = true;
        } else {
            const bool lan_only  = wxGetApp().app_config->get("lan_mode_only") == "1";
            const bool can_local = !ps.password.empty() && !ps.dev_ip.empty() && has_sdcard;
            if (lan_only) {
                if (!can_local) return { 409, "LAN-only mode is on but the printer has no IP address, access code or SD card" };
                p->call = "start_local_print_with_record";
            } else if (!obj->is_support_cloud_print_only && can_local) {
                p->call                  = "start_local_print_with_record";
                p->lan_fallback_to_cloud = true;
            } else {
                p->call = "start_print";
            }
        }
    } else {
        // SendToPrinterDialog::on_ok_btn + SendJob::process: upload to the printer's storage only
        ps.project_name = name + ".gcode.3mf";
        ps.preset_name  = wxGetApp().preset_bundle->prints.get_selected_preset_name();
        ps.task_use_ams = true;
        if (ps.connection_type == "lan") {
            if (!has_sdcard) return { 409, "An SD card needs to be inserted before sending to printer." };
        } else if (ps.password.empty() || ps.dev_ip.empty() || !has_sdcard) {
            return { 409, "uploading needs the printer's IP address, its access code and an SD card" };
        }
        p->call = "start_send_gcode_to_sdcard";
    }
    out = p;
    return { 200, "" };
}

// The sliced file's filaments, the way the Device page's own sw_GetFileFilamentMapping reads them:
// the project's colours and types, and which slots this plate's slice result actually used.
static std::vector<SnapmakerLan::FileFilament> file_filaments_of(PartPlate* plate)
{
    std::vector<SnapmakerLan::FileFilament> out;
    PresetBundle*                           bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return out;
    const DynamicPrintConfig   full    = bundle->full_config();
    const ConfigOptionStrings* colours = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    const ConfigOptionStrings* types   = full.option<ConfigOptionStrings>("filament_type");
    const ConfigOptionFloats*  density = full.option<ConfigOptionFloats>("filament_density");
    std::map<size_t, double>   volumes;
    if (plate && plate->is_slice_result_valid() && plate->get_slice_result())
        for (const auto& kv : plate->get_slice_result()->print_statistics.total_volumes_per_extruder)
            volumes[kv.first] = kv.second;
    for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
        SnapmakerLan::FileFilament f;
        f.index = (int) i;
        f.color = (colours && i < colours->values.size()) ? colours->values[i] : "";
        f.type  = (types && i < types->values.size()) ? types->values[i] : "";
        auto it = volumes.find(i);
        f.used  = it != volumes.end() && it->second > 0;
        if (f.used) {
            const double dens = (density && i < density->values.size()) ? density->values[i] : 1.24;
            f.used_g          = it->second / 1000.0 * dens;
        }
        out.push_back(f);
    }
    return out;
}

// "0:1,1:2" - the file's filament 0 prints on toolhead 1, filament 1 on toolhead 2.
static bool parse_mapping(const std::string& text, size_t filaments, std::vector<int>& out, std::string& error)
{
    out.assign(filaments, -1);
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t      comma = text.find(',', pos);
        const std::string item  = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos                     = comma == std::string::npos ? text.size() : comma + 1;
        if (item.empty())
            continue;
        const size_t colon = item.find(':');
        if (colon == std::string::npos) {
            error = "mapping wants <filament>:<toolhead> pairs, not '" + item + "'";
            return false;
        }
        int filament = -1, head = -1;
        try {
            filament = std::stoi(item.substr(0, colon));
            head     = std::stoi(item.substr(colon + 1));
        } catch (...) {
            error = "mapping wants numbers, not '" + item + "'";
            return false;
        }
        if (filament < 0 || filament >= (int) filaments) {
            error = "this file has no filament " + std::to_string(filament);
            return false;
        }
        if (head < 0 || head > 15) {
            error = "toolhead " + std::to_string(head) + " is out of range";
            return false;
        }
        out[filament] = head;
    }
    return true;
}

// A Snapmaker on the LAN: no connect, no host object, just its address (SnapmakerLan).
static std::pair<int, std::string> prepare_snapmaker(const Request& req, PartPlate* plate, std::shared_ptr<Prepared> p,
                                                     std::shared_ptr<Prepared>& out)
{
    Plater* plater = wxGetApp().plater();
    SnapmakerLan::Device d;
    if (!SnapmakerLan::find(req.printer.substr(3), d))
        return { 404, "no such printer: " + req.printer };
    const SnapmakerLan::Status st = SnapmakerLan::status(d);
    if (!st.online)
        return { 409, d.name + " is not answering on the network" };
    if (st.login_required)
        return { 409, d.name + " asks for a login; the phone can only reach a printer that does not" };
    if (st.printing())
        return { 409, d.name + " is " + (st.state == "paused" ? "paused mid-print" : "printing") + " (" + st.filename + ")" };
    p->kind         = "snapmaker";
    p->printer_name = d.name.empty() ? d.ip : d.name;
    p->lan          = d;
    p->toolheads    = SnapmakerLan::toolheads(d);
    // The file: the plate's sliced G-code, exactly what the desktop's Snapmaker path uploads.
    fs::path source = fs::path(plate->get_tmp_gcode_path());
    boost::system::error_code ec;
    if (!fs::is_regular_file(source, ec))
        return { 409, "this plate has no G-code file; slice it again" };
    std::string name = req.name.empty() ? std::string(plater->get_export_gcode_filename(".gcode", true).ToUTF8().data()) : req.name;
    name             = fs::path(name).filename().string(); // never a directory from the phone
    if (name.empty() || name == "." || name == "..")
        name = "plate_" + std::to_string(req.plate + 1) + ".gcode";
    if (!boost::iends_with(name, ".gcode"))
        name += ".gcode";
    p->upload.source_path = source;
    p->upload.upload_path = fs::path(name);
    p->lan_filename       = name;
    p->file_filaments     = file_filaments_of(plate);
    // The end-of-print unload the printer will do for us, if this preset asks for it. Read here,
    // on the GUI thread, with the same preset the plate was sliced against.
    if (PresetBundle* bundle = wxGetApp().preset_bundle) {
        const DynamicPrintConfig& pcfg = bundle->printers.get_edited_preset().config;
        const ConfigOptionBool* unload = pcfg.option<ConfigOptionBool>("unload_filaments_at_end");
        p->unload_supported = is_snapmaker_toolchanger(pcfg);
        p->unload_at_end    = unload != nullptr && unload->value && p->unload_supported;
        // The preset is only the default. The send sheet - like the desktop's send dialog - can
        // say otherwise for this print, in either direction, as long as the printer is one that
        // can do it at all.
        if (!req.unload_at_end.empty() && p->unload_supported)
            p->unload_at_end = req.unload_at_end == "1" || req.unload_at_end == "true";
    }
    if (req.mode == "print") {
        std::string error;
        if (req.mapping.empty())
            p->mapping = SnapmakerLan::auto_match(p->file_filaments, p->toolheads);
        else if (!parse_mapping(req.mapping, p->file_filaments.size(), p->mapping, error))
            return { 400, error };
        for (const SnapmakerLan::FileFilament& f : p->file_filaments) {
            if (!f.used || (size_t) f.index >= p->mapping.size())
                continue;
            if (p->mapping[f.index] < 0)
                return { 400, "filament " + std::to_string(f.index + 1) + " has no toolhead; every filament the file uses needs one" };
            if (!p->toolheads.empty() && p->mapping[f.index] >= (int) p->toolheads.size())
                return { 400, p->printer_name + " has " + std::to_string(p->toolheads.size()) + " toolheads, so there is no toolhead " +
                                  std::to_string(p->mapping[f.index] + 1) };
        }
        // A toolhead with nothing in it cannot print: say so before the file goes up.
        if (!p->toolheads.empty() && !req.force)
            for (const SnapmakerLan::FileFilament& f : p->file_filaments) {
                if (!f.used || (size_t) f.index >= p->mapping.size())
                    continue;
                const int h = p->mapping[f.index];
                if (h >= 0 && h < (int) p->toolheads.size() && !p->toolheads[h].loaded)
                    return { 409, "toolhead " + std::to_string(h + 1) + " is empty; load it, pick another, or send force=1" };
            }
    }
    out = p;
    return { 200, "" };
}

static std::pair<int, std::string> prepare_host(const Request& req, PartPlate* plate, std::shared_ptr<Prepared> p, std::shared_ptr<Prepared>& out)
{
    Plater*             plater = wxGetApp().plater();
    PresetBundle*       bundle = wxGetApp().preset_bundle;
    DynamicPrintConfig& cfg    = bundle->printers.get_edited_preset().config;
    std::shared_ptr<PrintHost> host;
    if (req.printer == "connect") {
        wxGetApp().get_connect_host(host);
        if (!host) return { 409, "no Snapmaker printer is connected on the PC's Device tab" };
        p->kind         = "connect";
        p->printer_name = "Snapmaker " + host->get_host();
    } else if (req.printer.compare(0, 3, "ph:") == 0) {
        // One of this model's devices, by id. The address and credentials come from the device,
        // not from the preset - the preset may have no print_host at all.
        if (bundle->use_bbl_network()) return { 409, "the current printer preset sends through the Bambu network; pick that printer by its id" };
        const std::string        model_key = PrintHostDevices::current_model_key(*bundle);
        PrintHostDevices::Device d;
        if (!PrintHostDevices::find(model_key, req.printer.substr(3), d))
            return { 404, "no such device for this printer model: " + req.printer };
        if (d.address.empty()) return { 409, d.display_name() + " has no address" };
        DynamicPrintConfig dev_cfg = PrintHostDevices::config_for(d, cfg);
        host.reset(PrintHost::get_print_host(&dev_cfg, false));
        if (!host) return { 500, "unsupported host type" };
        p->kind         = "printhost";
        p->printer_name = d.display_name();
        p->device_id    = d.id;
        p->model_key    = model_key;
        // Where the last plate went, so the desktop send dialog and the Device tab preselect the
        // printer the phone just used. Recorded here rather than after the upload, exactly as the
        // desktop records it when its dialog is confirmed: it is the choice, not the outcome.
        try {
            PrintHostDevices::set_last_used(model_key, d.id);
        } catch (...) {}
    } else {
        if (bundle->use_bbl_network()) return { 409, "the current printer preset sends through the Bambu network; pick that printer by its id" };
        const std::string url = cfg.opt_string("print_host");
        if (url.empty()) return { 409, "the printer preset has no print host address" };
        host.reset(PrintHost::get_print_host(&cfg, false));
        if (!host) return { 500, "unsupported host type" };
        p->kind         = "printhost";
        p->printer_name = std::string(host->get_name()) + " " + url;
    }
    const bool want_print = req.mode == "print";
    if (want_print && !host->get_post_upload_actions().has(PrintHostPostUploadAction::StartPrint))
        return { 409, std::string(host->get_name()) + " cannot start a print after the upload" };
    // Moonraker::upload with StartPrint stops at the PC's preprint page (WebPreprintDialog). Upload
    // first, then start the file over the MQTT channel like the page's own Print button does.
    if (want_print && dynamic_cast<Moonraker*>(host.get()) != nullptr) {
        if (dynamic_cast<Moonraker_Mqtt*>(host.get()) == nullptr) return { 409, "this Moonraker host can only start prints from the PC's preprint page" };
        p->two_step = true;
    }
    // The file: the plate's sliced G-code (what the Snapmaker path uploads), or the gcode 3mf when a
    // Bambu printer sits behind a print host (Plater::priv::on_action_print_plate, use_3mf).
    const bool use_3mf = bundle->is_bbl_vendor();
    fs::path   source;
    if (use_3mf) {
        if (plater->send_gcode(req.plate, nullptr) < 0) return { 500, "Abnormal print file data. Please slice again" };
        PrintPrepareData jd;
        plater->get_print_job_data(&jd);
        source = jd._3mf_path;
    } else {
        source = fs::path(plate->get_tmp_gcode_path());
        boost::system::error_code ec;
        if (!fs::is_regular_file(source, ec)) return { 409, "this plate has no G-code file; slice it again" };
    }
    const char* ext  = use_3mf ? ".gcode.3mf" : ".gcode";
    std::string name = req.name;
    if (name.empty()) name = plater->get_export_gcode_filename(ext, true).ToUTF8().data();
    name = fs::path(name).filename().string(); // never a directory from the phone
    if (name.empty() || name == "." || name == "..") name = "plate_" + std::to_string(req.plate + 1) + ext;
    p->host                = host;
    p->upload.use_3mf      = use_3mf;
    p->upload.source_path  = source;
    p->upload.upload_path  = fs::path(name);
    p->upload.post_action  = (want_print && !p->two_step) ? PrintHostPostUploadAction::StartPrint : PrintHostPostUploadAction::None;
    out                    = p;
    return { 200, "" };
}

std::pair<int, std::string> preselect(const Request& req, bool& wait)
{
    wait = false;
    if (req.printer.empty()) return { 400, "printer is required" };
    if (req.printer == "host" || req.printer == "connect" || req.printer.compare(0, 3, "sm:") == 0 ||
        req.printer.compare(0, 3, "ph:") == 0)
        return { 200, "" };
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return { 503, "no device manager" };
    MachineObject* obj = find_machine(dm, req.printer);
    if (!obj) return { 404, "no such printer: " + req.printer };
    if (!obj->is_online()) return { 409, obj->dev_name + " is offline" };
    if (obj->is_lan_mode_printer() && !obj->has_access_right()) return { 409, obj->dev_name + " needs its access code entered on the PC first" };
    // SelectMachineDialog::on_selection_changed: the picked printer becomes the selected one, which
    // connects to it; its SD card / printing state arrives with the first push afterwards.
    MachineObject* selected = dm->get_selected_machine();
    if (!selected || selected->dev_id != obj->dev_id) {
        dm->set_selected_machine(obj->dev_id, true);
        wait = true;
    } else if (!obj->is_connected()) {
        wait = true;
    }
    return { 200, "" };
}

bool printer_ready(const std::string& printer)
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return true;
    MachineObject* obj = find_machine(dm, printer);
    if (!obj) return true;
    return obj->is_connected() && obj->get_sdcard_state() != MachineObject::SdcardState::NO_SDCARD;
}

std::pair<int, std::string> prepare(const Request& req, std::shared_ptr<Prepared>& out)
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return { 503, "no plater" };
    if (req.mode != "upload" && req.mode != "print") return { 400, "mode must be upload or print" };
    if (req.mode == "print" && !req.confirm) return { 400, "starting a print needs confirm=1" };
    if (req.printer.empty()) return { 400, "printer is required" };
    PartPlateList& plates = plater->get_partplate_list();
    if (req.plate < 0 || req.plate >= plates.get_plate_count()) return { 404, "no such plate" };
    if (plater->is_background_process_slicing()) return { 409, "the slicer is slicing" };
    PartPlate* plate = plates.get_plate(req.plate);
    if (!plate->is_slice_result_valid() || plate->get_slice_result() == nullptr || !plate->is_slice_result_ready_for_print())
        return { 409, "this plate is not sliced, or its result is not printable" };
    // The export helpers and the file name work on the current plate, exactly as the dialogs do.
    if (plates.get_curr_plate_index() != req.plate) plater->select_plate(req.plate, false);

    auto p        = std::make_shared<Prepared>();
    p->mode       = req.mode;
    p->plate      = req.plate;
    p->dry_run    = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
    p->printer_id = req.printer;
    std::pair<int, std::string> rc;
    if (req.printer.compare(0, 3, "sm:") == 0)                  rc = prepare_snapmaker(req, plate, p, out);
    else if (req.printer == "host" || req.printer == "connect"
             || req.printer.compare(0, 3, "ph:") == 0)          rc = prepare_host(req, plate, p, out);
    else                                                        rc = prepare_bambu(req, plate, p, out);
    // Ultra: everything the G-code archive wants about this plate, read here on the GUI thread; the
    // file name and the printer identity are what the branch above worked out. Never fails a send.
    if (rc.first == 200 && out) {
        try {
            GcodeArchive::Meta m = GcodeArchive::meta_for_plate(req.plate, req.mode);
            m.printer_id   = p->printer_id;
            m.printer_kind = p->kind;
            m.printer_name = p->printer_name;
            m.source       = "phone";
            if (p->kind == "snapmaker") {
                m.file_name = p->lan_filename;
                // The wire form /api/plates/{i}/send takes, so a reprint replays it as a copy.
                std::string wire;
                for (size_t i = 0; i < p->mapping.size(); ++i) {
                    if (p->mapping[i] < 0) continue;
                    if (!wire.empty()) wire += ",";
                    wire += std::to_string((int) i) + ":" + std::to_string(p->mapping[i]);
                }
                m.mapping = wire;
                m.unload_at_end = p->unload_at_end;
            } else if (p->kind == "bambu") {
                // The plugin uploads the gcode 3mf; project_name is the print's name, with the
                // extension only in the upload branch.
                m.file_name = p->params.project_name;
                if (!boost::iends_with(m.file_name, ".3mf")) m.file_name += ".gcode.3mf";
            } else {
                m.file_name = p->upload.upload_path.string();
            }
            // Stage 1d: a phone send should deduct filament in Spoolman exactly as a desktop one
            // does. The desktop calls SpoolmanDialog::deduct_after_send_async() from
            // Plater::print_job_finished and SSWCP's print start; the print-host and Snapmaker-LAN
            // phone paths never had a call site, so the preference was silently half true. The gate
            // is read here, on the GUI thread, because Spoolman::base_url() and the preference both
            // read AppConfig - run() only fires it, and only when the send actually worked.
            if (p->kind == "printhost" || p->kind == "snapmaker") {
                AppConfig* cfg      = wxGetApp().app_config;
                p->spoolman_deduct  = Spoolman::enabled() && cfg && cfg->get("spoolman_deduct") == "true";
                m.spoolman_deduct   = p->spoolman_deduct;
            }
            p->archive_meta = std::move(m);
        } catch (...) {}
    }
    return rc;
}

// ------------------------------------------------- stage 2: send an archived file ----

// The record's filaments, back in the shape a Snapmaker send wants. The sidecar only lists the
// slots the plate actually used, each with its real index, so the gaps between them are filled in
// as unused - the mapping is indexed by the file's filament number, not by position in the list.
static std::vector<SnapmakerLan::FileFilament> file_filaments_of_record(const json& j)
{
    std::vector<SnapmakerLan::FileFilament> out;
    if (!j.contains("filaments") || !j["filaments"].is_array())
        return out;
    int highest = -1;
    for (const json& f : j["filaments"])
        highest = std::max(highest, f.is_object() ? f.value("index", -1) : -1);
    for (int i = 0; i <= highest; ++i) {
        SnapmakerLan::FileFilament f;
        f.index = i;
        f.used  = false;
        out.push_back(f);
    }
    for (const json& f : j["filaments"]) {
        if (!f.is_object()) continue;
        const int i = f.value("index", -1);
        if (i < 0 || i >= (int) out.size()) continue;
        out[i].color  = f.value("colour", std::string());
        out[i].type   = f.value("type", std::string());
        out[i].used_g = f.value("grams", 0.0);
        out[i].used   = true;
    }
    return out;
}

// Which of the four send kinds a printer id names, without asking anything. "ph:<device>" - one of
// a printer model's print-host devices (Moonraker, PrusaLink, OctoPrint, FlashForge, Elegoo...) -
// is a print host like the preset's own "host"; it used to fall through to "bambu", so every
// record sent to such a device was refused a reprint as "sent to a printhost printer and ph:... is
// a bambu one".
std::string printer_kind_of(const std::string& id)
{
    if (id.compare(0, 3, "sm:") == 0) return "snapmaker";
    if (id == "host")                 return "printhost";
    if (id.compare(0, 3, "ph:") == 0) return "printhost";
    if (id == "connect")              return "connect";
    return "bambu";
}

static std::string kind_of_printer(const std::string& id) { return printer_kind_of(id); }

// ------------------------------------------------- a Bambu reprint (BambuReprint) ----

// The name the print is given: the file's name without its extension, as the desktop's send
// dialog names a plate after its export name (print_name_for).
static std::string print_name_of_record(const GcodeArchive::Record& rec)
{
    std::string name = rec.json.value("sent_name", std::string());
    if (name.empty()) name = rec.file;
    name = fs::path(name).filename().string();
    for (const char* ext : { ".gcode.3mf", ".3mf" })
        if (boost::iends_with(name, ext)) { name.resize(name.size() - std::strlen(ext)); break; }
    name = drop_characters(name, "<>[]:/\\|?*\"");
    return name.empty() ? std::string("reprint") : name;
}

// The printer's answer to the desktop's own get_auto_nozzle_mapping query (SelectMachineDialog::
// check_nozzle_mapping), for the preview: advisory, exactly as there - a refusal or silence never
// stops the send; the plug-in asks again while sending and goes without "nozzle_mapping" then.
static json nozzle_mapping_probe(const std::string& printer, const std::string& request)
{
    json out = { { "applies", true }, { "state", "no_answer" } };
    auto seq = std::make_shared<std::string>();
    on_main([seq, printer, request]() {
        DeviceManager* dm  = wxGetApp().getDeviceManager();
        MachineObject* obj = dm ? find_machine(dm, printer) : nullptr;
        if (obj) *seq = obj->command_get_auto_nozzle_mapping(request);
    }, 5000);
    if (seq->empty()) return out;
    for (int i = 0; i < 24; ++i) { // up to 6 s: the phone is waiting on this answer
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto reply = std::make_shared<MachineObject::NozzleMappingReply>();
        on_main([reply, printer]() {
            DeviceManager* dm = wxGetApp().getDeviceManager();
            if (MachineObject* obj = dm ? find_machine(dm, printer) : nullptr) *reply = obj->m_nozzle_mapping_reply;
        }, 3000);
        if (reply->valid && reply->sequence_id == *seq) {
            const bool refused = BambuNozzleMapping::reply_is_refusal(reply->result);
            out["state"] = refused ? "refused" : "accepted";
            if (refused) out["reason"] = reply->reason.empty() ? reply->result : reply->reason;
            return out;
        }
    }
    return out;
}

// prepare_from_record() for a Bambu printer, and the preview when `preview` is set. The record's
// .gcode.3mf is the job (BambuReprint::load_job); the printer is selected and waited for the way
// /api/plates/{i}/send does it; BambuReprint::evaluate() makes the send dialog's checks and its AMS
// mapping on the GUI thread; the parameters are PrintJob's, filled from the job instead of the
// plater, for the same run_bambu().
static std::pair<int, std::string> prepare_bambu_record(const Request& req, const GcodeArchive::Record& rec, const std::string& printer,
                                                        const std::string& recorded, const std::string& recorded_kd,
                                                        const std::string& record_model, bool same_printer, const std::string& mode,
                                                        std::shared_ptr<Prepared>& out, json* preview)
{
    if (!boost::iends_with(rec.file, ".3mf"))
        return { 409, "this record is not a sliced Bambu job (.gcode.3mf), so it cannot go to a Bambu printer" };
    auto job = std::make_shared<BambuReprint::Job>(BambuReprint::load_job(rec.path));
    if (!job->error.empty()) return { 409, job->error };

    // Select the printer (which connects it) and give its status a moment, as a plate send does.
    Request sel = req;
    sel.printer = printer;
    auto rc   = std::make_shared<std::pair<int, std::string>>(500, "not run");
    auto wait = std::make_shared<bool>(false);
    if (!on_main([rc, wait, sel]() { *rc = preselect(sel, *wait); }, 30000)) return { 503, "the slicer is busy" };
    if (rc->first != 200) return *rc;
    if (*wait) {
        auto ready = std::make_shared<bool>(false);
        for (int i = 0; i < 30 && !*ready; ++i) { // up to 15 s
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            on_main([ready, printer]() { *ready = printer_ready(printer); }, 3000);
        }
    }

    BambuReprint::Request br;
    br.mode               = mode;
    br.mapping            = req.ams_mapping;
    br.bed_leveling       = req.bed_leveling;
    br.flow_cali          = req.flow_cali;
    br.timelapse          = req.timelapse;
    br.use_ams            = req.use_ams;
    br.nozzle_offset_cali = req.nozzle_offset_cali;
    br.force              = req.force;

    auto       ev         = std::make_shared<BambuReprint::Evaluation>();
    auto       p          = std::make_shared<Prepared>();
    auto       tmodel     = std::make_shared<std::string>();
    auto       lan_only   = std::make_shared<bool>(false);
    auto       cloud_only = std::make_shared<bool>(false);
    const bool ran        = on_main([ev, p, job, br, printer, tmodel, lan_only, cloud_only]() {
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm) { ev->status = 503; ev->error = "no device manager"; return; }
        if (!wxGetApp().getAgent()) { ev->status = 503; ev->error = "the network plugin is not loaded"; return; }
        MachineObject* obj = find_machine(dm, printer);
        if (!obj) { ev->status = 404; ev->error = "no such printer: " + printer; return; }
        *tmodel = obj->printer_type;
        *ev     = BambuReprint::evaluate(obj, *job, br);
        if (ev->status != 200) return;
        // What PrintJob reads off the printer (SelectMachineDialog::on_send_print).
        p->printer_name       = obj->dev_name;
        p->print_error_before = obj->print_error;
        BBL::PrintParams& ps  = p->params;
        ps.dev_id     = obj->dev_id;
        ps.dev_ip     = obj->dev_ip;
        ps.ftp_folder = obj->get_ftp_folder();
        ps.username   = "bblp";
        ps.password   = obj->get_access_code();
#if !BBL_RELEASE_TO_PUBLIC
        ps.use_ssl_for_ftp  = wxGetApp().app_config->get("enable_ssl_for_ftp") == "true";
        ps.use_ssl_for_mqtt = wxGetApp().app_config->get("enable_ssl_for_mqtt") == "true";
#else
        ps.use_ssl_for_ftp  = obj->local_use_ssl_for_ftp;
        ps.use_ssl_for_mqtt = obj->local_use_ssl_for_mqtt;
#endif
        ps.connection_type = obj->connection_type();
        *lan_only          = wxGetApp().app_config->get("lan_mode_only") == "1";
        *cloud_only        = obj->is_support_cloud_print_only;
        // Only a printer that maps AMS slots gets the mapping (SelectMachineDialog::on_send_print).
        if (!obj->is_support_ams_mapping()) ev->ams_mapping = ev->ams_mapping2 = ev->ams_mapping_info = "";
        ev->use_ams = ev->use_ams && obj->has_ams();
    }, 20000);
    if (!ran) return { 503, "the slicer is busy" };
    if (ev->status != 200) return { ev->status, ev->error };
    // The record's model against the target's, by name (#159): the same rule the Reprint list applies.
    if (!same_printer) {
        const std::string why = GcodeArchive::reprint_target_refusal(recorded, recorded_kd, record_model, printer, "bambu", *tmodel,
                                                                     GcodeArchive::model_names());
        if (!why.empty()) return { 409, why };
    }

    if (preview) {
        json& o     = *preview;
        o           = ev->preview;
        o["record"] = rec.id;
        o["mode"]   = mode;
        o["file"]   = rec.file;
        if (!ev->nozzle_mapping_request.empty() && ev->can_send)
            o["nozzle_mapping"] = nozzle_mapping_probe(printer, ev->nozzle_mapping_request);
        return { 200, "" };
    }

    if (mode == "print" && !ev->can_send) {
        const json probs = ev->preview.value("problems", json::array());
        return { 409, probs.empty() ? std::string("the filaments cannot be mapped to this printer's AMS")
                                    : probs.front().value("text", std::string("the filaments cannot be mapped")) };
    }

    p->kind               = "bambu";
    p->mode               = mode;
    p->plate              = -1;
    p->dry_run            = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
    p->printer_id         = printer;
    p->from_record        = true;
    p->record_id          = rec.id;
    p->upload.source_path = fs::path(rec.path);

    BBL::PrintParams& ps   = p->params;
    const std::string name = print_name_of_record(rec);
    ps.filename            = rec.path;   // the archived .gcode.3mf is the file the plug-in uploads
    ps.config_filename     = "";         // no configuration 3mf: that only serves a cloud print
    ps.plate_index         = job->plate; // the plate number inside the file (Metadata/plate_<n>.gcode)
    const bool has_sdcard  = ev->has_sdcard;
    if (mode == "print") {
        // SelectMachineDialog::on_send_print + PrintJob::process, with the job's own data.
        ps.print_type                = "from_normal";
        ps.project_name              = truncate_utf8(name, 100);
        ps.preset_name               = name + "_plate_" + std::to_string(job->plate);
        ps.task_bed_type             = job->bed_type;
        ps.task_bed_leveling         = ev->bed_leveling;
        ps.task_flow_cali            = ev->flow_cali;
        ps.task_vibration_cali       = false; // the desktop passes false here
        ps.task_record_timelapse     = ev->timelapse;
        ps.task_layer_inspect        = true;
        ps.task_use_ams              = ev->use_ams;
        ps.nozzles_info              = ev->nozzles_info;
        ps.auto_offset_cali          = ev->auto_offset_cali;
        ps.ams_mapping               = ev->ams_mapping;
        ps.ams_mapping2              = ev->ams_mapping2;
        ps.ams_mapping_info          = ev->ams_mapping_info;
        ps.nozzle_mapping_request    = ev->nozzle_mapping_request;
        ps.extruder_cali_manual_mode = 1; // no PA-value switch in this fork: automatic
        if (ps.connection_type == "lan") {
            if (!has_sdcard) return { 409, "An SD card needs to be inserted before printing via LAN." };
            p->call               = "start_local_print";
            p->verify_access_code = true;
        } else {
            const bool can_local = !ps.password.empty() && !ps.dev_ip.empty() && has_sdcard;
            if (*lan_only) {
                if (!can_local) return { 409, "LAN-only mode is on but the printer has no IP address, access code or SD card" };
                p->call = "start_local_print_with_record";
            } else if (!*cloud_only && can_local) {
                p->call                  = "start_local_print_with_record";
                p->lan_fallback_to_cloud = true;
            } else {
                p->call = "start_print";
            }
        }
    } else {
        // SendToPrinterDialog::on_ok_btn + SendJob::process: upload to the printer's storage only.
        ps.project_name = name + ".gcode.3mf";
        ps.preset_name  = name;
        ps.task_use_ams = true;
        if (ps.connection_type == "lan") {
            if (!has_sdcard) return { 409, "An SD card needs to be inserted before sending to printer." };
        } else if (ps.password.empty() || ps.dev_ip.empty() || !has_sdcard) {
            return { 409, "uploading needs the printer's IP address, its access code and an SD card" };
        }
        p->call = "start_send_gcode_to_sdcard";
    }
    out = p;
    return { 200, "" };
}

std::pair<int, std::string> preview_record(const Request& req, json& out)
{
    if (req.record.empty()) return { 400, "record is required" };
    const GcodeArchive::Record rec = GcodeArchive::find(req.record);
    if (rec.id.empty()) return { 404, "no such record: " + req.record };
    boost::system::error_code ec;
    if (rec.path.empty() || !fs::is_regular_file(fs::path(rec.path), ec))
        return { 409, "the file of record " + rec.id + " is gone (evicted, or the archive folder moved); it cannot be sent again" };
    const json&       j           = rec.json;
    const json        jp          = (j.contains("printer") && j["printer"].is_object()) ? j["printer"] : json::object();
    const std::string recorded    = jp.value("id", std::string());
    const std::string recorded_kd = jp.value("kind", std::string());
    const std::string printer     = req.printer.empty() ? recorded : req.printer;
    if (printer.empty()) return { 409, "this record does not say which printer it went to; name one with printer=" };
    const std::string kind = printer_kind_of(printer);
    if (kind != "bambu") return { 409, "only a reprint to a Bambu printer has a mapping preview; send it directly" };
    const std::string record_model = jp.value("model", std::string());
    const bool        same_printer = GcodeArchive::reprint_in_place_allowed(recorded, printer);
    if (!same_printer) {
        const std::string why = GcodeArchive::reprint_target_refusal(recorded, recorded_kd, record_model, printer, kind, record_model,
                                                                     GcodeArchive::model_names());
        if (!why.empty()) return { 409, why };
    }
    const std::string mode = req.mode.empty() ? std::string("print") : req.mode;
    if (mode != "upload" && mode != "print") return { 400, "mode must be upload or print" };
    std::shared_ptr<Prepared> unused;
    return prepare_bambu_record(req, rec, printer, recorded, recorded_kd, record_model, same_printer, mode, unused, &out);
}

std::pair<int, std::string> prepare_from_record(const Request& req, std::shared_ptr<Prepared>& out)
{
    if (req.record.empty())
        return { 400, "record is required" };
    const GcodeArchive::Record rec = GcodeArchive::find(req.record);
    if (rec.id.empty())
        return { 404, "no such record: " + req.record };
    // A reprint puts a file on a printer, so it is always deliberate - upload as well as print.
    if (!req.confirm)
        return { 400, "sending an archived file needs confirm=1" };
    boost::system::error_code ec;
    if (rec.path.empty() || !fs::is_regular_file(fs::path(rec.path), ec))
        return { 409, "the file of record " + rec.id + " is gone (evicted, or the archive folder moved); it cannot be sent again" };

    const json& j  = rec.json;
    const json  jp = (j.contains("printer") && j["printer"].is_object()) ? j["printer"] : json::object();
    const std::string recorded    = jp.value("id", std::string());
    const std::string recorded_kd = jp.value("kind", std::string());
    const std::string printer     = req.printer.empty() ? recorded : req.printer;
    if (printer.empty())
        return { 409, "this record does not say which printer it went to; name one with printer=" };
    const std::string kind = kind_of_printer(printer);
    // Another printer than the one the record went to: the same kind (a .gcode.3mf cannot go to a
    // Moonraker host and a plain .gcode cannot go through the Bambu plugin) and the same model - a
    // sliced file is made for one model, and any printer of that model takes it. The model check
    // needs the target's model, so it is made again below once the target is looked up; this first
    // pass refuses what the ids alone already rule out.
    const std::string record_model = jp.value("model", std::string());
    const bool        same_printer = GcodeArchive::reprint_in_place_allowed(recorded, printer);
    if (!same_printer) {
        const std::string why = GcodeArchive::reprint_target_refusal(recorded, recorded_kd, record_model, printer, kind,
                                                                     record_model, GcodeArchive::model_names());
        if (!why.empty()) return { 409, why };
    }
    // The MQTT "connect" path (the Snapmaker the PC's Device tab is connected to) starts its print
    // from the PC's own preprint page; a U1 is reprinted through its LAN card (sm:<id>) instead.
    if (kind == "connect")
        return { 409, "reprinting over the PC's Snapmaker connection is not supported; add the printer on the home network "
                      "(the PC's Device tab) and reprint to it there" };

    const std::string mode = req.mode.empty() ? j.value("mode", std::string("upload")) : req.mode;
    if (mode != "upload" && mode != "print")
        return { 400, "mode must be upload or print" };

    // A Bambu printer: the job is read back from the .gcode.3mf and sent through the same
    // parameters and plug-in calls as a plate send (prepare_bambu_record).
    if (kind == "bambu")
        return prepare_bambu_record(req, rec, printer, recorded, recorded_kd, record_model, same_printer, mode, out, nullptr);

    // The name the printer is given: the caller's, else the one it was given last time, else the
    // archived file's own name. Never a directory, whatever the sidecar holds.
    std::string name = req.name.empty() ? j.value("sent_name", std::string()) : req.name;
    if (name.empty()) name = rec.file;
    name = fs::path(name).filename().string();
    if (name.empty() || name == "." || name == "..") name = rec.file;

    auto p        = std::make_shared<Prepared>();
    p->mode       = mode;
    p->plate      = -1;
    p->dry_run    = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
    p->printer_id = printer;
    p->from_record = true;
    p->record_id   = rec.id;
    p->upload.source_path = fs::path(rec.path);
    p->upload.upload_path = fs::path(name);

    if (kind == "snapmaker") {
        SnapmakerLan::Device d;
        if (!SnapmakerLan::find(printer.substr(3), d))
            return { 404, "no such printer: " + printer };
        if (!same_printer) {
            const std::string why = GcodeArchive::reprint_target_refusal(recorded, recorded_kd, record_model, printer, kind,
                                                                         d.model, GcodeArchive::model_names());
            if (!why.empty()) return { 409, why };
        }
        const SnapmakerLan::Status st = SnapmakerLan::status(d);
        if (!st.online)
            return { 409, d.name + " is not answering on the network" };
        if (st.login_required)
            return { 409, d.name + " asks for a login; the phone can only reach a printer that does not" };
        if (st.printing())
            return { 409, d.name + " is " + (st.state == "paused" ? "paused mid-print" : "printing") + " (" + st.filename + ")" };
        p->kind         = "snapmaker";
        p->printer_name = d.name.empty() ? d.ip : d.name;
        p->lan          = d;
        p->toolheads    = SnapmakerLan::toolheads(d);
        if (!boost::iends_with(name, ".gcode")) name += ".gcode";
        // Where the printer keeps it, when the send said (the PC's pre-print page names the path
        // it started): the file a reprint looks for first.
        // Only on the printer the file went to: another one never has "the same file" by name.
        std::string remote = same_printer ? j.value("remote_path", std::string()) : std::string();
        if (remote.compare(0, 7, "gcodes/") == 0) remote = remote.substr(7);
        while (!remote.empty() && remote.front() == '/') remote.erase(remote.begin());
        if (!remote.empty() && remote.find("..") == std::string::npos && req.name.empty()) name = remote;
        p->upload.upload_path = fs::path(name);
        p->lan_filename       = name;
        // Still on the printer, the same size as the archived bytes: start it in place. An upload
        // made "to start later" is exactly this - the phone starts the file the printer already
        // has. Anything else (gone, a different size, the printer cannot say) uploads again.
        p->reuse_remote       = same_printer && SnapmakerLan::file_on_printer(d, name, rec.size);
        p->file_filaments     = file_filaments_of_record(j);
        // A reprint unloads if the print it replays did. A record written before this existed has
        // no such key and reprints the way it always has.
        p->unload_supported   = true; // an archived Snapmaker send: the same family, the same firmware
        p->unload_at_end      = j.value("unload_at_end", false);
        // ... unless this reprint says otherwise, exactly as it may override the mapping.
        if (!req.unload_at_end.empty())
            p->unload_at_end = req.unload_at_end == "1" || req.unload_at_end == "true";
        if (mode == "print") {
            // The record's own mapping is the memory; the caller may override it, and a record
            // written before mappings were kept falls back to the colour match.
            // Another printer of the model has its own toolheads: match them by colour again rather
            // than replaying toolhead numbers that belonged to the first printer.
            const std::string wanted = !req.mapping.empty() ? req.mapping
                                     : same_printer         ? j.value("mapping", std::string())
                                                            : std::string();
            std::string       error;
            if (wanted.empty())
                p->mapping = SnapmakerLan::auto_match(p->file_filaments, p->toolheads);
            else if (!parse_mapping(wanted, p->file_filaments.size(), p->mapping, error))
                return { 400, error };
            for (const SnapmakerLan::FileFilament& f : p->file_filaments) {
                if (!f.used || (size_t) f.index >= p->mapping.size()) continue;
                if (p->mapping[f.index] < 0)
                    return { 400, "filament " + std::to_string(f.index + 1) + " has no toolhead; every filament the file uses needs one" };
                if (!p->toolheads.empty() && p->mapping[f.index] >= (int) p->toolheads.size())
                    return { 400, p->printer_name + " has " + std::to_string(p->toolheads.size()) +
                                      " toolheads, so there is no toolhead " + std::to_string(p->mapping[f.index] + 1) };
            }
            if (!p->toolheads.empty() && !req.force)
                for (const SnapmakerLan::FileFilament& f : p->file_filaments) {
                    if (!f.used || (size_t) f.index >= p->mapping.size()) continue;
                    const int h = p->mapping[f.index];
                    if (h >= 0 && h < (int) p->toolheads.size() && !p->toolheads[h].loaded)
                        return { 409, "toolhead " + std::to_string(h + 1) + " is empty; load it, pick another, or send force=1" };
                }
        }
        out = p;
        return { 200, "" };
    }

    // A print host: the address is the recorded device's ("ph:<id>", looked up across every printer
    // model, since the record may be for a printer other than the one the PC has selected now), or
    // the PC's current printer preset's for a plain "host" record. Presets are GUI-thread state,
    // so this is the one hop onto it.
    auto host  = std::make_shared<std::shared_ptr<PrintHost>>();
    auto url   = std::make_shared<std::string>();
    auto hname = std::make_shared<std::string>();
    auto tmodel = std::make_shared<std::string>(); // the target's printer model, for a reprint to another printer
    auto rc    = std::make_shared<std::pair<int, std::string>>(200, "");
    const std::string device = printer.compare(0, 3, "ph:") == 0 ? printer.substr(3) : std::string();
    const bool ran = on_main([host, url, hname, tmodel, rc, device]() {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (!bundle) { *rc = { 503, "no preset bundle" }; return; }
        if (!device.empty()) {
            for (const auto& kv : PrintHostDevices::all_devices())
                for (const PrintHostDevices::Device& d : kv.second) {
                    if (d.id != device || host->get()) continue;
                    if (d.address.empty()) { *rc = { 409, d.display_name() + " has no address" }; return; }
                    DynamicPrintConfig dev_cfg = PrintHostDevices::config_for(d, bundle->printers.get_edited_preset().config);
                    host->reset(PrintHost::get_print_host(&dev_cfg, false));
                    *url    = d.address;
                    *hname  = d.display_name();
                    *tmodel = d.printer_model.empty() ? kv.first : d.printer_model;
                }
            if (!*host) { *rc = { 404, "no such print-host device: ph:" + device }; return; }
            return;
        }
        if (auto* model = bundle->printers.get_edited_preset().config.option<ConfigOptionString>("printer_model"))
            *tmodel = model->value;
        if (bundle->use_bbl_network()) {
            *rc = { 409, "the current printer preset sends through the Bambu network; pick that printer by its id" };
            return;
        }
        DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
        *url = cfg.opt_string("print_host");
        if (url->empty()) { *rc = { 409, "the printer preset has no print host address" }; return; }
        host->reset(PrintHost::get_print_host(&cfg, false));
        if (!*host) { *rc = { 500, "unsupported host type" }; return; }
        *hname = std::string((*host)->get_name());
    }, 20000);
    if (!ran) return { 503, "the slicer is busy" };
    if (rc->first != 200) return *rc;
    if (!same_printer) {
        const std::string why = GcodeArchive::reprint_target_refusal(recorded, recorded_kd, record_model, printer, kind,
                                                                     *tmodel, GcodeArchive::model_names());
        if (!why.empty()) return { 409, why };
    }

    p->kind         = "printhost";
    p->printer_name = device.empty() ? *hname + " " + *url : *hname;
    p->host         = *host;
    p->device_id    = device;
    // The archived file keeps the extension it was sent with, so the payload's form is on disk.
    p->upload.use_3mf = boost::iends_with(rec.file, ".3mf");
    if (mode == "print") {
        if (!p->host->get_post_upload_actions().has(PrintHostPostUploadAction::StartPrint))
            return { 409, *hname + " cannot start a print after the upload" };
        if (dynamic_cast<Moonraker*>(p->host.get()) != nullptr) {
            if (dynamic_cast<Moonraker_Mqtt*>(p->host.get()) == nullptr)
                return { 409, "this Moonraker host can only start prints from the PC's preprint page" };
            p->two_step = true;
        }
    }
    p->upload.post_action = (mode == "print" && !p->two_step) ? PrintHostPostUploadAction::StartPrint
                                                              : PrintHostPostUploadAction::None;
    out = p;
    return { 200, "" };
}

// -------------------------------------------------------------------- run ----

// Ultra: the file has reached the printer - keep a copy if the user asked for one. Archiving is a
// side note to a send: it reports into the result and can never turn a good send into a failure.
static void archive_sent(std::shared_ptr<Prepared> p, const std::string& path, json& result)
{
    // A reprint replays bytes the archive already holds: recording them a second time would burn
    // one of the user's kept records on a file that is already there (stage 3 appends to sent[]).
    if (p->from_record) {
        result["reprint_of"] = p->record_id;
        // The record's own history of sends (GcodeArchive::note_reprint): what went where, when.
        if (!p->dry_run) {
            json entry;
            entry["time"]    = (long long) std::time(nullptr);
            entry["printer"] = { { "id", p->printer_id }, { "name", p->printer_name }, { "kind", p->kind } };
            entry["mode"]    = p->mode;
            entry["source"]  = "phone";
            if (p->kind == "bambu") entry["plate"] = p->params.plate_index;
            if (p->reuse_remote) entry["in_place"] = true;
            if (GcodeArchive::note_reprint(p->record_id, entry)) result["history"] = true;
            // An upload made to start later, started now: the record is a print from here on.
            if (p->kind == "bambu" && p->mode == "print") GcodeArchive::set_mode(p->record_id, "print");
        }
        return;
    }
    if (p->dry_run || !GcodeArchive::enabled()) return;
    const GcodeArchive::Record r = GcodeArchive::archive(path, p->archive_meta);
    if (!r.id.empty()) result["archived"] = r.id;
}

// Ultra stage 1d: the file is on the printer - deduct its filament from the bound spools, the way
// Plater::print_job_finished and SSWCP's print start already do for the desktop. CallAfter, never a
// wait: deduct_after_send_async() reads the plate on the GUI thread and then does the HTTP on a
// thread of its own, so the send thread is never held up and a Spoolman that is down or wrong can
// only log. It re-checks the preference itself; p->spoolman_deduct is what prepare() saw.
static void deduct_spoolman(std::shared_ptr<Prepared> p, json& result)
{
    if (p->dry_run || !p->spoolman_deduct) return;
    result["spoolman_deduct"] = true;
    try {
        if (!RemoteAccess::post_to_main([]() {
                try { SpoolmanDialog::deduct_after_send_async(); } catch (...) {}
            }))
            BOOST_LOG_TRIVIAL(warning) << "RemoteSend: the Spoolman deduction was not queued: the slicer is closing";
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "RemoteSend: the Spoolman deduction could not be queued";
    }
}

static void run_bambu(std::shared_ptr<Prepared> p, Sink& sink)
{
    NetworkAgent* agent = wxGetApp().getAgent();
    json          result;
    result["kind"]    = "bambu";
    result["mode"]    = p->mode;
    result["printer"] = { { "id", p->printer_id }, { "name", p->printer_name } };
    result["call"]    = p->call;
    result["params"]  = params_json(p->params);
    if (p->from_record) {
        // A reprint's source is inside the archive folder, and the phone never learns where that
        // is (no paths on the wire): its id says everything.
        result["record"]             = p->record_id;
        result["params"]["filename"] = "archive:" + p->record_id;
    }
    if (!agent) { sink.done(false, "the network plugin is not loaded", result); return; }
    if (p->dry_run) {
        result["dry_run"] = true;
        sink.progress(99, "dry run: nothing was sent");
        sink.done(true, "", result);
        return;
    }

    const bool  upload_only = p->mode == "upload";
    std::mutex  m;
    std::string last_error;
    auto update_fn = [&](int stage, int code, std::string info) {
        std::lock_guard<std::mutex> lock(m);
        if (stage < 0 || code < 0 || code > 100 || stage == BBL::PrintingStageERROR) {
            last_error = info.empty() ? "error code " + std::to_string(code) : info;
            return;
        }
        sink.progress(stage_percent(stage, code, upload_only), info.empty() ? stage_name(stage) : info);
    };
    auto cancel_fn = []() { return false; };
    auto wait_fn   = [](int, std::string) { return true; };

    if (p->verify_access_code) {
        // PrintJob::process: a tiny upload proves the IP address and access code before the real file goes out.
        BBL::PrintParams v = p->params;
        v.project_name     = "verify_job";
        v.filename         = (fs::path(resources_dir()) / "check_access_code.txt").string();
        sink.progress(5, "checking the access code");
        if (agent->start_send_gcode_to_sdcard(v, nullptr, nullptr, nullptr) != 0) {
            sink.done(false, p->printer_name + " rejected the access code (or its IP address changed); fix it on the PC's Device tab", result);
            return;
        }
    }
    sink.progress(10, p->params.connection_type == "lan" ? "Sending print job over LAN" : "Sending print job through cloud service");
    int rc = -1;
    if (p->call == "start_send_gcode_to_sdcard") {
        rc = agent->start_send_gcode_to_sdcard(p->params, update_fn, cancel_fn, nullptr);
    } else if (p->call == "start_local_print") {
        rc = agent->start_local_print(p->params, update_fn, cancel_fn);
    } else if (p->call == "start_local_print_with_record") {
        rc = agent->start_local_print_with_record(p->params, update_fn, cancel_fn, wait_fn);
        if (rc < 0 && p->lan_fallback_to_cloud) {
            result["fallback"] = "cloud";
            sink.progress(10, "Sending print job through cloud service");
            rc = agent->start_print(p->params, update_fn, cancel_fn, wait_fn);
        }
    } else if (p->call == "start_print") {
        rc = agent->start_print(p->params, update_fn, cancel_fn, wait_fn);
    }
    result["result_code"] = rc;
    if (rc < 0) {
        std::lock_guard<std::mutex> lock(m);
        sink.done(false, result_text(rc) + (last_error.empty() ? "" : ": " + last_error), result);
        return;
    }
    archive_sent(p, p->params.filename, result);
    if (upload_only) { sink.done(true, "", result); return; }

    // The command left the PC; the printer may still refuse it (an H2-series printer that is not in
    // LAN-only mode with Developer Mode answers "command verification failed" on its own screen).
    // Watch what it reports for a few seconds so the phone learns about it.
    sink.progress(98, "waiting for the printer to start");
    struct Watch { std::mutex m; std::string state { "unknown" }, err_text; int err { 0 }; };
    auto w = std::make_shared<Watch>(); // shared: a timed-out GUI call may still run after this loop
    for (int i = 0; i < 12; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        on_main([w, p]() {
            DeviceManager* dm = wxGetApp().getDeviceManager();
            if (!dm) return;
            MachineObject* obj = find_machine(dm, p->printer_id);
            if (!obj) return;
            std::lock_guard<std::mutex> lock(w->m);
            if (w->state != "unknown") return;
            if (obj->print_error != 0 && obj->print_error != p->print_error_before) {
                w->err   = obj->print_error;
                w->state = "error";
                if (HMSQuery* q = wxGetApp().get_hms_query()) w->err_text = q->describe_print_error(obj->dev_id, w->err).ToUTF8().data();
            } else if (obj->is_in_printing()) {
                w->state = "printing";
            }
        }, 3000);
        std::lock_guard<std::mutex> lock(w->m);
        if (w->state != "unknown") break;
    }
    std::lock_guard<std::mutex> lock(w->m);
    result["printer_state"] = w->state;
    if (w->state == "error") {
        char code[16];
        std::snprintf(code, sizeof code, "%08X", (unsigned) w->err);
        result["printer_error"] = { { "code", code }, { "message", w->err_text } };
        // err_text already names the code (HMSQuery::format_error), so it is not repeated here.
        sink.done(false,
                  w->err_text.empty() ? "the printer refused the print (error " + HMSQuery::pretty_code(code) + ")"
                                      : "the printer refused the print - " + w->err_text,
                  result);
        return;
    }
    sink.done(true, "", result);
}

static void run_host(std::shared_ptr<Prepared> p, Sink& sink)
{
    json result;
    result["kind"]        = p->kind;
    result["mode"]        = p->mode;
    result["printer"]     = { { "id", p->printer_id }, { "name", p->printer_name } };
    result["host"]        = p->host->get_name();
    result["url"]         = p->host->get_host();
    // A reprint's source is inside the archive folder, and the phone must never learn where that
    // is (the design's privacy line: no paths on the wire). Its id says everything it needs.
    result["source"]      = p->from_record ? ("archive:" + p->record_id) : p->upload.source_path.string();
    result["upload_path"] = p->upload.upload_path.string();
    result["post_action"] = p->upload.post_action == PrintHostPostUploadAction::StartPrint ? "start_print" : "none";
    result["two_step"]    = p->two_step;
    if (p->from_record) result["record"] = p->record_id;
    if (p->dry_run) {
        result["dry_run"] = true;
        sink.progress(99, "dry run: nothing was sent");
        sink.done(true, "", result);
        return;
    }
    std::string error;
    sink.progress(1, "uploading " + p->upload.upload_path.string());
    const bool ok = p->host->upload(
        p->upload,
        [&](Http::Progress prog, bool& cancel) {
            cancel = false;
            if (prog.ultotal > 0) {
                const size_t pct = prog.ulnow * 100 / prog.ultotal;
                sink.progress((int) std::min<size_t>(95, pct * 95 / 100), "uploading " + std::to_string(pct) + "%");
            }
        },
        [&](wxString e) { error = e.ToUTF8().data(); },
        [&](wxString tag, wxString status) { if (tag == "resolve") result["resolved"] = std::string(status.ToUTF8().data()); });
    if (!ok) { sink.done(false, error.empty() ? "upload failed" : error, result); return; }
    result["uploaded"] = true;
    archive_sent(p, p->upload.source_path.string(), result);
    deduct_spoolman(p, result);
    if (!p->two_step) { sink.done(true, "", result); return; }

    sink.progress(97, "starting the print");
    auto reply = std::make_shared<std::promise<json>>();
    auto once  = std::make_shared<std::atomic<bool>>(false);
    auto fut   = reply->get_future();
    p->host->async_start_print_job(p->upload.upload_path.string(), [reply, once](const json& r) {
        if (!once->exchange(true)) reply->set_value(r);
    });
    if (fut.wait_for(std::chrono::seconds(30)) != std::future_status::ready) {
        sink.done(false, "the printer did not answer the print start within 30 s (the file is uploaded)", result);
        return;
    }
    const json r          = fut.get();
    result["start_reply"] = r;
    if (r.is_null() || (r.is_object() && r.contains("error"))) {
        sink.done(false, "the printer did not start the print: " + (r.is_null() ? std::string("no reply") : r["error"].dump()), result);
        return;
    }
    sink.done(true, "", result);
}

// A Snapmaker on the LAN: upload with print=false, then - for a print - the toolhead mapping and
// the start, each a plain HTTP call. Nothing here needs the PC to be connected to the printer.
static void run_snapmaker(std::shared_ptr<Prepared> p, Sink& sink)
{
    json result;
    result["kind"]     = "snapmaker";
    result["mode"]     = p->mode;
    result["printer"]  = { { "id", p->printer_id }, { "name", p->printer_name } };
    result["url"]      = SnapmakerLan::base_url(p->lan);
    result["source"]   = p->from_record ? ("archive:" + p->record_id) : p->upload.source_path.string();
    result["filename"] = p->lan_filename;
    if (p->from_record) result["record"] = p->record_id;
    json filaments = json::array();
    for (const SnapmakerLan::FileFilament& f : p->file_filaments) {
        json j;
        j["index"]   = f.index;
        j["color"]   = f.color;
        j["type"]    = f.type;
        j["used"]    = f.used;
        j["used_g"]  = f.used_g;
        j["toolhead"] = (size_t) f.index < p->mapping.size() ? p->mapping[f.index] : -1;
        filaments.push_back(j);
    }
    result["filaments"] = filaments;
    if (p->mode == "print") {
        result["mapping"]        = p->mapping;
        result["mapping_script"] = SnapmakerLan::mapping_script(p->mapping, p->unload_at_end);
        result["unload_at_end"]  = p->unload_at_end;
        result["unload_supported"] = p->unload_supported;
    }
    if (p->dry_run) {
        result["dry_run"] = true;
        sink.progress(99, "dry run: nothing was sent");
        sink.done(true, "", result);
        return;
    }
    std::string error;
    if (p->reuse_remote && p->mode == "print") {
        // A reprint of a file the printer still holds: nothing to upload, start it where it is.
        result["uploaded"]    = false;
        result["reused_file"] = true;
        sink.progress(90, p->lan_filename + " is still on the printer");
    } else {
        sink.progress(1, "uploading " + p->lan_filename);
        if (!SnapmakerLan::upload(p->lan, p->upload.source_path.string(), p->lan_filename,
                                  [&sink](int pct) { sink.progress(std::min(95, pct * 95 / 100), "uploading " + std::to_string(pct) + "%"); },
                                  error)) {
            sink.done(false, error.empty() ? "the upload failed" : error, result);
            return;
        }
        result["uploaded"] = true;
    }
    archive_sent(p, p->upload.source_path.string(), result);
    deduct_spoolman(p, result);
    long long size     = 0;
    if (SnapmakerLan::metadata(p->lan, p->lan_filename, size, error))
        result["size"] = size;
    else
        result["metadata_error"] = error; // the file may still be there; the print start will tell
    if (p->mode != "print") {
        sink.progress(100, "uploaded");
        sink.done(true, "", result);
        return;
    }
    sink.progress(97, "starting the print");
    json sent;
    // A printer that reports no toolheads at all (a plain Klipper machine someone added by IP) has
    // nothing to map: the standard start is right for it.
    const bool mapped = !p->toolheads.empty();
    bool       ok     = mapped ? SnapmakerLan::start_print_mapped(p->lan, p->lan_filename, p->mapping, p->unload_at_end, sent, error)
                               : SnapmakerLan::start_print(p->lan, p->lan_filename, error);
    result["start"] = sent;
    if (!ok) {
        sink.done(false, error + " (the file is on the printer)", result);
        return;
    }
    // An upload made to start later, started now: the record is a print from here on.
    if (p->from_record && !p->record_id.empty())
        GcodeArchive::set_mode(p->record_id, "print", p->reuse_remote ? p->lan_filename : std::string());
    // What the printer itself says a moment later - the only proof the job took.
    for (int i = 0; i < 6; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const SnapmakerLan::Status st = SnapmakerLan::status_now(p->lan); // never the cached answer here
        result["printer_state"]       = st.state;
        result["printer_file"]        = st.filename;
        if (st.state == "printing" || st.state == "paused") {
            sink.progress(100, "printing");
            sink.done(true, "", result);
            return;
        }
        if (st.state == "error") {
            sink.done(false, "the printer refused the print" + (st.message.empty() ? "" : ": " + st.message), result);
            return;
        }
    }
    // It accepted every call but has not started yet: not a failure, just not proof.
    sink.progress(100, "sent");
    sink.done(true, "", result);
}

void run(std::shared_ptr<Prepared> p, Sink sink)
{
    try {
        if (p->kind == "bambu")           run_bambu(p, sink);
        else if (p->kind == "snapmaker")  run_snapmaker(p, sink);
        else                              run_host(p, sink);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "RemoteSend: " << e.what();
        sink.done(false, std::string("send failed: ") + e.what(), json::object());
    } catch (...) {
        sink.done(false, "send failed", json::object());
    }
}

// ----------------------------------------------------------- /api/printers ----

void describe_bambu(MachineObject* m, json& p)
{
    p["kind"]            = "bambu";
    p["lan_mode"]        = m->is_lan_mode_printer();
    p["access_code_set"] = m->has_access_right();
    p["has_ams"]         = m->has_ams();
    p["ams_mapping"]     = m->is_support_ams_mapping();
    p["sdcard"]          = m->get_sdcard_state() == MachineObject::SdcardState::HAS_SDCARD_NORMAL;
    std::string profile_model;
    p["model_matches"]   = same_printer_model(m, &profile_model);
    p["profile_model"]   = profile_model;
    p["can_upload"]      = true;
    p["can_print"]       = true;
    p["options"]         = { { "bed_leveling", remembered("bed_leveling") }, { "flow_cali", remembered("flow_cali") },
                             { "timelapse", remembered("timelapse") },       { "vibration_cali", false },
                             { "use_ams", m->has_ams() } };
}

// Plater::priv::get_export_gcode_filename (Plater.cpp:16778) for a plate that is not the current
// one: that helper reads partplate_list.get_curr_plate(), and merely listing printers must not move
// the plate the PC shows. Same rule - the project name (or the first object's, for a project that
// was never named or saved), then the plate's own name, or "_plate_<n>" when there is more than one.
std::string export_name_for(int plate, const std::string& extension)
{
    Plater* plater = wxGetApp().plater();
    if (!plater) return {};
    PartPlateList& plates = plater->get_partplate_list();
    if (plate < 0 || plate >= plates.get_plate_count()) plate = plates.get_curr_plate_index();
    if (plate < 0 || plate >= plates.get_plate_count()) return {};
    std::string       suffix;
    const std::string plate_name = plates.get_plate(plate)->get_plate_name();
    if (!plate_name.empty())
        suffix = "_" + plate_name;
    else if (plates.get_plate_count() > 1)
        suffix = "_plate_" + std::to_string(plate + 1);
    wxString base = plater->get_project_name();
    // An unsaved, unnamed project is named after its first object, as the export does.
    if (plater->get_project_filename().empty() && base == _L("Untitled") && !plater->model().objects.empty())
        base = wxString(fs::path(plater->model().objects.front()->name).replace_extension().c_str());
    return std::string(base.ToUTF8().data()) + suffix + extension;
}

std::string lan_upload_name(int plate) { return export_name_for(plate, ".gcode"); }

void list_hosts(json& printers, int plate)
{
    // The Snapmakers on the LAN are listed by the caller from its own thread (RemoteAccess::
    // api_printers): listing them probes each one over HTTP with a 4 s timeout when the status
    // cache is stale, and this function runs on the GUI thread. Doing it here froze the whole
    // window while the phone's Devices page polled (2026-09-06).
    PresetBundle*       bundle = wxGetApp().preset_bundle;
    DynamicPrintConfig& cfg    = bundle->printers.get_edited_preset().config;
    const bool          use_3mf = bundle->is_bbl_vendor();
    const std::string   upload_name = export_name_for(plate, use_3mf ? ".gcode.3mf" : ".gcode");
    const std::string   url         = cfg.opt_string("print_host");
    if (!url.empty() && !bundle->use_bbl_network()) {
        std::unique_ptr<PrintHost> host(PrintHost::get_print_host(&cfg, false));
        json p;
        p["id"]          = "host";
        p["kind"]        = "printhost";
        p["name"]        = (host ? std::string(host->get_name()) : std::string("print host")) + " " + url;
        p["model"]       = cfg.opt_string("printer_model");
        p["url"]         = url;
        p["online"]      = true; // a print host has no live status here; the upload itself tells
        p["can_upload"]  = host != nullptr;
        p["can_print"]   = host && host->get_post_upload_actions().has(PrintHostPostUploadAction::StartPrint) &&
                           (dynamic_cast<Moonraker*>(host.get()) == nullptr || dynamic_cast<Moonraker_Mqtt*>(host.get()) != nullptr);
        p["upload_name"] = upload_name;
        printers.push_back(p);
    }
    // The printers of this model, by address (<datadir>/hub/print_host_devices.json). Read-only in
    // phase 1: the phone's Devices tab can see the whole list, but a send still goes through the one
    // address the preset holds (the "host" entry above, which is whichever device is current).
    // Their live state is filled in off this thread by RemoteControl::describe_hosts, which probes
    // only the Moonraker-shaped ones through its own probe cache; the rest stay "unknown".
    // A Bambu printer is left out: it has its own device list, in the Device tab.
    if (!bundle->use_bbl_network()) try {
        PrintHostDevices::migrate_from_presets(*bundle); // the preset's own address becomes device 1
        const std::string          model_key = PrintHostDevices::current_model_key(*bundle);
        const std::string          selected  = PrintHostDevices::last_used_id(model_key);
        const std::string          norm_url  = PrintHostDevices::normalize_address(url);
        for (const PrintHostDevices::Device& d : PrintHostDevices::devices(model_key)) {
            json p;
            p["id"]           = "ph:" + d.id;
            p["kind"]         = "printhost";
            p["name"]         = d.display_name();
            p["model"]        = d.printer_model.empty() ? cfg.opt_string("printer_model") : d.printer_model;
            p["url"]          = d.address;
            p["host_type"]    = d.host_type;
            p["device_id"]    = d.id;
            p["model_key"]    = model_key;
            p["alias"]        = d.alias;
            // Not "the main printer" - the one the last plate went to, which is what the desktop
            // send dialog and the Device tab preselect. Falls back to whichever device happens to
            // carry the preset's own address when nothing was ever sent.
            const bool is_last = (!selected.empty() && d.id == selected) ||
                                 (selected.empty() && !norm_url.empty() && PrintHostDevices::normalize_address(d.address) == norm_url);
            p["last_used"]    = is_last;
            p["is_current"]   = is_last; // phase 1's name for it, kept so the phone's list still reads
            p["online"]       = true;   // no live status until describe_hosts answers for it
            p["status"]       = "unknown";
            // A device with an address can be sent to, whatever the preset's own print_host says:
            // POST /api/plates/{i}/send?printer=ph:<id> builds the host from this device.
            std::unique_ptr<PrintHost> dev_host;
            if (!d.address.empty()) {
                DynamicPrintConfig dev_cfg = PrintHostDevices::config_for(d, cfg);
                dev_host.reset(PrintHost::get_print_host(&dev_cfg, false));
            }
            p["can_upload"]   = dev_host != nullptr;
            p["can_print"]    = dev_host && dev_host->get_post_upload_actions().has(PrintHostPostUploadAction::StartPrint) &&
                                (dynamic_cast<Moonraker*>(dev_host.get()) == nullptr || dynamic_cast<Moonraker_Mqtt*>(dev_host.get()) != nullptr);
            p["can_pause"]    = false;
            p["can_resume"]   = false;
            p["can_stop"]     = false;
            p["print_error"]  = nullptr;
            p["upload_name"]  = upload_name;
            printers.push_back(p);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[PrintHostDevices] listing devices failed: " << e.what();
    } catch (...) {}

    std::shared_ptr<PrintHost> connected;
    wxGetApp().get_connect_host(connected);
    if (connected) {
        const std::string host  = connected->get_host();
        // Through the Snapmaker cloud the host is the cloud's MQTT broker
        // (a1pr8yczi3n0se.iot.us-west-1.amazonaws.com:8883), which named the card "Snapmaker
        // a1pr8y...amazonaws.com" and read as the printer's address. The card is named after the
        // printer the Device tab marked connected instead, and says "cloud" rather than an address.
        const bool        cloud = SnapmakerLan::is_cloud_host(host);
        DeviceInfo        which;
        bool              known = false;
        if (wxGetApp().app_config)
            for (const DeviceInfo& d : wxGetApp().app_config->get_devices()) {
                if (!d.connected) continue;
                const bool same_addr = !d.ip.empty() && host.compare(0, d.ip.size(), d.ip) == 0;
                if (!known || same_addr) { which = d; known = true; }
                if (same_addr) break;
            }
        json p;
        p["id"]          = "connect";
        p["kind"]        = "connect";
        p["name"]        = known && !which.dev_name.empty() ? which.dev_name
                                                            : (cloud ? std::string("Snapmaker (cloud)") : "Snapmaker " + host);
        p["model"]       = known && !which.model_name.empty() ? which.model_name : cfg.opt_string("printer_model");
        p["url"]         = host;
        p["via"]         = cloud ? "cloud" : "lan";
        if (!cloud) p["ip"] = SnapmakerLan::host_of(host);
        // The LAN card of the same printer, when it has one: /api/printers lets that card win while
        // it answers (RemoteAccess::api_printers), and the connect stays as the cloud fallback.
        if (known && !which.sn.empty()) p["lan_id"] = "sm:" + which.sn;
        p["online"]      = connected->check_sn_arrived();
        p["can_upload"]  = true;
        p["can_print"]   = dynamic_cast<Moonraker_Mqtt*>(connected.get()) != nullptr;
        p["upload_name"] = upload_name;
        printers.push_back(p);
    }
}

} // namespace RemoteSend
} // namespace GUI
} // namespace Slic3r
