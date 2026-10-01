#include "PrintHost.hpp"

#include <atomic>
#include <climits>
#include <cstdint>
#include <vector>
#include <thread>
#include <exception>
#include <boost/algorithm/string.hpp>
#include <boost/optional.hpp>
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>

#include <wx/string.h>
#include <wx/app.h>
#include <wx/arrstr.h>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Channel.hpp"
#include "MoonRaker.hpp"
#include "OctoPrint.hpp"
#include "Duet.hpp"
#include "FlashAir.hpp"
#include "AstroBox.hpp"
#include "Repetier.hpp"
#include "MKS.hpp"
#include "ESP3D.hpp"
#include "CrealityPrint.hpp"
#include "../GUI/PrintHostDialogs.hpp"
#include "../GUI/MainFrame.hpp"
#include "../GUI/GcodeArchive.hpp"
#include "../GUI/SnapmakerLan.hpp" // Ultra: END_UNLOAD_FILAMENT, built in one place for every send path
#include "Obico.hpp"
#include "Flashforge.hpp"
#include "SimplyPrint.hpp"
#include "ElegooLink.hpp"

namespace fs = boost::filesystem;
using boost::optional;
using Slic3r::GUI::PrintHostQueueDialog;

namespace Slic3r {


PrintHost::~PrintHost() {}

void PrintHost::set_tls_config(const DynamicPrintConfig *config)
{
    m_tls_cafile        = config != nullptr && config->has("printhost_cafile") ? config->opt_string("printhost_cafile") : std::string();
    m_tls_ignore_revoke = config != nullptr && config->has("printhost_ssl_ignore_revoke") && config->opt_bool("printhost_ssl_ignore_revoke");
}

Http &PrintHost::apply_tls(Http &http) const
{
    http.tls_policy(tls_policy_without_ca());
    if (!m_tls_cafile.empty())
        http.ca_file(m_tls_cafile); // verifies this host against the CA file, see Http::tls_verify_for
    http.ssl_revoke_best_effort(m_tls_ignore_revoke);
    return http;
}

static PrintHost* make_print_host(DynamicPrintConfig *config, bool change_engine)
{
    PrinterTechnology tech = ptFFF;

    {
        const auto opt = config->option<ConfigOptionEnum<PrinterTechnology>>("printer_technology");
        if (opt != nullptr) {
            tech = opt->value;
        }
    }

    if (tech == ptFFF) {
        const auto opt = config->option<ConfigOptionEnum<PrintHostType>>("host_type");
        const auto host_type = opt != nullptr ? opt->value : htMoonRaker_mqtt;

        switch (host_type) {
            case htMoonRaker_mqtt: return new Moonraker_Mqtt(config, change_engine);
            case htMoonRaker: return new Moonraker(config);
            case htOctoPrint: return new OctoPrint(config);
            case htDuet:      return new Duet(config);
            case htFlashAir:  return new FlashAir(config);
            case htAstroBox:  return new AstroBox(config);
            case htRepetier:  return new Repetier(config);
            case htPrusaLink: return new PrusaLink(config);
            case htPrusaConnect: return new PrusaConnect(config);
            case htMKS:       return new MKS(config);
            case htESP3D:       return new ESP3D(config);
            case htCrealityPrint:    return new CrealityPrint(config);
            case htObico:     return new Obico(config);
            case htFlashforge: return new Flashforge(config);
            case htSimplyPrint: return new SimplyPrint(config);
            case htElegooLink: return new ElegooLink(config);
            default:          return nullptr;
        }
    } else {
        return new SL1Host(config);
    }
}

PrintHost* PrintHost::get_print_host(DynamicPrintConfig *config, bool change_engine)
{
    PrintHost *host = make_print_host(config, change_engine);
    if (host != nullptr)
        host->set_tls_config(config);
    return host;
}

namespace {

// Integers and numeric strings that fit in int. Out-of-range values (e.g. 2^32, which
// get<int>() / static_cast<int>(stol) would truncate to 0 / "success") return false.
// Booleans and floats are not accepted; see get_err_code_from_body.
bool try_parse_json_int(const nlohmann::json &value, int &out)
{
    try {
        // is_number_integer() is also true for unsigned; check unsigned first.
        if (value.is_number_unsigned()) {
            const auto u = value.get<std::uint64_t>();
            if (u > static_cast<std::uint64_t>(INT_MAX))
                return false;
            out = static_cast<int>(u);
            return true;
        }
        if (value.is_number_integer()) {
            const auto n = value.get<std::int64_t>();
            if (n < static_cast<std::int64_t>(INT_MIN) || n > static_cast<std::int64_t>(INT_MAX))
                return false;
            out = static_cast<int>(n);
            return true;
        }

        if (value.is_string()) {
            std::string text = value.get<std::string>();
            boost::trim(text);
            if (text.empty())
                return false;

            size_t          pos    = 0;
            const long long parsed = std::stoll(text, &pos, 10);
            if (pos != text.size())
                return false;
            if (parsed < INT_MIN || parsed > INT_MAX)
                return false;
            out = static_cast<int>(parsed);
            return true;
        }
    } catch (...) {}

    return false;
}

wxString wxstring_from_exception(const std::exception &e)
{
    wxString msg = wxString::FromUTF8(e.what());
    if (msg.empty())
        msg = wxString(e.what(), wxConvLibc);
    if (msg.empty())
        msg = _L("Unknown error");
    return msg;
}

} // namespace

int PrintHost::get_err_code_from_body(const std::string &body)
{
    const auto parsed = nlohmann::json::parse(body, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
        return -1;

    if (!parsed.contains("err"))
        return 0;

    int err_code = 0;
    if (!try_parse_json_int(parsed["err"], err_code))
        return -1;

    return err_code;
}

wxString PrintHost::format_error(const std::string &body, const std::string &error, unsigned status) const
{
    if (status != 0) {
        auto wxbody = wxString::FromUTF8(body.data());
        return wxString::Format("HTTP %u: %s", status, wxbody);
    } else {
        if (error.find("curl:Timeout was reached") != std::string::npos) {
            return _L("Connection timed out. Please check if the printer and computer network are functioning properly, and confirm that they are on the same network.");
        }else if(error.find("curl:Couldn't resolve host name")!= std::string::npos){
            return _L("The Hostname/IP/URL could not be parsed, please check it and try again.");
        } else if (error.find("Connection was reset") != std::string::npos){
            return _L("File/data transfer interrupted. Please check the printer and network, then try it again.");
        }
        else {
            return wxString::FromUTF8(error.data());
        }
    }
}


struct PrintHostJobQueue::priv
{
    // XXX: comment on how bg thread works

    PrintHostJobQueue *q;

    Channel<PrintHostJob> channel_jobs;
    Channel<size_t> channel_cancels;
    size_t job_id = 0;
    int prev_progress = -1;
    fs::path source_to_remove;

    std::thread bg_thread;
    bool bg_exit = false;
    // Set when error_fn runs for this job, so a later throw does not emit a second dialog.
    std::atomic<bool> error_emitted { false };

    PrintHostQueueDialog *queue_dialog;

    priv(PrintHostJobQueue *q) : q(q) {}

    void emit_progress(int progress);
    void emit_error(wxString error);
    void emit_cancel(size_t id);
    void emit_info(wxString tag, wxString status);
    void start_bg_thread();
    void stop_bg_thread();
    void bg_thread_main();
    void progress_fn(Http::Progress progress, bool &cancel);
    void error_fn(wxString error);
    void info_fn(wxString tag, wxString status);
    void remove_source(const fs::path &path);
    void remove_source();
    void perform_job(PrintHostJob the_job);
};

PrintHostJobQueue::PrintHostJobQueue(PrintHostQueueDialog *queue_dialog)
    : p(new priv(this))
{
    p->queue_dialog = queue_dialog;
}

PrintHostJobQueue::~PrintHostJobQueue()
{
    if (p) { p->stop_bg_thread(); }
}

void PrintHostJobQueue::priv::emit_progress(int progress)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_PROGRESS, queue_dialog->GetId(), job_id, progress);
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::emit_error(wxString error)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_ERROR, queue_dialog->GetId(), job_id, std::move(error));
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::emit_info(wxString tag, wxString status)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_INFO, queue_dialog->GetId(), job_id, std::move(tag), std::move(status));
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::emit_cancel(size_t id)
{
    auto evt = new PrintHostQueueDialog::Event(GUI::EVT_PRINTHOST_CANCEL, queue_dialog->GetId(), id);
    wxQueueEvent(queue_dialog, evt);
}

