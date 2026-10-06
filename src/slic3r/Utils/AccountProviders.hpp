#ifndef slic3r_AccountProviders_hpp_
#define slic3r_AccountProviders_hpp_

#include <functional>
#include <string>
#include <vector>

// Which vendors have a cloud account the slicer can be signed in to, and whether the current
// printer's account is signed out (the Account button in the title bar turns yellow then,
// GUI/AccountStatus.cpp). Kept free of wx and of the network plug-in so the rule can be tested on
// its own (tests/slic3rutils/account_providers_tests.cpp).
//
// A provider is one vendor (or a few preset vendor ids) plus a way to ask "is the user signed in
// to it right now". Bambu Lab and Snapmaker are registered in GUI/AccountStatus.cpp; another login
// (a FlashForge cloud account, say) is one more register_provider() call there. Anything that
// wants to know a sign-in state (the first-time Bambu setup notice, for one) asks the registry
// instead of reaching into the network agent:
//
//     const Accounts::Provider* bbl = Accounts::registry().find("bbl");
//     if (bbl && !bbl->is_signed_in()) ...
namespace Slic3r {
namespace Accounts {

struct Provider
{
    // Stable key: used in the app_config flag name and by callers. Lower case, no spaces ("bbl", "snapmaker").
    std::string id;
    // Shown to the user ("Bambu Lab", "Snapmaker"). Not translated: it is a brand.
    std::string display_name;
    // Preset vendor ids (VendorProfile::id) of the printers whose account this is ("BBL", "Snapmaker").
    std::vector<std::string> vendor_ids;
    // Signed in to this account right now. Called on the GUI thread. Required.
    std::function<bool()> is_signed_in;
    // The selected printer can only be reached through this account (a Bambu printer bound to the
    // user's cloud account, not a LAN-only one). Optional: no function means never.
    std::function<bool()> selected_printer_cloud_bound;
};

class Registry
{
public:
    // Adds a provider, or replaces the one with the same id. A provider without an id or without
    // is_signed_in is ignored.
    void add(Provider provider);
    void clear() { m_providers.clear(); }

    const std::vector<Provider>& providers() const { return m_providers; }
    const Provider*              find(const std::string& id) const;
    // The provider whose account the printers of preset vendor `vendor_id` use, or nullptr
    // (a vendor with no account, or an empty id). Exact match, as VendorProfile ids are.
    const Provider*              find_for_vendor(const std::string& vendor_id) const;

private:
    std::vector<Provider> m_providers;
};

// The one the app uses.
Registry& registry();

struct Inputs
{
    bool signed_in            = false; // the provider reports signed in
    bool previously_signed_in = false; // the user has been signed in to it before (flag in app_config)
    bool cloud_bound          = false; // the selected printer needs the account
};

// The highlight rule. Adjust it here:
//   * signed in: never.
//   * signed out and the user has been signed in before (an expired session, or a sign-out they
//     may not have meant): yes.
//   * signed out and the selected printer is cloud-bound: yes, it cannot be reached without it.
//   * signed out, never signed in, not cloud-bound (a LAN-only user): no.
bool should_warn(const Inputs& in);

struct Status
{
    const Provider* provider = nullptr; // null: the current printer's vendor has no account
    bool            signed_in = false;
    bool            warn      = false;  // show the Account button as a warning
};

// What the Account button shows for a printer of preset vendor `vendor_id`. `was_signed_in(id)`
// answers the "previously signed in" flag for a provider id.
Status evaluate(const Registry&                                  registry,
                const std::string&                               vendor_id,
                const std::function<bool(const std::string&)>&   was_signed_in);

// app_config key (section "app") of the "has signed in to this provider before" flag.
std::string signed_in_flag_key(const std::string& provider_id);

} // namespace Accounts
} // namespace Slic3r

#endif // slic3r_AccountProviders_hpp_
