#ifndef slic3r_GUI_FFPrinterSources_hpp_
#define slic3r_GUI_FFPrinterSources_hpp_

#include <set>
#include <string>
#include <vector>

// Which FlashForge printers the Device tab lists from the user's own print-host settings, kept free
// of wx, the config, the network and FlashNetwork so the matching can be tested on its own.
//
// A user who set a Creator 5 up for sending (Printer settings > print host: IP address, serial
// number, check code) has already said everything the Device tab needs, yet the tab listed only what
// a LAN scan or its own "Add printer" had saved. The settings are the source of truth here: the tab
// takes every FlashForge device from them, keyed by serial number, and a printer that is also saved
// by "Add printer" is the same printer, not a second tile.

namespace Slic3r { namespace GUI {

// One FlashForge device as the print-host settings hold it. The caller has already picked the ones
// whose host type is Flashforge; everything is as typed.
struct FFPrinterSource
{
    std::string name;       // alias, or the address when nobody named it
    std::string address;    // "192.168.1.50", "192.168.1.50:8898", "http://192.168.1.50:8898/"
    std::string serial;
    std::string check_code; // never logged, never written anywhere but where it came from
    std::string origin;     // which settings entry this was, for tests and the log ("model/id")
};

enum class FFPrinterState
{
    Ready,      // address, serial number and check code are all there: connect
    NeedsSetup, // something is missing: show the tile and point at the settings
};

struct FFPrinterEntry
{
    std::string    key;        // the tile id: the serial number, else "addr:<ip>", else "name:<name>"
    std::string    name;
    std::string    serial;
    std::string    ip;         // IPv4 literal, empty when the address is not one
    unsigned short port = 8898;
    std::string    check_code;
    std::string    origin;
    FFPrinterState state = FFPrinterState::NeedsSetup;
    // What is missing, in words a user can act on: "serial number", "IP address", "check code".
    std::vector<std::string> missing;

    // "serial number and check code" / "IP address, serial number and check code"; empty when Ready.
    std::string missing_text() const;
};

// "192.168.1.50", "192.168.1.50:8898" or "http://192.168.1.50:8898/x" -> ip and port. False when
// what is left is not a dotted-quad IPv4 address (a host name cannot be used: FlashNetwork takes an
// IPv4 literal). The port is 8898 (the HTTP local API) unless the address names another one;
// 8899, the old console port, is not the API's and counts as none.
bool ff_parse_address(const std::string& address, std::string& ip, unsigned short& port);

// The tiles the settings ask for, one per printer:
//  - entries with no address, serial number and check code at all are not printers and are dropped;
//  - printers are the same when their serial numbers match, so several settings entries for one
//    serial give one tile - the most complete entry wins, then the first by name, so the answer does
//    not depend on the order the settings were read in;
//  - an entry with no serial number that points at the address of one that has one is that printer,
//    and gives no tile of its own;
//  - what is left without a serial is keyed by address, then by name.
// Sorted by name for a stable layout.
std::vector<FFPrinterEntry> ff_merge_printer_sources(const std::vector<FFPrinterSource>& sources);

// Tiles that came from the settings last time (`previous`) and should go now: the settings no
// longer ask for them and "Add printer" did not save them (`saved_serials`) - those are the user's
// own and stay.
std::vector<std::string> ff_stale_setting_tiles(const std::set<std::string>&         previous,
                                                const std::vector<FFPrinterEntry>&   now,
                                                const std::set<std::string>&         saved_serials);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_FFPrinterSources_hpp_