void PrintHostJobQueue::priv::start_bg_thread()
{
    if (bg_thread.joinable()) { return; }

    std::shared_ptr<priv> p2 = q->p;
    bg_thread = std::thread([p2]() {
        p2->bg_thread_main();
    });
}

void PrintHostJobQueue::priv::stop_bg_thread()
{
    if (bg_thread.joinable()) {
        bg_exit = true;
        channel_jobs.push(PrintHostJob()); // Push an empty job to wake up bg_thread in case it's sleeping
        bg_thread.detach();                // Let the background thread go, it should exit on its own
    }
}

void PrintHostJobQueue::priv::bg_thread_main()
{
    // bg thread entry point

    // Pick up jobs from the job channel:
    while (! bg_exit) {
        auto job = channel_jobs.pop();   // Sleeps in a cond var if there are no jobs
        if (job.empty()) {
            // This happens when the thread is being stopped
            break;
        }

        source_to_remove = job.upload_data.source_path;

        BOOST_LOG_TRIVIAL(debug) << boost::format("PrintHostJobQueue/bg_thread: Received job: [%1%]: `%2%` -> `%3%`, cancelled: %4%")
            % job_id
            % job.upload_data.upload_path
            % job.printhost->get_host()
            % job.cancelled;

        // One throwing job must not kill the worker: later sends would sit in the queue forever,
        // and remove_source() for this job would be skipped. (Orca #15947 / decision D7.)
        error_emitted.store(false);
        try {
            if (! job.cancelled) {
                perform_job(std::move(job));
            }
        } catch (const std::exception &e) {
            if (!error_emitted.load())
                emit_error(wxstring_from_exception(e));
        } catch (...) {
            if (!error_emitted.load())
                emit_error(_L("Unknown error"));
        }

        remove_source();
        job_id++;
    }

    // Cleanup leftover files, if any
    remove_source();
    auto jobs = channel_jobs.lock_rw();
    for (const PrintHostJob &job : *jobs) {
        remove_source(job.upload_data.source_path);
    }
}

