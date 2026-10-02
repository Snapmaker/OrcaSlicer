#include "BambuReprint.hpp"

#include "BambuSendMapping.hpp"
#include "BitmapCache.hpp"
#include "DeviceManager.hpp"
#include "DualNozzleState.hpp"
#include "GcodeArchive.hpp"
#include "GUI_App.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/BambuExtruderMap.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp" // NOT_GENERATE_TIMELAPSE
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"                // Print::get_hrc_by_nozzle_type
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/miniz_extension.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace Slic3r {
namespace GUI {
namespace BambuReprint {

using nlohmann::json;

// ------------------------------------------------------------------------ the job ----

// "#RRGGBB" / "#RRGGBBAA" / "RRGGBBAA" -> "#RRGGBBAA" upper case, the form the send dialog builds
// its filament list with (SelectMachineDialog::reset_and_sync_ams_list: parse_color4 + %02X).
static std::string colour8(const std::string& c)
{
    std::string s = c;
    if (!s.empty() && s[0] != '#') s = "#" + s;
    unsigned char rgba[4] = { 0, 0, 0, 255 };
    if (!BitmapCache::parse_color4(s, rgba)) return c;
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X%02X", rgba[0], rgba[1], rgba[2], rgba[3]);
    return buf;
}

// "#RRGGBBAA" -> "#RRGGBB", for the phone's swatches.
static std::string colour6(const std::string& c)
{
    const std::string s = colour8(c);
    return s.size() >= 7 ? s.substr(0, 7) : s;
}

std::vector<FilamentInfo> Job::filament_infos() const
{
    std::vector<FilamentInfo> out;
    for (const Filament& f : filaments) {
        FilamentInfo info;
        info.id          = f.id;
        info.type        = f.type;
        info.color       = f.color;
        info.filament_id = f.filament_id;
        info.used_m = info.used_g = 0.f;
        info.tray_id     = -1;
        info.distance    = 0.f;
        out.push_back(info);
    }
    return out;
}

const Filament* Job::find(int id) const
{
    for (const Filament& f : filaments)
        if (f.id == id) return &f;
    return nullptr;
}

static std::vector<double> parse_diameters(const std::string& text)
{
    std::vector<double> out;
    std::vector<std::string> parts;
    boost::split(parts, text, boost::is_any_of(", "), boost::token_compress_on);
    for (const std::string& p : parts) {
        if (p.empty()) continue;
        try { out.push_back(std::stod(p)); } catch (...) { return {}; }
    }
    return out;
}

// Metadata/filament_sequence.json, {"plate_<n>": {"sequence": [1-based ids], ...}}: the filament
// change order the slice produced - what the desktop's nozzle mapping request reads off the live
// G-code result (filament_change_sequence).
static std::vector<int> read_filament_sequence(const std::string& path, int plate)
{
    std::vector<int> out;
    mz_zip_archive   zip;
    mz_zip_zero_struct(&zip);
    if (!open_zip_reader(&zip, path)) return out;
    try {
        const int idx = mz_zip_reader_locate_file(&zip, "Metadata/filament_sequence.json", nullptr, 0);
        if (idx >= 0) {
            size_t size = 0;
            void*  data = mz_zip_reader_extract_to_heap(&zip, (mz_uint) idx, &size, 0);
            if (data) {
                const std::string text(static_cast<const char*>(data), size);
                mz_free(data);
                const json j = json::parse(text, nullptr, false);
                const std::string key = "plate_" + std::to_string(plate);
                if (j.is_object() && j.contains(key) && j[key].is_object()) {
                    const json& p = j[key];
                    const json  seq = p.contains("sequence") ? p["sequence"] : p.value("filament_sequence", json::array());
                    if (seq.is_array())
                        for (const json& v : seq)
                            if (v.is_number_integer() && v.get<int>() >= 1) out.push_back(v.get<int>() - 1);
                }
            }
        }
    } catch (...) {
        out.clear();
    }
    close_zip_reader(&zip);
    return out;
}

Job load_job(const std::string& path)
{
    Job job;
    std::string data;
    {
        boost::nowide::ifstream f(path.c_str(), std::ios::binary);
        if (!f) { job.error = "the archived file cannot be read"; return job; }
        std::stringstream ss;
        ss << f.rdbuf();
        data = ss.str();
    }
    DynamicPrintConfig config;
    Model              model;
    PlateDataPtrs      plates;
    Semver             version;
    bool               ok = false;
    try {
        std::istringstream is(data);
        ok = load_gcode_3mf_from_stream(is, &config, &model, &plates, &version);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "BambuReprint: reading " << path << " failed: " << e.what();
        ok = false;
    }
    struct Release { PlateDataPtrs& p; ~Release() { release_PlateData_list(p); } } release { plates };
    if (!ok || plates.empty()) { job.error = "the archived file is not a sliced Bambu job (.gcode.3mf) this slicer can read"; return job; }

    // The plate that carries G-code. A file the send dialog exported holds exactly one; a file
    // with several (an "all plates" export) cannot be replayed as one print from here.
    PlateData* plate = nullptr;
    int        with_gcode = 0;
    for (PlateData* p : plates)
        if (p && !p->gcode_file.empty()) { if (!plate) plate = p; ++with_gcode; }
    if (!plate) { job.error = "the archived file holds no G-code; slice the plate again and send it from the PC"; return job; }
    if (with_gcode > 1) { job.error = "the archived file holds G-code for several plates; send it from the PC's storage browser"; return job; }

    job.plate            = plate->plate_index + 1;
    job.printer_model_id = plate->printer_model_id;
    if (auto* pm = config.option<ConfigOptionString>("printer_model")) job.printer_model = pm->value;
    job.nozzle_diameters = parse_diameters(plate->nozzle_diameters);
    if (job.nozzle_diameters.empty())
        if (auto* nd = config.option<ConfigOptionFloats>("nozzle_diameter")) job.nozzle_diameters = nd->values;
    if (auto* nvt = config.option<ConfigOptionEnumsGeneric>("nozzle_volume_type"))
        for (size_t i = 0; i < nvt->size(); ++i) job.nozzle_volume_types.push_back(nvt->get_at(i));
    if (auto* pem = config.option<ConfigOptionInts>("physical_extruder_map")) job.physical_extruder_map = pem->values;

    if (auto* colours = config.option<ConfigOptionStrings>("filament_colour")) job.project_filament_count = colours->size();
    if (auto* ids = config.option<ConfigOptionStrings>("filament_ids")) job.filament_ids = ids->values;
    job.project_filament_count = std::max(job.project_filament_count, job.filament_ids.size());

    // The grouping the plate was sliced with: slice_info's per-plate filament_maps is the truth
    // (a plate may override the project's filament_map), the project's is the fallback.
    if (job.dual()) {
        std::vector<int> fm = plate->filament_maps;
        if (fm.empty())
            if (auto* pfm = config.option<ConfigOptionInts>("filament_map")) fm = pfm->values;
        job.filament_map = fm;
    }

    for (const FilamentInfo& fi : plate->slice_filaments_info) {
        Filament f;
        f.id          = fi.id;
        f.type        = fi.type;
        f.color       = colour8(fi.color.empty() ? std::string("#FFFFFF") : fi.color);
        f.filament_id = fi.filament_id;
        f.used_g      = fi.used_g;
        f.group_ids   = fi.group_id;
        if (f.filament_id.empty() && f.id >= 0 && f.id < (int) job.filament_ids.size()) f.filament_id = job.filament_ids[f.id];
        if (job.dual() && f.id >= 0 && f.id < (int) job.filament_map.size()) f.extruder = job.filament_map[f.id] - 1;
        job.project_filament_count = std::max(job.project_filament_count, (size_t) f.id + 1);
        job.filaments.push_back(f);
    }
    std::sort(job.filaments.begin(), job.filaments.end(), [](const Filament& a, const Filament& b) { return a.id < b.id; });
    if (job.filaments.empty()) { job.error = "the archived file does not say which filaments it prints with"; return job; }
    if (job.dual())
        for (const Filament& f : job.filaments)
            if (f.extruder != 0 && f.extruder != 1) {
                job.error = "the archived file does not say which nozzle filament " + std::to_string(f.id + 1) + " was sliced for";
                return job;
            }

    // (filament, nozzle) pairs: <filament group_id="..."> names the logical nozzles, <nozzle id=..
    // extruder_id=..> says which extruder each of them is on - what the desktop reads off the
    // plate's nozzle group result.
    for (const Filament& f : job.filaments)
        for (int g : f.group_ids)
            for (const MultiNozzleUtils::NozzleInfo& nz : plate->nozzles_info)
                if (nz.group_id == g)
                    job.filament_nozzles.push_back({ f.id, nz.extruder_id, nz.group_id, nz.diameter, nz.volume_type });

    // The bed: the plate's own type when it has one, else the project's - bed_type_to_gcode_string
    // is exactly what PrintJob sends (task_bed_type).
    BedType bed = btDefault;
    if (auto* pb = plate->config.option<ConfigOptionEnum<BedType>>("curr_bed_type")) bed = pb->value;
    if (bed == btDefault)
        if (auto* cb = config.option<ConfigOptionEnum<BedType>>("curr_bed_type")) bed = cb->value;
    job.bed_type = bed_type_to_gcode_string(bed);

    for (const auto& w : plate->warnings)
        if (w.msg == NOT_GENERATE_TIMELAPSE) job.timelapse_warning = true;
    try { job.prediction_s = (int) std::stod(plate->gcode_prediction); } catch (...) {}
    try { job.weight_g = std::stod(plate->gcode_weight); } catch (...) {}

    job.filament_change_sequence = read_filament_sequence(path, job.plate);
    return job;
}

// ---------------------------------------------------------------------- the trays ----

std::vector<Tray> trays_of(MachineObject* obj, const std::vector<int>& physical_extruder_map)
{
    std::vector<Tray> out;
    if (!obj) return out;
    const bool dual = obj->is_multi_extruders();
    for (const auto& kv : obj->amsList) {
        const Ams* a = kv.second;
        if (!a) continue;
        int ams_id = -1;
        try { ams_id = std::stoi(kv.first); } catch (...) { continue; }
        if (BambuExtruderMap::is_external_spool_ams_id(ams_id)) continue;
        for (const auto& tk : a->trayList) {
            AmsTray* t = tk.second;
            if (!t) continue;
            Tray tr;
            tr.ams_id = ams_id;
            try { tr.slot_id = std::stoi(tk.first); } catch (...) { continue; }
            tr.tray_id           = ams_id * 4 + tr.slot_id;
            tr.physical_extruder = a->nozzle;
            tr.extruder          = dual ? BambuExtruderMap::physical_to_logical(physical_extruder_map, a->nozzle) : -1;
            tr.name              = BambuExtruderMap::tray_name(ams_id, tr.slot_id);
            tr.exists            = t->is_exists;
            tr.ready             = t->is_exists && t->is_tray_info_ready();
            if (tr.ready) {
                tr.type         = t->get_filament_type();
                tr.display_type = t->get_display_filament_type();
                tr.color        = t->color;
                tr.colors       = t->cols;
                tr.ctype        = t->ctype;
                tr.filament_id  = t->setting_id;
            }
            out.push_back(tr);
        }
    }
    std::sort(out.begin(), out.end(), [](const Tray& a, const Tray& b) { return a.tray_id < b.tray_id; });
    return out;
}

// ------------------------------------------------------------------ the overrides ----

bool parse_choices(const std::string& text, std::vector<Choice>& out, std::string& error)
{
    out.clear();
    std::vector<std::string> items;
    boost::split(items, text, boost::is_any_of(","));
    for (std::string item : items) {
        boost::trim(item);
        if (item.empty()) continue;
        const size_t colon = item.find(':');
        const size_t dash  = colon == std::string::npos ? std::string::npos : item.find('-', colon + 1);
        if (colon == std::string::npos || dash == std::string::npos) {
            error = "ams_mapping wants <filament>:<ams>-<slot> items, not '" + item + "'";
            return false;
        }
        Choice c;
        try {
            size_t used = 0;
            const std::string f = item.substr(0, colon), a = item.substr(colon + 1, dash - colon - 1), s = item.substr(dash + 1);
            c.filament = std::stoi(f, &used);
            if (used != f.size()) throw std::invalid_argument(f);
            c.ams_id = std::stoi(a, &used);
            if (used != a.size()) throw std::invalid_argument(a);
            c.slot_id = std::stoi(s, &used);
            if (used != s.size()) throw std::invalid_argument(s);
        } catch (...) {
            error = "ams_mapping wants numbers, not '" + item + "'";
            return false;
        }
        if (c.filament < 0 || c.filament > 255 || c.ams_id < 0 || c.ams_id > 255 || c.slot_id < 0 || c.slot_id > 15) {
            error = "ams_mapping item '" + item + "' is out of range";
            return false;
        }
        for (const Choice& o : out)
            if (o.filament == c.filament) {
                error = "ams_mapping names filament " + std::to_string(c.filament) + " twice";
                return false;
            }
        out.push_back(c);
    }
    return true;
}

std::string choices_text(const std::vector<FilamentInfo>& result)
{
    std::string out;
    for (const FilamentInfo& r : result) {
        if (r.tray_id < 0 || r.ams_id.empty() || r.slot_id.empty()) continue;
        if (!out.empty()) out += ",";
        out += std::to_string(r.id) + ":" + r.ams_id + "-" + r.slot_id;
    }
    return out;
}

bool apply_choices(const Job& job, const std::vector<Tray>& trays, const std::vector<Choice>& choices,
                   std::vector<FilamentInfo>& result, std::string& error)
{
    for (const Choice& c : choices) {
        const Filament* f = job.find(c.filament);
        if (!f) {
            error = "this job does not print with filament " + std::to_string(c.filament + 1);
            return false;
        }
        const Tray* tray = nullptr;
        for (const Tray& t : trays)
            if (t.ams_id == c.ams_id && t.slot_id == c.slot_id) tray = &t;
        if (!tray) {
            error = "the printer has no AMS slot " + BambuExtruderMap::tray_name(c.ams_id, c.slot_id);
            return false;
        }
        if (!tray->ready) {
            error = "AMS slot " + tray->name + (tray->exists ? " holds a spool the printer has not identified" : " is empty");
            return false;
        }
        FilamentInfo* r = nullptr;
        for (FilamentInfo& x : result)
            if (x.id == c.filament) r = &x;
        if (!r) {
            FilamentInfo add;
            add.id = c.filament;
            result.push_back(add);
            r = &result.back();
        }
        // The slot the automatic mapping already picked: keep its entry as it is, so a phone that
        // sends the proposal back unchanged sends exactly what the desktop's dialog would.
        if (r->tray_id == tray->tray_id && r->ams_id == std::to_string(tray->ams_id) && r->slot_id == std::to_string(tray->slot_id))
            continue;
        // SelectMachineDialog::on_select_ams: tray id, its colour, its colours, type and ams/slot.
        r->tray_id     = tray->tray_id;
        r->color       = colour8(tray->color);
        r->colors      = tray->colors;
        r->ctype       = tray->ctype;
        r->type        = tray->type;
        r->filament_id = tray->filament_id;
        r->ams_id      = std::to_string(tray->ams_id);
        r->slot_id     = std::to_string(tray->slot_id);
        r->distance    = 0.f;
    }
    std::sort(result.begin(), result.end(), [](const FilamentInfo& a, const FilamentInfo& b) { return a.id < b.id; });
    return true;
}

std::string side_word(int extruder) { return extruder == 0 ? "left" : extruder == 1 ? "right" : ""; }

// A colour name good enough for a sentence; the phone shows the swatch itself.
static std::string colour_word(const std::string& c)
{
    unsigned char rgba[4] = { 0, 0, 0, 255 };
    if (!BitmapCache::parse_color4(colour8(c), rgba)) return c;
    const int r = rgba[0], g = rgba[1], b = rgba[2];
    const int mx = std::max({ r, g, b }), mn = std::min({ r, g, b });
    if (mx < 50) return "black";
    if (mn > 215) return "white";
    if (mx - mn < 25) return "grey";
    char buf[16];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", r, g, b);
    return buf;
}

std::string filament_label(const Filament& f)
{
    return "Filament " + std::to_string(f.id + 1) + " (" + (f.type.empty() ? std::string("unknown type") : f.type) + ", " +
           colour_word(f.color) + ")";
}

std::vector<Problem> check(const Job& job, const std::vector<Tray>& trays, const std::vector<FilamentInfo>& result)
{
    std::vector<Problem> out;
    for (const Filament& f : job.filaments) {
        const FilamentInfo* r = nullptr;
        for (const FilamentInfo& x : result)
            if (x.id == f.id) r = &x;
        const std::string side = side_word(f.extruder);
        if (!r || r->tray_id < 0) {
            bool type_there = false;
            for (const Tray& t : trays)
                if (t.ready && t.type == f.type && (f.extruder < 0 || t.extruder == f.extruder)) type_there = true;
            out.push_back({ f.id, "unmapped",
                            filament_label(f) + " has no AMS slot: " +
                                (type_there ? std::string("pick one of the matching slots")
                                            : "no " + f.type + " is loaded" + (side.empty() ? std::string() : " in an AMS on the " + side + " side")) });
            continue;
        }
        const Tray* tray = nullptr;
        for (const Tray& t : trays)
            if (t.tray_id == r->tray_id) tray = &t;
        const std::string slot = tray ? tray->name : ("slot " + std::to_string(r->tray_id));
        const std::string held = tray ? tray->type : r->type;
        if (!held.empty() && held != f.type)
            out.push_back({ f.id, "type", filament_label(f) + " is " + f.type + ", but AMS slot " + slot + " holds " + held });
        if (f.extruder >= 0 && tray && tray->extruder >= 0 && tray->extruder != f.extruder)
            out.push_back({ f.id, "side", filament_label(f) + " was sliced for the " + side + " nozzle, but AMS slot " + slot +
                                              " feeds the " + side_word(tray->extruder) + " one" });
    }
    return out;
}

// -------------------------------------------------------------------- evaluation ----

static bool flag(int requested, bool def) { return requested < 0 ? def : requested != 0; }

static std::string model_name(const std::string& code)
{
    const std::string n = GcodeArchive::canonical_model(code, GcodeArchive::model_names());
    return n.empty() ? code : n;
}

static std::string diameter_text(double d)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2g mm", d);
    return buf;
}

Evaluation evaluate(const Printer& pr, const Job& job, const Request& req, const Env& env, const std::vector<FilamentInfo>& automatic)
{
    Evaluation ev;
    auto refuse = [&ev](int status, const std::string& text) {
        ev.status = status;
        ev.error  = text;
        return ev;
    };
    const std::string& name = pr.name.empty() ? pr.id : pr.name;
    if (!job.error.empty()) return refuse(409, job.error);

    // --- the printer: reachable, idle, able to take a file (SelectMachineDialog::update_show_status)
    if (!pr.online) return refuse(409, name + " is offline");
    if (pr.lan_mode && !pr.access_code_known)
        return refuse(409, name + " needs its access code entered on the PC first (Device tab)");
    // EdgeSlicer's network plug-in prints over the printer's LAN (FTPS upload + MQTT start); a
    // printer the PC only knows through the Bambu cloud has no route for that.
    if (!pr.lan_route)
        return refuse(409, name + " can only be reached through the Bambu cloud from this PC; enter its access code on the PC's "
                                  "Device tab so it can be printed on over the local network");
    if (pr.upgrading) return refuse(409, name + " is updating its firmware");
    if (pr.system_printing) return refuse(409, name + " is busy with a calibration or maintenance task");
    if (pr.printing || pr.filament_changing)
        return refuse(409, name + " is busy printing" + (pr.printing_what.empty() ? std::string() : " (" + pr.printing_what + ")"));

    // --- the model (SelectMachineDialog::is_blocking_printing: the file's printer_model_id)
    if (!job.printer_model_id.empty() && !pr.model.empty() && job.printer_model_id != pr.model &&
        std::find(pr.compatible_models.begin(), pr.compatible_models.end(), job.printer_model_id) == pr.compatible_models.end())
        return refuse(409, "this job was sliced for a " + model_name(job.printer_model_id) + ", and " + name + " is a " +
                               model_name(pr.model) + "; a sliced file only prints on the model it was sliced for");
    if (pr.extruder_count > 0 && job.dual() != (pr.extruder_count > 1))
        return refuse(409, "this job was sliced for a " + std::string(job.dual() ? "two" : "one") + "-nozzle printer and " + name + " has " +
                               (pr.extruder_count > 1 ? "two" : "one"));

    // --- the nozzles: diameter per extruder, and hardness (the dialog's confirm-before-send checks)
    const size_t extruders = job.dual() ? 2 : 1;
    if (!req.force)
        for (size_t e = 0; e < extruders && e < job.nozzle_diameters.size(); ++e) {
            bool used = !job.dual();
            for (const Filament& f : job.filaments) used = used || f.extruder == (int) e;
            if (!used || e >= pr.diameters.size() || pr.diameters[e].empty()) continue;
            const double want  = job.nozzle_diameters[e];
            bool         match = false;
            for (double d : pr.diameters[e]) match = match || std::fabs(d - want) < 0.01;
            if (!match)
                return refuse(409, "this job was sliced for a " + diameter_text(want) + " nozzle" +
                                       (job.dual() ? " on the " + side_word((int) e) + " extruder" : std::string()) + ", and " + name +
                                       " has " + diameter_text(pr.diameters[e].front()) + "; change the nozzle, or re-slice for it");
        }
    if (!req.force && env.required_hrc)
        for (const Filament& f : job.filaments) {
            const size_t logical = f.extruder < 0 ? 0 : (size_t) f.extruder;
            if (logical >= pr.nozzle_hrc.size() || pr.nozzle_hrc[logical] == 0) continue; // nozzle type not reported
            if (std::abs(env.required_hrc(f.type)) > std::abs(pr.nozzle_hrc[logical]))
                return refuse(409, "printing " + f.type + " with the " + (job.dual() ? side_word((int) logical) + " " : std::string()) +
                                       "nozzle of " + name + " may damage it (the filament needs a hardened nozzle)");
        }

    // --- storage: a LAN print is uploaded to the printer's SD card / internal storage first
    ev.has_sdcard = pr.sdcard == 1; // MachineObject::HAS_SDCARD_NORMAL
    if (pr.sdcard == 0) return refuse(409, name + " has no SD card or storage; a print over the LAN is uploaded to it first");
    if (!ev.has_sdcard)
        return refuse(409, name + "'s storage is " + (pr.sdcard == 3 ? "read-only" : "not usable") + "; check the SD card on the printer");

    // --- options, as the send dialog defaults them (and remembers them in AppConfig "print")
    auto remembered   = [&env](const char* key) { return env.remembered ? env.remembered(key) : true; };
    ev.bed_leveling   = flag(req.bed_leveling, remembered("bed_leveling"));
    ev.flow_cali      = flag(req.flow_cali, remembered("flow_cali"));
    // A slice that could not make a timelapse turns the option off and greys it out; an i3 (A1)
    // starts with it off.
    ev.timelapse      = job.timelapse_warning ? false : flag(req.timelapse, !pr.i3 && remembered("timelapse"));
    ev.use_ams        = pr.has_ams && flag(req.use_ams, true);
    ev.auto_offset_cali = job.dual() && flag(req.nozzle_offset_cali, remembered("nozzle_offset_cali")) ? 2 : 0;
    ev.nozzles_info   = BambuSendMapping::nozzles_info(job.nozzle_diameters, job.nozzle_volume_types);

    // --- the mapping
    const std::vector<FilamentInfo> filaments = job.filament_infos();
    std::vector<Problem>            problems;
    if (ev.use_ams && pr.ams_mapping_supported) {
        ev.result = automatic;
        for (const Filament& f : job.filaments) { // every job filament has an entry, mapped or not
            bool there = false;
            for (const FilamentInfo& r : ev.result) there = there || r.id == f.id;
            if (!there) {
                FilamentInfo add;
                add.id      = f.id;
                add.tray_id = -1;
                ev.result.push_back(add);
            }
        }
        if (!req.mapping.empty()) {
            std::vector<Choice> choices;
            std::string         error;
            if (!parse_choices(req.mapping, choices, error) || !apply_choices(job, pr.trays, choices, ev.result, error))
                return refuse(400, error);
        }
        std::sort(ev.result.begin(), ev.result.end(), [](const FilamentInfo& a, const FilamentInfo& b) { return a.id < b.id; });
        problems = check(job, pr.trays, ev.result);
        BambuSendMapping::ComposeInput in;
        in.project_filament_count = job.project_filament_count;
        in.filament_ids           = job.filament_ids;
        if (job.dual()) in.nozzle_filament_map = job.filament_map;
        if (problems.empty())
            BambuSendMapping::compose(ev.result, filaments, in, ev.ams_mapping, ev.ams_mapping2, ev.ams_mapping_info);
    } else if (!ev.use_ams && !filaments.empty()) {
        // SelectMachineDialog::on_ok_btn with use_ams off: only the first filament, no trays.
        json a = json::array(), it;
        it["sourceColor"]  = filaments[0].color.size() >= 9 ? filaments[0].color.substr(1, 8) : filaments[0].color;
        it["filamentType"] = filaments[0].type;
        a.push_back(it);
        ev.ams_mapping_info = a.dump();
    }

    // --- the H2C nozzle-mapping handshake request (SelectMachineDialog::build_nozzle_mapping_request, V0)
    {
        BambuNozzleMapping::QueryConditions qc;
        qc.sliced_send        = req.mode == "print";
        qc.dual_nozzle_preset = job.dual();
        qc.printer_has_rack   = pr.nozzle_rack;
        bool right_used       = false;
        for (const Filament& f : job.filaments) right_used = right_used || f.extruder == 1;
        qc.right_nozzle_used  = right_used;
        qc.has_ams_mapping    = ev.use_ams && !ev.result.empty();
        if (problems.empty() && BambuNozzleMapping::query_applies(qc)) {
            BambuNozzleMapping::RequestInput rin;
            rin.calibration              = ev.flow_cali ? 1 : 0;
            rin.extrude_cali_manual_mode = 1;
            rin.filament_change_sequence = job.filament_change_sequence;
            for (const FilamentInfo& r : ev.result)
                rin.mapped.push_back({ r.id, r.ams_id, r.slot_id, r.tray_id, r.filament_id, r.color });
            for (const FilamentInfo& r : ev.result)
                for (const BambuNozzleMapping::FilamentNozzle& fn : job.filament_nozzles)
                    if (fn.filament == r.id) rin.filament_nozzles.push_back(fn);
            rin.physical_extruder_map = job.physical_extruder_map;
            rin.preset_diameters      = job.nozzle_diameters;
            for (int v : job.nozzle_volume_types) rin.preset_volumes.push_back(NozzleVolumeType(v));
            rin.printer_nozzles       = pr.nozzles;
            ev.nozzle_mapping_request = BambuNozzleMapping::build_v0_request(rin);
        }
    }

    // --- the preview the phone's mapping sheet shows
    json& p = ev.preview;
    p["printer"] = { { "id", pr.id },
                     { "name", name },
                     { "model", pr.model },
                     { "model_name", model_name(pr.model) },
                     { "dual", pr.extruder_count > 1 },
                     { "has_ams", pr.has_ams },
                     { "lan_mode", pr.lan_mode },
                     { "nozzle_rack", pr.nozzle_rack } };
    json jn = json::array();
    for (double d : job.nozzle_diameters) jn.push_back(d);
    p["job"] = { { "plate", job.plate },
                 { "model", job.printer_model_id },
                 { "model_name", model_name(job.printer_model_id.empty() ? job.printer_model : job.printer_model_id) },
                 { "dual", job.dual() },
                 { "nozzle_diameters", jn },
                 { "bed_type", job.bed_type },
                 { "estimated_time_s", job.prediction_s },
                 { "estimated_weight_g", job.weight_g } };
    json jt = json::array();
    for (const Tray& t : pr.trays) {
        json o;
        o["ams_id"]     = t.ams_id;
        o["slot_id"]    = t.slot_id;
        o["id"]         = std::to_string(t.ams_id) + "-" + std::to_string(t.slot_id);
        o["name"]       = t.name;
        o["side"]       = t.extruder == 0 ? "L" : t.extruder == 1 ? "R" : "";
        o["exists"]     = t.exists;
        o["ready"]      = t.ready;
        o["type"]       = t.display_type.empty() ? t.type : t.display_type;
        o["match_type"] = t.type;
        o["colour"]     = t.ready ? colour6(t.color) : "";
        jt.push_back(o);
    }
    p["trays"] = jt;
    json jf = json::array();
    for (const Filament& f : job.filaments) {
        json o;
        o["index"]  = f.id;
        o["type"]   = f.type;
        o["colour"] = colour6(f.color);
        o["grams"]  = std::round(f.used_g * 100.0) / 100.0;
        o["side"]   = f.extruder == 0 ? "L" : f.extruder == 1 ? "R" : "";
        o["tray"]   = nullptr;
        o["auto"]   = false;
        for (const FilamentInfo& r : ev.result)
            if (r.id == f.id && r.tray_id >= 0 && !r.ams_id.empty() && !r.slot_id.empty()) {
                o["tray"] = r.ams_id + "-" + r.slot_id;
                for (const FilamentInfo& a : automatic)
                    if (a.id == f.id) o["auto"] = a.tray_id == r.tray_id;
            }
        json probs = json::array();
        for (const Problem& pb : problems)
            if (pb.filament == f.id) probs.push_back(pb.text);
        o["problems"] = probs;
        jf.push_back(o);
    }
    p["filaments"] = jf;
    p["mapping"]   = choices_text(ev.result);
    json jp = json::array();
    for (const Problem& pb : problems) jp.push_back({ { "filament", pb.filament }, { "code", pb.code }, { "text", pb.text } });
    p["problems"] = jp;
    p["options"]  = { { "bed_leveling", { { "value", ev.bed_leveling }, { "shown", true } } },
                      { "flow_cali", { { "value", ev.flow_cali }, { "shown", true } } },
                      { "timelapse", { { "value", ev.timelapse }, { "shown", true }, { "enabled", !job.timelapse_warning } } },
                      { "use_ams", { { "value", ev.use_ams }, { "shown", pr.has_ams } } },
                      { "nozzle_offset_cali", { { "value", ev.auto_offset_cali != 0 }, { "shown", job.dual() } } } };
    json warnings = json::array();
    if (job.timelapse_warning) warnings.push_back("This slice cannot record a timelapse (spiral vase or by-object printing).");
    if (!ev.use_ams && job.filaments.size() > 1)
        warnings.push_back("With the AMS off the printer feeds every filament from the external spool.");
    p["warnings"]       = warnings;
    p["nozzle_mapping"] = { { "applies", !ev.nozzle_mapping_request.empty() } };
    ev.can_send         = problems.empty();
    p["can_send"]       = ev.can_send;
    return ev;
}

// ------------------------------------------------------------- the live printer ----

// The nozzle diameters the printer has on each logical extruder: the extruder nozzle, and on an
// H2C the rack nozzles too (they belong to the main extruder). Empty when it reports none.
static std::vector<std::vector<double>> printer_diameters(MachineObject* obj, const std::vector<int>& pem, size_t extruders)
{
    std::vector<std::vector<double>> out(extruders);
    for (const Nozzle& n : obj->m_nozzle_data.nozzles) {
        if (!n.is_normal()) continue;
        const int physical = n.on_rack() ? 0 : (n.id & 0xF);
        const int logical  = extruders > 1 ? BambuExtruderMap::physical_to_logical(pem, physical) : 0;
        if (logical >= 0 && logical < (int) extruders && n.diameter > 0.f) out[logical].push_back(n.diameter);
    }
    bool any = false;
    for (const auto& v : out) any = any || !v.empty();
    if (any) return out;
    // No nozzle report: the extruder's current nozzle, when the printer said what it is (the send
    // dialog makes the same check against extders[0], and only when the type is known).
    for (size_t logical = 0; logical < extruders; ++logical) {
        const int physical = extruders > 1 ? BambuExtruderMap::logical_to_physical(pem, (int) logical) : 0;
        if (physical >= 0 && physical < (int) obj->m_extder_data.extders.size()) {
            const Extder& e = obj->m_extder_data.extders[physical];
            if (e.current_nozzle_type != ntUndefine && e.current_nozzle_diameter > 0.f) out[logical].push_back(e.current_nozzle_diameter);
        }
    }
    return out;
}

Printer snapshot(MachineObject* obj, const Job& job)
{
    Printer pr;
    if (!obj) return pr;
    pr.id                = obj->dev_id;
    pr.name              = obj->dev_name;
    pr.model             = obj->printer_type;
    pr.online            = obj->is_online();
    pr.lan_mode          = obj->is_lan_mode_printer();
    pr.access_code_known = obj->has_access_right();
    pr.lan_route         = !obj->dev_ip.empty() && !obj->get_access_code().empty();
    pr.upgrading         = obj->is_in_upgrading();
    pr.system_printing   = obj->is_system_printing();
    pr.printing          = obj->is_in_printing();
    pr.printing_what     = obj->subtask_name;
    pr.filament_changing = obj->ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE;
    pr.extruder_count    = obj->m_extder_data.total_extder_count;
    pr.has_ams           = obj->has_ams();
    pr.ams_mapping_supported = obj->is_support_ams_mapping();
    pr.nozzle_rack       = obj->has_nozzle_rack();
    pr.i3                = obj->get_printer_arch() == PrinterArch::ARCH_I3;
    pr.sdcard            = (int) obj->get_sdcard_state();
    if (!pr.model.empty()) pr.compatible_models = DeviceManager::get_compatible_machine(pr.model);
    const size_t extruders = job.dual() ? 2 : 1;
    pr.diameters = printer_diameters(obj, job.physical_extruder_map, extruders);
    for (size_t logical = 0; logical < extruders; ++logical) {
        const int physical = extruders > 1 ? BambuExtruderMap::logical_to_physical(job.physical_extruder_map, (int) logical) : 0;
        int       hrc      = 0;
        if (physical >= 0 && physical < (int) obj->m_extder_data.extders.size()) {
            const Extder& e = obj->m_extder_data.extders[physical];
            if (e.current_nozzle_type != ntUndefine) hrc = Print::get_hrc_by_nozzle_type(e.current_nozzle_type);
        }
        pr.nozzle_hrc.push_back(hrc);
    }
    pr.trays   = trays_of(obj, job.physical_extruder_map);
    pr.nozzles = DualNozzle::printer_state(obj).nozzles;
    return pr;
}

Env app_env()
{
    Env env;
    // The send dialog remembers its checkboxes in AppConfig section "print": unset or "1" means on.
    env.remembered = [](const char* key) {
        AppConfig* cfg = wxGetApp().app_config;
        return !(cfg && cfg->get("print", key) == "0");
    };
    env.required_hrc = [](const std::string& type) {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        return bundle ? bundle->get_required_hrc_by_filament_type(type) : 0;
    };
    return env;
}

Evaluation evaluate(MachineObject* obj, const Job& job, const Request& req)
{
    if (!obj) {
        Evaluation ev;
        ev.status = 404;
        ev.error  = "no such printer";
        return ev;
    }
    // The send dialog's automatic mapping, on the live printer (MachineObject::ams_filament_mapping
    // per side, BambuSendMapping::auto_map); SelectMachineDialog::do_ams_mapping resets an unusable
    // result rather than half using it.
    std::vector<FilamentInfo> automatic;
    if (job.error.empty() && obj->has_ams() && obj->is_support_ams_mapping()) {
        const int rc = BambuSendMapping::auto_map(obj, job.filament_infos(), job.filament_map, job.physical_extruder_map, automatic);
        if (rc != 0 && rc != 1 && !obj->is_valid_mapping_result(automatic))
            for (FilamentInfo& r : automatic) { r.tray_id = -1; r.distance = 99999; }
    }
    return evaluate(snapshot(obj, job), job, req, app_env(), automatic);
}

} // namespace BambuReprint
} // namespace GUI
} // namespace Slic3r
