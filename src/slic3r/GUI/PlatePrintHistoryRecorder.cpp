#include "PlatePrintHistoryRecorder.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <map>

#include <wx/app.h>

namespace Slic3r { namespace GUI { namespace PlateHistoryRecorder {

std::string connection_key_for_host(const std::string &host_name)
{
    std::string out;
    for (char c : host_name) {
        if (std::isalnum(static_cast<unsigned char>(c)))
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!out.empty() && out.back() != '_')
            out += '_';
    }
    while (!out.empty() && out.back() == '_')
        out.pop_back();
    return out;
}

namespace {

// The plates a Send names, resolved against the current project.
std::vector<int> resolve_plates(const std::vector<int> &wanted, PartPlateList &plates)
{
    const int        count = plates.get_plate_count();
    std::vector<int> out;
    auto             add = [&](int i) {
        if (i >= 0 && i < count && std::find(out.begin(), out.end(), i) == out.end())
            out.push_back(i);
    };
    if (wanted.empty())
        add(plates.get_curr_plate_index());
    for (int w : wanted) {
        if (w == PLATE_ALL_IDX) {
            for (int i = 0; i < count; ++i)
                add(i);
        } else if (w == PLATE_CURRENT_IDX) {
            add(plates.get_curr_plate_index());
        } else {
            add(w);
        }
    }
    return out;
}

// Estimated print time and filament weight of a sliced plate (0 = unknown).
void fill_estimate(PartPlate &plate, PlateHistory::Entry &e)
{
    if (!plate.is_slice_result_valid() || plate.get_slice_result() == nullptr)
        return;
    const auto &stats = plate.get_slice_result()->print_statistics;
    if (!stats.modes.empty() && stats.modes.front().time > 0.f)
        e.time_estimate_s = static_cast<int>(stats.modes.front().time);
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return;
    const DynamicPrintConfig full = bundle->full_config();
    std::vector<double>      density;
    if (auto *d = full.option<ConfigOptionFloats>("filament_density"))
        density = d->values;
    double grams = 0.0;
    for (const auto &kv : stats.total_volumes_per_extruder) {
        const double d = kv.first < density.size() ? density[kv.first] : 1.24;
        grams += kv.second / 1000.0 * d;
    }
    e.filament_g = grams;
}

} // namespace

std::vector<std::pair<int, std::string>> record_now(const Send &send)
{
    std::vector<std::pair<int, std::string>> done;
    try {
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr)
            return done;
        PartPlateList &plates = plater->get_partplate_list();
        for (int index : resolve_plates(send.plates, plates)) {
            PartPlate *plate = plates.get_plate(index);
            if (plate == nullptr)
                continue;
            PlateHistory::Entry e;
            e.printer_name  = send.printer_name;
            e.printer_model = send.printer_model;
            e.connection    = send.connection;
            e.file_name     = send.file_name;
            e.action        = send.action;
            e.uid           = send.uid;
            fill_estimate(*plate, e);
            const std::string uid = plate->add_print_history_entry(std::move(e));
            if (!uid.empty())
                done.emplace_back(index, uid);
        }
        if (!done.empty())
            if (auto *canvas = plater->get_current_canvas3D())
                canvas->set_as_dirty();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "PlateHistoryRecorder: " << ex.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(warning) << "PlateHistoryRecorder: unknown error";
    }
    return done;
}

void record(const Send &send)
{
    if (wxIsMainThread()) {
        record_now(send);
        return;
    }
    if (!wxTheApp)
        return;
    wxGetApp().CallAfter([send]() { record_now(send); });
}

void record_export(int plate, const std::string &file_path)
{
    Send s;
    s.plates     = {plate};
    s.connection = "file";
    s.file_name  = boost::filesystem::path(file_path).filename().string();
    s.action     = PlateHistory::Action::Exported;
    record(s);
}

void upgrade(int plate, const std::string &uid, PlateHistory::Action action)
{
    auto work = [plate, uid, action]() {
        try {
            Plater *plater = wxGetApp().plater();
            if (plater == nullptr)
                return;
            if (PartPlate *p = plater->get_partplate_list().get_plate(plate))
                p->set_print_history_action(uid, action);
        } catch (...) {
        }
    };
    if (wxIsMainThread())
        work();
    else if (wxTheApp)
        wxGetApp().CallAfter(work);
}

void bambu_identity(const std::string &dev_id, std::string &name, std::string &model)
{
    auto result = std::make_shared<std::pair<std::string, std::string>>(dev_id, std::string());
    auto work   = [result, dev_id]() {
        DeviceManager *dm = wxGetApp().getDeviceManager();
        if (!dm)
            return;
        std::map<std::string, MachineObject *> all = dm->get_my_machine_list();
        for (const auto &kv : dm->get_local_machine_list())
            all.insert(kv);
        for (const auto &kv : all) {
            MachineObject *m = kv.second;
            if (m && m->dev_id == dev_id) {
                if (!m->dev_name.empty())
                    result->first = m->dev_name;
                result->second = MachineObject::get_preset_printer_model_name(m->printer_type);
                return;
            }
        }
    };
    if (wxIsMainThread()) {
        try { work(); } catch (...) {}
    } else if (wxTheApp) {
        auto done = std::make_shared<std::promise<void>>();
        auto fut  = done->get_future();
        wxGetApp().CallAfter([done, work]() {
            try { work(); } catch (...) {}
            done->set_value();
        });
        fut.wait_for(std::chrono::seconds(5));
    }
    name  = result->first;
    model = result->second;
}

}}} // namespace Slic3r::GUI::PlateHistoryRecorder