void PrintHostJobQueue::priv::progress_fn(Http::Progress progress, bool &cancel)
{
    if (cancel) {
        // When cancel is true from the start, Http indicates request has been cancelled
        emit_cancel(job_id);
        return;
    }

    if (bg_exit) {
        cancel = true;
        return;
    }

    if (channel_cancels.size_hint() > 0) {
        // Lock both queues
        auto cancels = channel_cancels.lock_rw();
        auto jobs = channel_jobs.lock_rw();

        for (size_t cancel_id : *cancels) {
            if (cancel_id == job_id) {
                cancel = true;
            } else if (cancel_id > job_id) {
                const size_t idx = cancel_id - job_id - 1;
                if (idx < jobs->size()) {
                    jobs->at(idx).cancelled = true;
                    BOOST_LOG_TRIVIAL(debug) << boost::format("PrintHostJobQueue: Job id %1% cancelled") % cancel_id;
                    emit_cancel(cancel_id);
                }
            }
        }

        cancels->clear();
    }

    if (! cancel) {
        int gui_progress = progress.ultotal > 0 ? 100*progress.ulnow / progress.ultotal : 0;
        if (gui_progress != prev_progress) {
            emit_progress(gui_progress);
            prev_progress = gui_progress;
        }
    }
}

void PrintHostJobQueue::priv::error_fn(wxString error)
{
    // Remember that this job already reported, even if cancel suppresses the dialog: a throw
    // after error_fn must not emit a second one.
    error_emitted.store(true);
    // check if transfer was not canceled before error occured - than do not show the error
    bool do_emit_err = true;
    if (channel_cancels.size_hint() > 0) {
        // Lock both queues
        auto cancels = channel_cancels.lock_rw();
        auto jobs = channel_jobs.lock_rw();

        for (size_t cancel_id : *cancels) {
            if (cancel_id == job_id) {
                do_emit_err = false;
                emit_cancel(job_id);
            }
            else if (cancel_id > job_id) {
                const size_t idx = cancel_id - job_id - 1;
                if (idx < jobs->size()) {
                    jobs->at(idx).cancelled = true;
                    BOOST_LOG_TRIVIAL(debug) << boost::format("PrintHostJobQueue: Job id %1% cancelled") % cancel_id;
                    emit_cancel(cancel_id);
                }
            }
        }
        cancels->clear();
    }
    if (do_emit_err)
        emit_error(std::move(error));
}

