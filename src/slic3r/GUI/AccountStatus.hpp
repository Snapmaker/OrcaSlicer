#ifndef slic3r_GUI_AccountStatus_hpp_
#define slic3r_GUI_AccountStatus_hpp_

#include <string>

#include "slic3r/Utils/AccountProviders.hpp"

class BBLTopbar;

// The wx / app side of the account registry (Utils/AccountProviders.hpp): registers the Bambu Lab
// and Snapmaker providers, works out which one the selected printer uses, remembers that the user
// has signed in, and tells the title bar whether to show the Account button as a warning
// (yellow text + tooltip) because that account is signed out.
//
// refresh() is cheap and idempotent. It is called when:
//   * the title bar is made,
//   * the Bambu sign-in state changes (NetworkAgent::change_user / user_logout, and the plug-in's
//     own user-login callback where it reports one),
//   * the Snapmaker sign-in state changes (SMUserInfo::notify: sign-in, silent sign-in, sign-out),
//   * the printer preset changes (Sidebar::update_presets) or another printer is picked in the
//     Device tab (DeviceManager::set_selected_machine).
// The theme is not an input: the title bar reads the colour as it draws.
//
// To add another account (a FlashForge cloud login, say): add one Accounts::Provider in
// register_providers() in AccountStatus.cpp, and call refresh_async() when its state changes.
namespace Slic3r {
namespace GUI {
namespace AccountStatus {

// Puts the Bambu Lab and Snapmaker providers in Accounts::registry(). Safe to call again.
void register_providers();

// The title bar this reports to (BBLTopbar's constructor / destructor). None on macOS.
void attach_topbar(BBLTopbar* topbar);
void detach_topbar(BBLTopbar* topbar);

// Re-evaluates and updates the title bar. GUI thread only.
void refresh();
// The same, queued onto the GUI thread: for callbacks that may run on another one.
void refresh_async();

// VendorProfile id of the selected printer ("BBL", "Snapmaker", "Flashforge", ...), "" when unknown.
std::string current_printer_vendor_id();

// What the Account button shows for the selected printer right now.
Accounts::Status current_status();

} // namespace AccountStatus
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_AccountStatus_hpp_
