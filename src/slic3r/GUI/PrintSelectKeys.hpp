#ifndef slic3r_PrintSelectKeys_hpp_
#define slic3r_PrintSelectKeys_hpp_

#include <cstddef>
#include <string>

namespace Slic3r {
namespace GUI {
namespace PrintSelectKeys {

// Stable string keys for the print-button dropdown, persisted as `last_print_action`.
// Reordering MainFrame::PrintSelectType must not remap a saved preference, so the key never
// derives from the enum integer. The integer values here MUST match MainFrame::PrintSelectType.
constexpr int ePrintAll            = 0;
constexpr int ePrintPlate          = 1;
constexpr int eExportSlicedFile    = 2;
constexpr int eExportGcode         = 3;
constexpr int eSendGcode           = 4;
constexpr int eSendToPrinter       = 5;
constexpr int eSendToPrinterAll    = 6;
constexpr int eUploadGcode         = 7;
constexpr int eExportAllSlicedFile = 8;
constexpr int ePrintMultiMachine   = 9;

// Only the print-host flow depends on a configured print host. Exporting G-code (or a sliced
// file) writes a file and never needs one, so a restored "Export G-code file" must not be greyed
// out on a printer without a host. MainFrame::set_print_button_to_default gates on this.
inline bool requires_print_host(int type)
{
    return type == eSendGcode;
}

// Empty string: no dropdown entry exists (eUploadGcode) or the type is unknown.
inline const char *key(int type)
{
    switch (type) {
    case ePrintAll:            return "print_all";
    case ePrintPlate:          return "print_plate";
    case eExportSlicedFile:    return "export_sliced_file";
    case eExportAllSlicedFile: return "export_all_sliced_file";
    case eExportGcode:         return "export_gcode";
    case eSendGcode:           return "send_gcode";
    case eSendToPrinter:       return "send_to_printer";
    case eSendToPrinterAll:    return "send_to_printer_all";
    case ePrintMultiMachine:   return "print_multi_machine";
    case eUploadGcode:         break;
    }
    return "";
}

// True and writes `out` only for a known persisted action. Empty / unknown keys fail so the
// caller can keep its computed default. Integer strings ("1") must not match.
inline bool from_key(const std::string &saved, int &out)
{
    if (saved == "print_all") {
        out = ePrintAll;
        return true;
    }
    if (saved == "print_plate") {
        out = ePrintPlate;
        return true;
    }
    if (saved == "export_sliced_file") {
        out = eExportSlicedFile;
        return true;
    }
    if (saved == "export_all_sliced_file") {
        out = eExportAllSlicedFile;
        return true;
    }
    if (saved == "export_gcode") {
        out = eExportGcode;
        return true;
    }
    if (saved == "send_gcode") {
        out = eSendGcode;
        return true;
    }
    if (saved == "send_to_printer") {
        out = eSendToPrinter;
        return true;
    }
    if (saved == "send_to_printer_all") {
        out = eSendToPrinterAll;
        return true;
    }
    if (saved == "print_multi_machine") {
        out = ePrintMultiMachine;
        return true;
    }
    return false;
}

// Restore a saved key only if it names an action in `offered`. Unknown keys, empty keys,
// keys that are just the enum integer, and actions the current printer does not offer all
// return `fallback` (the caller's computed default).
inline int resolve_or_default(const std::string &saved_key, const int *offered, size_t offered_count, int fallback)
{
    int parsed = 0;
    if (!from_key(saved_key, parsed))
        return fallback;
    for (size_t i = 0; i < offered_count; ++i) {
        if (offered[i] == parsed)
            return parsed;
    }
    return fallback;
}

} // namespace PrintSelectKeys
} // namespace GUI
} // namespace Slic3r

#endif // slic3r_PrintSelectKeys_hpp_
