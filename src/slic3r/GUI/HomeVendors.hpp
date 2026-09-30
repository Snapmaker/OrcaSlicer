#ifndef slic3r_GUI_HomeVendors_hpp_
#define slic3r_GUI_HomeVendors_hpp_

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/VendorConnector.hpp"

class wxWindow;

namespace Slic3r {
namespace GUI {

// The Home tab's Vendors section, on the slicer's side (the page is resources/web/home; the engine
// is Utils/VendorConnector). This owns everything the page must not touch:
//   * the connectors (app_config "home_vendor_connectors", specs without secrets),
//   * their secrets, in the OS credential store (wxSecretStore: Credential Manager, Keychain,
//     libsecret) or, where there is none, in memory for this session only,
//   * the HTTP calls (sync, thumbnails, downloads), always off the GUI thread,
//   * the caches in <data_dir>/vendors/<connector id>/.
// The page gets display fields and opaque keys; it never sees a secret, a thumbnail or download URL.
class HomeVendors
{
public:
    using SendFn = std::function<void(const nlohmann::json&)>;
    HomeVendors(wxWindow* parent, SendFn send, std::function<void()> library_changed);
    ~HomeVendors();

    // True when `msg` was a Vendors command (handled or refused).
    bool handle(const nlohmann::json& msg);
    void send_state();
    // The Vendor tags of all connectors, for the Library's folder editor.
    std::vector<std::string> vendor_tags() const;

private:
    struct Connector
    {
        Vendors::Spec  spec;
        Vendors::Cache cache;
        bool           cache_loaded { false };
        bool           syncing { false };
    };

    std::vector<Vendors::Spec> load_specs() const;
    void                       save_specs();
    Connector*                 find(const std::string& id);
    void                       ensure_cache(Connector& c);
    std::string                cache_dir(const std::string& id) const;

    // Secrets.
    bool             secure_store() const;
    Vendors::Secrets secrets_of(const Vendors::Spec& spec) const;
    bool             has_secret(const std::string& id, const std::string& key) const;
    void             set_secret(const std::string& id, const std::string& key, const std::string& value);
    void             forget_secrets(const Vendors::Spec& spec);

    void sync(const std::string& id, bool full);
    void test(const nlohmann::json& spec_json);
    void save_connector(const nlohmann::json& spec_json);
    void delete_connector(const std::string& id);
    void ask_secret(const std::string& id, const std::string& key);
    void import_spec();
    void export_spec(const std::string& id);
    void export_csv(const nlohmann::json& keys);
    void send_thumbnails(const std::vector<std::string>& keys);
    void download(const std::string& key, const std::string& sub_id);
    void notice(const std::string& text, bool error = false);

    // A key the page acts on -> (connector id, item id); rebuilt on every send_state().
    std::map<std::string, std::pair<std::string, std::string>> m_keys;
    const Vendors::Item* item_of(const std::string& key, Connector** connector = nullptr);

    wxWindow*                          m_parent;
    SendFn                             m_send;
    std::function<void()>              m_library_changed;
    std::vector<Connector>             m_connectors;
    // Secrets kept for this session when the system has no credential store.
    std::map<std::string, std::string> m_session_secrets;
    std::shared_ptr<bool>              m_alive { std::make_shared<bool>(true) };
    std::shared_ptr<std::atomic<bool>> m_cancel { std::make_shared<std::atomic<bool>>(false) };
    std::set<std::string>              m_thumbs_asked;
    bool                               m_thumbs_busy { false };
    std::vector<std::string>           m_thumbs_queue;
};

} // namespace GUI
} // namespace Slic3r

#endif
