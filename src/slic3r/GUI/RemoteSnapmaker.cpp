#include "RemoteSnapmaker.hpp"

#include "GUI_App.hpp"
#include "HttpServer.hpp" // LOCALHOST_URL
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "PrinterWebView.hpp"
#include "RemoteAccess.hpp"
#include "RemoteHub.hpp" // hub_dir
#include "SSWCP.hpp"     // query_machine_info
#include "SnapmakerLan.hpp" // is_cloud_host
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/MQTT.hpp"
#include "slic3r/Utils/MoonRaker.hpp"
#include "slic3r/Utils/PrintHost.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/asio.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace Slic3r {
namespace GUI {
namespace RemoteSnapmaker {

using nlohmann::json;
namespace asio = boost::asio;
namespace fs   = boost::filesystem;

// ---------------------------------------------------------------- helpers ----

// From a request thread: run fn on the GUI thread and wait for it (bounded). False on timeout,
// e.g. a modal dialog is blocking the app.
static bool on_main(std::function<void()> fn, int timeout_ms = 15000)
{
    // Through the gate (MainThreadGate.hpp): once the main window is closing this is false at once
    // and fn never runs against a Plater that is going away.
    return RemoteAccess::call_on_main(std::move(fn), timeout_ms) == MainCallResult::Done;
}

// ------------------------------------------------------ credential store ----
//
// The printer refuses every client but one holding the certificate it issued when the device was
// paired (its plain-MQTT port answers nothing, its MQTTS port refuses a client without one), and
// this fork keeps that certificate only inside the Device page. Remembering it is therefore the
// difference between the phone being able to connect a printer and not - and it is also a private
// key at rest, so it happens only when the person turns it on:
//     app_config  app / snapmaker_remember_keys = "1"
// The file is <datadir>/hub/snapmaker_keys.json, keyed by the printer's serial number; deleting it
// (or clearing the setting) takes the phone's connect away again.

static std::string keys_path() { return (fs::path(RemoteHub::hub_dir()) / "snapmaker_keys.json").string(); }

static bool remember_keys_on()
{
    // "1" from a hand edit or "true" from the Preferences checkbox (Phone access) both count.
    return wxGetApp().app_config && wxGetApp().app_config->get_bool("snapmaker_remember_keys");
}

static std::mutex s_keys_mutex;

static json load_keys()
{
    std::lock_guard<std::mutex> lock(s_keys_mutex);
    try {
        boost::nowide::ifstream in(keys_path());
        if (!in.good())
            return json::object();
        json j;
        in >> j;
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

void remember_credentials(const std::string& sn, const json& params)
{
    if (sn.empty() || !remember_keys_on())
        return;
    auto str = [&params](const char* k) {
        return params.count(k) && params[k].is_string() ? params[k].get<std::string>() : std::string();
    };
    json entry;
    entry["ca"]       = str("ca");
    entry["cert"]     = str("cert");
    entry["key"]      = str("key");
    entry["clientId"] = str("clientId");
    entry["port"]     = params.count("port") && params["port"].is_number_integer() ? params["port"].get<int>() : 8883;
    if (entry["ca"].get<std::string>().empty() || entry["cert"].get<std::string>().empty() ||
        entry["key"].get<std::string>().empty())
        return; // nothing a later connect could use
    json all = load_keys();
    all[sn]  = entry;
    try {
        boost::system::error_code ec;
        fs::create_directories(RemoteHub::hub_dir(), ec);
        std::lock_guard<std::mutex> lock(s_keys_mutex);
        boost::nowide::ofstream out(keys_path(), std::ios::binary | std::ios::trunc);
        out << all.dump();
        out.close();
        BOOST_LOG_TRIVIAL(info) << "[RemoteSnapmaker] kept the connect credentials of " << sn;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[RemoteSnapmaker] could not keep the connect credentials: " << e.what();
    }
}

// Fill in what the device record does not carry (the fork blanks it) from the store.
static void apply_stored_credentials(DeviceInfo& d)
{
    if (d.sn.empty() || (!d.ca.empty() && !d.cert.empty() && !d.key.empty()))
        return;
    const json all = load_keys();
    if (!all.contains(d.sn) || !all[d.sn].is_object())
        return;
    const json& e   = all[d.sn];
    auto        str = [&e](const char* k) { return e.count(k) && e[k].is_string() ? e[k].get<std::string>() : std::string(); };
    d.ca   = str("ca");
    d.cert = str("cert");
    d.key  = str("key");
    if (d.clientId.empty())
        d.clientId = str("clientId");
    if (e.count("port") && e["port"].is_number_integer())
        d.port = e["port"].get<int>();
}

// The MQTT port the Device page uses: the one the printer handed out with the certificate. A
// record without one predates the pairing (or carries none): 8883 for the secure port the printer
// serves requests on, 1884 for the plain one that only carries the pairing exchange.
static int device_port(const DeviceInfo& d)
{
    if (d.port > 0)
        return d.port;
    return (!d.ca.empty() && !d.cert.empty() && !d.key.empty()) ? 8883 : 1884;
}

// The page connects over TLS only when it holds all three PEM blobs, and over plain MQTT
// otherwise (main.dart.js, WcpClient.createAgentId); mirror that.
static bool device_has_tls(const DeviceInfo& d) { return !d.ca.empty() && !d.cert.empty() && !d.key.empty(); }

// Can the phone connect this device on its own, or does it need the person at the PC? Only a
// device whose certificate is at hand (in the record, or kept from an earlier PC connect).
static bool device_can_connect(const DeviceInfo& d)
{
    DeviceInfo copy = d;
    apply_stored_credentials(copy);
    return device_has_tls(copy);
}

// Paho needs a client id. The device's own (handed out when it was paired) is the right one; the
// fork's fallback elsewhere is this PC's LAN address, so use that when the device has none.
static std::string client_id_for(const DeviceInfo& d)
{
    if (!d.clientId.empty())
        return d.clientId;
    try {
        asio::io_context      ioc;
        asio::ip::udp::socket s(ioc);
        s.open(asio::ip::udp::v4());
        s.connect(asio::ip::udp::endpoint(asio::ip::make_address_v4("8.8.8.8"), 53)); // sends nothing
        const std::string a = s.local_endpoint().address().to_string();
        if (a != "0.0.0.0")
            return a;
    } catch (...) {}
    return d.sn.empty() ? std::string("Snapmaker Orca") : ("Snapmaker Orca " + d.sn);
}

static bool is_snapmaker_preset()
{
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (!bundle)
        return false;
    const ConfigOptionString* model = bundle->printers.get_edited_preset().config.option<ConfigOptionString>("printer_model");
    return model && boost::icontains(model->value, "Snapmaker");
}

// One device, without anything secret (no certificates, no password). What the store holds for it
// is folded in first, so the port and `can_connect` describe the connection the phone would get.
static json device_json(const DeviceInfo& in)
{
    DeviceInfo d = in;
    apply_stored_credentials(d);
    json j;
    j["id"]           = d.dev_id;
    j["name"]         = d.dev_name;
    j["model"]        = d.model_name;
    j["ip"]           = d.ip;
    j["port"]         = device_port(d);
    j["sn"]           = d.sn;
    j["preset"]       = d.preset_name;
    j["nozzles"]      = d.nozzle_sizes;
    j["connected"]    = d.connected;
    j["link_mode"]    = d.link_mode;
    j["can_connect"]  = device_can_connect(d);
    return j;
}

// ------------------------------------------------------------------ list ----

void list(json& out)
{
    const std::vector<DeviceInfo> devices = wxGetApp().app_config->get_devices();
    std::shared_ptr<PrintHost>    host    = nullptr;
    wxGetApp().get_connect_host(host);
    const std::string host_addr = host ? host->get_host() : std::string();

    out["devices"] = json::array();
    for (const DeviceInfo& d : devices) {
        json j = device_json(d);
        // The connected host is addressed as ip[:port]; match on the address, as list_hosts does.
        // Not for a connection through the Snapmaker cloud: then the host is the cloud's broker,
        // which is the recorded "ip" of EVERY cloud-bound printer, and only the record the Device
        // tab marked connected is the one.
        const bool by_cloud = host && SnapmakerLan::is_cloud_host(host_addr);
        j["is_host"] = host && !d.ip.empty() && host_addr.compare(0, d.ip.size(), d.ip) == 0 && (!by_cloud || d.connected);
        if (SnapmakerLan::is_cloud_host(d.ip)) {
            // Where the phone shows an address it must not show the broker as the printer's.
            j["ip"]  = "";
            j["via"] = "cloud";
        }
        out["devices"].push_back(j);
    }
    out["connected"]       = host != nullptr;
    out["host"]            = host_addr;
    out["host_online"]     = host ? host->check_sn_arrived() : false;
    out["printer_preset"]  = wxGetApp().preset_bundle ? wxGetApp().preset_bundle->printers.get_selected_preset_name() : "";
    out["is_snapmaker"]    = is_snapmaker_preset();
    out["use_new_connect"] = wxGetApp().app_config->get("use_new_connect") == "true";
}

// --------------------------------------------------------------- connect ----

// What the Device page does when the machine drops: forget the host, clear the cards and let the
// sidebar redraw. The desktop also shows a "please reconnect" dialog; a phone connection must not
// put a modal on a PC nobody is looking at, so this only logs.
static void note_connection_lost()
{
    RemoteAccess::post_to_main([]() {
        BOOST_LOG_TRIVIAL(warning) << "[RemoteSnapmaker] the connected Snapmaker dropped";
        wxGetApp().app_config->set("use_new_connect", "false");
        std::shared_ptr<PrintHost> ptr = nullptr;
        wxGetApp().get_connect_host(ptr);
        if (ptr) {
            wxString disconn_msg = "";
            json     disconn_param;
            ptr->disconnect(disconn_msg, disconn_param);
        }
        wxGetApp().set_connect_host(nullptr);
        auto devices = wxGetApp().app_config->get_devices();
        for (size_t i = 0; i < devices.size(); ++i) {
            if (devices[i].connected) {
                devices[i].connected = false;
                wxGetApp().app_config->save_device_info(devices[i]);
                break;
            }
        }
        json data = wxGetApp().app_config->get_devices();
        wxGetApp().device_card_notify(data);
        if (wxGetApp().mainframe && wxGetApp().mainframe->plater())
            wxGetApp().mainframe->plater()->sidebar().update_all_preset_comboboxes();
    });
}

// The Device page's own post-connect work (SSWCP.cpp:6908-6951): the cards, the feature flag, the
// printer combo boxes, the print button and the Device tab's page.
static void announce_connected()
{
    auto devices = wxGetApp().app_config->get_devices();
    // The card list the Flutter page listens for, both ways it is delivered.
    json param;
    param["command"]    = "local_devices_arrived";
    param["sequece_id"] = "10001";
    param["data"]       = devices;
    try {
        wxGetApp().run_script(wxString::Format("window.postMessage(%s)", param.dump()));
    } catch (...) {}
    json data = devices;
    wxGetApp().device_card_notify(data);

    wxGetApp().app_config->set("use_new_connect", "true");
    MainFrame* mf = wxGetApp().mainframe;
    if (!mf || !mf->plater())
        return;
    mf->plater()->sidebar().update_all_preset_comboboxes(true);
    mf->m_print_enable = true;
    mf->update_slice_print_status(MainFrame::eEventPlateUpdate);
    // update_all_preset_comboboxes only loads the device page the first time (its is_sm_page
    // latch), so ask for it here the way the Device page's own connect does.
    if (mf->m_printer_view) {
        const wxString url = wxString::FromUTF8(wxGetApp().page_url("/web/flutter_web/index.html?path=2"));
        mf->load_printer_url(wxGetApp().get_international_url(url));
    }
}

static std::pair<int, std::string> connect_impl(const std::string& dev_id)
{
    // ---- 1. the stored device ----
    auto found = std::make_shared<bool>(false);
    auto dev   = std::make_shared<DeviceInfo>();
    auto busy  = std::make_shared<bool>(false);
    if (!on_main([found, dev, busy, dev_id]() {
            const std::vector<DeviceInfo> devices = wxGetApp().app_config->get_devices();
            for (const DeviceInfo& d : devices) {
                if (d.dev_id == dev_id || (!d.sn.empty() && d.sn == dev_id) || (!d.ip.empty() && d.ip == dev_id)) {
                    *dev   = d;
                    *found = true;
                    break;
                }
            }
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
            *busy = host != nullptr;
        }))
        return { 503, "the slicer is busy" };
    if (!*found)
        return { 404, "no such device: " + dev_id + " (pair it on the PC's Device page first)" };
    if (dev->ip.empty())
        return { 409, "that device has no address; pair it again on the PC" };
    if (dev->connected && *busy)
        return { 200, "" }; // already the connected host
    // Where the certificate comes from decides whether the device record keeps one (below).
    const bool record_had_certs = device_has_tls(*dev);
    apply_stored_credentials(*dev);
    if (!device_has_tls(*dev))
        return { 409, "there is no certificate for " + (dev->dev_name.empty() ? dev->ip : dev->dev_name) +
                          " on this PC: the printer answers only the client it issued one to, and the slicer keeps it "
                          "in the Device page alone. Turn on \"Remember Snapmaker printer certificates\" under "
                          "Preferences > Phone access, connect this printer once on the PC, and the phone can "
                          "connect it from then on." };

    // ---- 2. the host, built exactly as sw_mqtt_set_engine builds it ----
    const int         port = device_port(*dev);
    const std::string addr = dev->ip + ":" + std::to_string(port);
    auto              host = std::make_shared<std::shared_ptr<PrintHost>>();
    auto              err  = std::make_shared<std::string>();
    // The Moonraker_Mqtt constructor drops the process-wide MQTTS client and opens a socket to
    // find this PC's address, so it must run before the new engine is created - and it touches
    // the preset bundle, so it runs on the GUI thread.
    if (!on_main([host, err, addr]() {
            try {
                DynamicPrintConfig config = wxGetApp().preset_bundle->printers.get_edited_preset().config;
                config.option<ConfigOptionEnum<PrintHostType>>("host_type")->value = PrintHostType::htMoonRaker_mqtt;
                config.set("print_host", addr);
                std::shared_ptr<PrintHost> h(PrintHost::get_print_host(&config));
                if (!h) {
                    *err = "could not build the Moonraker host";
                    return;
                }
                wxGetApp().set_connect_host(h);
                wxGetApp().set_host_config(config);
                *host = h;
            } catch (const std::exception& e) {
                *err = std::string("could not build the Moonraker host: ") + e.what();
            }
        }))
        return { 503, "the slicer is busy" };
    if (!*host)
        return { 500, err->empty() ? "could not build the Moonraker host" : *err };
    std::shared_ptr<Moonraker_Mqtt> mqtt = std::dynamic_pointer_cast<Moonraker_Mqtt>(*host);
    if (!mqtt)
        return { 500, "the printer preset does not use the Snapmaker MQTT host" };

    // ---- 3. the engine: the socket the Device page opens with sw_create_mqtt_client ----
    // Always MQTTS: the printer's plain port carries the pairing exchange only and answers no
    // request, so a connect without the certificate was refused above.
    const std::string url    = "mqtts://" + addr;
    const std::string client = client_id_for(*dev);
    // From here on the host is the app's connected host, so every way out has to take it back:
    // a half-connected host would show up as a printer to send to.
    auto forget_host = []() { on_main([]() { wxGetApp().set_connect_host(nullptr); }); };
    std::shared_ptr<MqttClient> engine;
    try {
        engine = MqttClient::create(url, client, dev->ca, dev->cert, dev->key, dev->user, dev->password, false);
    } catch (const std::exception& e) {
        forget_host();
        return { 500, std::string("could not create the MQTT client: ") + e.what() };
    }
    engine->SetConnectionFailureCallback([engine]() {
        std::string msg = "";
        engine->Disconnect(msg);
    });
    std::string msg;
    if (!engine->Connect(msg)) { // blocks, 20 s at most
        forget_host();
        return { 502, "could not reach " + addr + ": " + (msg.empty() ? std::string("no answer") : msg) };
    }

    // ---- 4. hand it to the host and subscribe, as sw_mqtt_set_engine + sw_mqtt_subscribe do ----
    mqtt->m_ca        = dev->ca;
    mqtt->m_cert      = dev->cert;
    mqtt->m_key       = dev->key;
    mqtt->m_user_name = dev->user;
    mqtt->m_password  = dev->password;
    mqtt->m_port      = port;
    mqtt->m_client_id = client;
    {
        Moonraker_Mqtt::m_sn_mtx.lock();
        Moonraker_Mqtt::m_sn = dev->sn;
        Moonraker_Mqtt::m_sn_mtx.unlock();
    }
    std::string engine_msg = "success";
    if (!mqtt->set_engine(engine, engine_msg)) {
        std::string ig;
        engine->Disconnect(ig);
        forget_host();
        return { 502, "the printer accepted the connection but the slicer could not use it: " + engine_msg };
    }
    std::string sub_msg = "success";
    if (!mqtt->subscribe_device_topics(sub_msg))
        BOOST_LOG_TRIVIAL(warning) << "[RemoteSnapmaker] topic subscription: " << sub_msg;
    mqtt->set_connection_lost([]() { note_connection_lost(); });

    // ---- 5. does it answer? (the Device page's own check, and where the nozzles come from) ----
    std::string              model, device_name;
    std::vector<std::string> nozzles;
    std::shared_ptr<PrintHost> as_host = *host;
    const bool answered = SSWCP::query_machine_info(as_host, model, nozzles, device_name, 10);
    if (!answered || model.empty()) {
        std::string ig;
        engine->Disconnect(ig);
        forget_host();
        return { 504, "connected to " + addr + " but the printer did not answer; is it awake?" };
    }

    // ---- 6. what the PC shows: the cards, the combo boxes, the Device tab ----
    if (!on_main([dev, model, nozzles, device_name, record_had_certs]() {
            // One device at a time, like the Device page.
            auto devices = wxGetApp().app_config->get_devices();
            for (size_t i = 0; i < devices.size(); ++i) {
                if (devices[i].connected && devices[i].dev_id != dev->dev_id) {
                    devices[i].connected = false;
                    wxGetApp().app_config->save_device_info(devices[i]);
                }
            }
            DeviceInfo info    = *dev;
            info.connected     = true;
            info.model_name    = model;
            // A certificate that came from the store stays there: this record goes into the app
            // config, which is not where the fork keeps one (SSWCP.cpp:6716-6718 blanks it). A
            // record that carried its own keeps it.
            if (!record_had_certs)
                info.ca = info.cert = info.key = "";
            if (!device_name.empty())
                info.dev_name = device_name;
            if (!nozzles.empty()) {
                info.nozzle_sizes = nozzles;
                info.preset_name  = model + " (" + nozzles[0] + " nozzle)";
            }
            wxGetApp().app_config->save_device_info(info);
            announce_connected();
        }, 30000))
        return { 503, "the slicer is busy" };
    BOOST_LOG_TRIVIAL(info) << "[RemoteSnapmaker] connected " << addr << " (" << model << ")";
    return { 200, "" };
}

// The MQTT library throws on material it cannot use (a certificate the printer no longer accepts,
// a truncated key), and an escaping exception would take the request's thread down with no answer
// and the half-built host still in place.
std::pair<int, std::string> connect(const std::string& dev_id)
{
    try {
        return connect_impl(dev_id);
    } catch (const std::exception& e) {
        on_main([]() { wxGetApp().set_connect_host(nullptr); });
        BOOST_LOG_TRIVIAL(error) << "[RemoteSnapmaker] connect failed: " << e.what();
        return { 502, std::string("the connection failed: ") + e.what() };
    } catch (...) {
        on_main([]() { wxGetApp().set_connect_host(nullptr); });
        return { 502, "the connection failed" };
    }
}

// ------------------------------------------------------------ disconnect ----

static std::pair<int, std::string> disconnect_impl()
{
    auto have = std::make_shared<bool>(false);
    if (!on_main([have]() {
            std::shared_ptr<PrintHost> host = nullptr;
            wxGetApp().get_connect_host(host);
            *have = host != nullptr;
        }))
        return { 503, "the slicer is busy" };
    if (!*have)
        return { 409, "no Snapmaker is connected" };
    // The Device page's sw_Disconnect: the MQTT teardown on a worker thread, the rest on the GUI
    // thread through this call's own CallAfter chain.
    wxGetApp().sm_disconnect_current_machine(true);
    if (!on_main([]() {
            wxGetApp().app_config->clear_filament_extruder_map();
            if (wxGetApp().preset_bundle) {
                wxGetApp().preset_bundle->machine_filaments.clear();
                wxGetApp().preset_bundle->m_connect_machine_info_list.clear();
            }
            json data = wxGetApp().app_config->get_devices();
            wxGetApp().device_card_notify(data);
        }, 30000))
        return { 503, "the slicer is busy" };
    return { 200, "" };
}

std::pair<int, std::string> disconnect()
{
    try {
        return disconnect_impl();
    } catch (const std::exception& e) {
        on_main([]() { wxGetApp().set_connect_host(nullptr); });
        BOOST_LOG_TRIVIAL(error) << "[RemoteSnapmaker] disconnect failed: " << e.what();
        return { 500, std::string("the disconnect failed: ") + e.what() };
    } catch (...) {
        on_main([]() { wxGetApp().set_connect_host(nullptr); });
        return { 500, "the disconnect failed" };
    }
}

} // namespace RemoteSnapmaker
} // namespace GUI
} // namespace Slic3r
