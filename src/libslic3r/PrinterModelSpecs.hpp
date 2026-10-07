#pragma once

// Bed size and toolhead count of a printer model, read from one vendor's machine presets.
//
// The Printer Selection page (resources/web/guide/24, also the wizard's page 21) lists every
// printer model in one table with its build volume and toolhead count. Those come from the
// model's default machine preset: the instantiated preset of that model with a 0.4 mm nozzle
// (else the model's first listed nozzle, else any), with `inherits` resolved inside the vendor.
//
//   X, Y = bounding box of printable_area (a list of "XxY" points, or one comma-separated string)
//   Z    = printable_height
//   toolheads = number of nozzle_diameter entries (U1 = 4, H2D = 2, CORE One INDX 8T = 8)
//
// Anything missing or malformed leaves that value empty; the page shows a dash.

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {

struct PrinterModelSpecs
{
    std::optional<double> size_x;
    std::optional<double> size_y;
    std::optional<double> size_z;
    std::optional<int>    extruders;
    // Name of the machine preset the values came from (empty when none was found).
    std::string           machine;
};

// Width and depth of the bounding box of a printable_area value, or nothing when it does not
// parse. Accepts ["0x0","256x0",...], "0x0,256x0,..." and negative coordinates (delta beds).
std::optional<std::pair<double, double>> printable_area_size(const nlohmann::json &area);

// A number from a profile value: 250, "250", ["250"] (first element). Nothing when not a number.
std::optional<double> profile_number(const nlohmann::json &value);

// The machine presets of ONE vendor, by name. Names are only unique within a vendor (many vendors
// have their own "fdm_machine_common"), so build one index per vendor.
class MachinePresetIndex
{
public:
    // Adds one machine preset file (its parsed JSON). Only the keys the specs need are kept.
    // Returns false (and keeps nothing) when it has no string "name".
    bool add(const nlohmann::json &machine);

    size_t size() const { return m_by_name.size(); }

    // The specs of `model` (a machine_model "name"). `model_nozzles` is the model's
    // nozzle_diameter string ("0.4;0.2;0.6"); it picks the fallback preset when the model has
    // no 0.4 mm one.
    PrinterModelSpecs specs_for_model(const std::string &model, const std::string &model_nozzles) const;

    // The value of `key` for preset `name`, following `inherits` (null when not found).
    nlohmann::json resolve(const std::string &name, const std::string &key) const;

private:
    struct Entry
    {
        std::string    inherits;
        bool           instantiation { false };
        nlohmann::json values; // object holding only the kept keys
    };
    std::map<std::string, Entry> m_by_name;
};

} // namespace Slic3r