void PrintHostJobQueue::priv::info_fn(wxString tag, wxString status)
{
    emit_info(tag, status);
}

void PrintHostJobQueue::priv::remove_source(const fs::path &path)
{
    if (! path.empty()) {
        boost::system::error_code ec;
        fs::remove(path, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << boost::format("PrintHostJobQueue: Error removing file `%1%`: %2%") % path % ec;
        }
    }
}

void PrintHostJobQueue::priv::remove_source()
{
    remove_source(source_to_remove);
    source_to_remove.clear();
}

void PrintHostJobQueue::priv::perform_job(PrintHostJob the_job)
{
    emit_progress(0);   // Indicate the upload is starting

    // Ultra: what went where, read before upload() takes the data - the queue deletes the temporary
    // source file as soon as this returns, so the archive copy has to happen here.
    const std::string archive_source = the_job.upload_data.source_path.string();
    const std::string archive_name   = the_job.upload_data.upload_path.string();
    const bool        archive_print  = the_job.upload_data.post_action == PrintHostPostUploadAction::StartPrint;
    const std::string host_name      = the_job.printhost->get_name();
    const std::string host_url       = the_job.printhost->get_host();
    // Which device of the printer model this went to, when the send dialog picked one.
    const std::string archive_device   = the_job.device_id;
    const std::string archive_dev_name = the_job.device_name;
    const std::string archive_mapping  = the_job.filament_mapping;
    const bool        archive_unload   = the_job.unload_at_end;

    // "Unload filaments after print", for a Snapmaker tool changer only. The flag cannot
    // ride inside the G-code: the firmware refuses SET_PRINT_PREFERENCES while print_stats.state is
    // "printing", so it has to reach the printer before the job starts. This queue uploads with
    // print=true in one request, so the preference goes out just ahead of it.
    //
    // Nothing here runs unless the send dialog offered the checkbox and it was ticked, and the
    // dialog only offers it for a Snapmaker tool changer starting a print. Every other host - every
    // Moonraker, OctoPrint, Duet, Elegoo ... send - reaches the upload below having executed not one
    // extra statement, and a failure to set the preference is logged and never fails the send: the
    // print is what the person asked for, the unload is a convenience on top of it.
    if (archive_unload && archive_print) {
        GUI::SnapmakerLan::Device d;
        d.ip = the_job.printhost->get_host();
        // "http://10.0.0.106:80/" and friends: SnapmakerLan wants the bare host and a port.
        {
            std::string h = d.ip;
            const size_t scheme = h.find("://");
            if (scheme != std::string::npos)
                h = h.substr(scheme + 3);
            if (const size_t slash = h.find('/'); slash != std::string::npos)
                h = h.substr(0, slash);
            if (const size_t colon = h.rfind(':'); colon != std::string::npos) {
                try {
                    d.port = std::stoi(h.substr(colon + 1));
                } catch (...) {}
                h = h.substr(0, colon);
            }
            d.ip = h;
        }
        // Which toolheads this print uses: the dialog's own mapping ("0:1,1:2" - filament:slot),
        // and, when nothing was mapped, the file goes out as sliced and toolhead 0 is the one it
        // runs on.
        std::vector<int> used;
        {
            std::string field;
            std::string list = archive_mapping + ",";
            bool        after_colon = false;
            for (char c : list) {
                if (c == ':') {
                    after_colon = true;
                    field.clear();
                } else if (c == ',') {
                    if (after_colon && !field.empty()) {
                        try {
                            const int t = std::stoi(field);
                            if (t >= 0 && std::find(used.begin(), used.end(), t) == used.end())
                                used.push_back(t);
                        } catch (...) {}
                    }
                    after_colon = false;
                    field.clear();
                } else {
                    field += c;
                }
            }
        }
        if (used.empty())
            used.push_back(0);
        const std::string script = "SET_PRINT_PREFERENCES " + GUI::SnapmakerLan::end_unload_parameter(used);
        std::string       err;
        if (!GUI::SnapmakerLan::run_script(d, script, err))
            BOOST_LOG_TRIVIAL(error) << "PrintHostJobQueue: the printer refused the end-of-print unload preference ("
                                     << err << "); the print goes ahead without it";
        else
            BOOST_LOG_TRIVIAL(info) << "PrintHostJobQueue: sent " << script;
    }

    bool success = false;
    try {
        success = the_job.printhost->upload(std::move(the_job.upload_data),
            [this](Http::Progress progress, bool &cancel) { this->progress_fn(std::move(progress), cancel); },
            [this](wxString error) { this->error_fn(std::move(error)); },
            [this](wxString tag, wxString host) { this->info_fn(std::move(tag), std::move(host)); });
    } catch (const std::exception &e) {
        if (!error_emitted.load())
            error_fn(wxstring_from_exception(e));
        success = false;
    } catch (...) {
        if (!error_emitted.load())
            error_fn(_L("Unknown error"));
        success = false;
    }

    if (success) {
        emit_progress(100);
        // Ultra: keep a copy of the file the print host received (Preferences > Ultra > G-Code
        // Archive). Archiving never fails an upload; switching to the device tab must not either.
        // These sit outside the upload try so a throw here cannot flip success into an error.
        try {
            if (GUI::GcodeArchive::enabled()) {
                GUI::GcodeArchive::Meta am = GUI::GcodeArchive::meta_for_plate(-1, archive_print ? "print" : "upload");
                // "ph:<device id>" when the send picked one of this model's devices, so a reprint can be
                // replayed to the printer it actually went to; "host" (the preset's own address) when
                // there were none, exactly as before.
                am.printer_id    = archive_device.empty() ? std::string("host") : ("ph:" + archive_device);
                am.printer_kind  = "printhost";
                am.printer_name  = archive_dev_name.empty() ? (host_name + " " + host_url) : archive_dev_name;
                am.file_name     = archive_name;
                am.mapping       = archive_mapping;
                am.unload_at_end = archive_unload && archive_print;
                GUI::GcodeArchive::archive(archive_source, am);
            }
            if (the_job.switch_to_device_tab) {
                const auto mainframe = GUI::wxGetApp().mainframe;
                mainframe->request_select_tab(MainFrame::TabPosition::tpMonitor);
            }
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(error) << "PrintHostJobQueue: post-upload archive/tab switch failed: " << e.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "PrintHostJobQueue: post-upload archive/tab switch failed";
        }
    }
}

void PrintHostJobQueue::enqueue(PrintHostJob job)
{
    p->start_bg_thread();
    p->queue_dialog->append_job(job);
    p->channel_jobs.push(std::move(job));
}

void PrintHostJobQueue::cancel(size_t id)
{
    p->channel_cancels.push(id);
}

}
