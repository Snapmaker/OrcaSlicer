#include "PrinterModelSpecs.hpp"

#include "LocalesUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>

namespace Slic3r {

namespace {

// The preset keys the specs read. Everything else in a machine file is dropped on add().
const char *const pms_kept_keys[] = { "printer_model", "printable_area", "printable_height", "nozzle_diameter" };

std::string pms_trim(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b])) ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
    return s.substr(b, e - b);
}

// A whole-string, locale-independent number ("250", "0.4", "-125"). Nothing on junk or trailing text.
std::optional<double> pms_parse_number(const std::string &text)
{
    const std::string s = pms_trim(text);
    if (s.empty()) return std::nullopt;
    size_t pos = 0;
    double v   = 0.;
    try {
        v = string_to_double_decimal_point(s, &pos);
    } catch (...) {
        return std::nullopt;
    }
    if (pos != s.size() || !std::isfinite(v)) return std::nullopt;
    return v;
}

std::vector<std::string> pms_split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t p = s.find(sep, start);
        out.emplace_back(pms_trim(s.substr(start, p == std::string::npos ? std::string::npos : p - start)));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

} // namespace

std::optional<double> profile_number(const nlohmann::json &value)
{
    if (value.is_number()) {
        double v = value.get<double>();
        return std::isfinite(v) ? std::optional<double>(v) : std::nullopt;
    }
    if (value.is_string()) return pms_parse_number(value.get<std::string>());
    if (value.is_array() && !value.empty()) return profile_number(value.front());
    return std::nullopt;
}

std::optional<std::pair<double, double>> printable_area_size(const nlohmann::json &area)
{
    std::vector<std::string> points;
    if (area.is_array()) {
        for (const nlohmann::json &p : area) {
            if (!p.is_string()) return std::nullopt;
            points.emplace_back(p.get<std::string>());
        }
    } else if (area.is_string()) {
        for (std::string &p : pms_split(area.get<std::string>(), ','))
            if (!p.empty()) points.emplace_back(std::move(p));
    } else
        return std::nullopt;
    if (points.size() < 2) return std::nullopt;

    double min_x = std::numeric_limits<double>::max(), max_x = std::numeric_limits<double>::lowest();
    double min_y = min_x, max_y = max_x;
    for (const std::string &p : points) {
        const size_t sep = p.find('x');
        if (sep == std::string::npos) return std::nullopt;
        const std::optional<double> x = pms_parse_number(p.substr(0, sep));
        const std::optional<double> y = pms_parse_number(p.substr(sep + 1));
        if (!x || !y) return std::nullopt;
        min_x = std::min(min_x, *x); max_x = std::max(max_x, *x);
        min_y = std::min(min_y, *y); max_y = std::max(max_y, *y);
    }
    const double w = max_x - min_x, d = max_y - min_y;
    if (w <= 0. || d <= 0.) return std::nullopt;
    return std::make_pair(w, d);
}

bool MachinePresetIndex::add(const nlohmann::json &machine)
{
    if (!machine.is_object()) return false;
    auto name = machine.find("name");
    if (name == machine.end() || !name->is_string()) return false;

    Entry e;
    auto inh = machine.find("inherits");
    if (inh != machine.end() && inh->is_string()) e.inherits = inh->get<std::string>();
    auto inst = machine.find("instantiation");
    if (inst != machine.end())
        e.instantiation = (inst->is_string() && inst->get<std::string>() == "true") || (inst->is_boolean() && inst->get<bool>());
    e.values = nlohmann::json::object();
    for (const char *key : pms_kept_keys) {
        auto it = machine.find(key);
        if (it != machine.end() && !it->is_null()) e.values[key] = *it;
    }
    // First wins, like the preset loader's own de-duplication by name.
    m_by_name.emplace(name->get<std::string>(), std::move(e));
    return true;
}

nlohmann::json MachinePresetIndex::resolve(const std::string &name, const std::string &key) const
{
    std::set<std::string> seen; // an `inherits` cycle in a broken bundle must not hang the dialog
    std::string           cur = name;
    while (!cur.empty() && seen.insert(cur).second) {
        auto it = m_by_name.find(cur);
        if (it == m_by_name.end()) break;
        auto v = it->second.values.find(key);
        if (v != it->second.values.end()) return *v;
        cur = it->second.inherits;
    }
    return nlohmann::json();
}

PrinterModelSpecs MachinePresetIndex::specs_for_model(const std::string &model, const std::string &model_nozzles) const
{
    PrinterModelSpecs out;

    // The model's instantiated presets and their (first) nozzle diameter.
    struct Candidate { const std::string *name; std::optional<double> nozzle; };
    std::vector<Candidate> cands;
    for (const auto &[name, entry] : m_by_name) {
        if (!entry.instantiation) continue;
        const nlohmann::json pm = resolve(name, "printer_model");
        if (!pm.is_string() || pm.get<std::string>() != model) continue;
        cands.push_back({ &name, profile_number(resolve(name, "nozzle_diameter")) });
    }
    if (cands.empty()) return out;

    // Shortest name first ("X 0.4 nozzle" before "X HF 0.4 nozzle"), then by name: deterministic.
    std::sort(cands.begin(), cands.end(), [](const Candidate &a, const Candidate &b) {
        return a.name->size() != b.name->size() ? a.name->size() < b.name->size() : *a.name < *b.name;
    });

    std::vector<double> wanted { 0.4 };
    for (const std::string &n : pms_split(model_nozzles, ';'))
        if (std::optional<double> v = pms_parse_number(n)) wanted.push_back(*v);

    const Candidate *pick = nullptr;
    for (double w : wanted) {
        for (const Candidate &c : cands)
            if (c.nozzle && std::abs(*c.nozzle - w) < 1e-6) { pick = &c; break; }
        if (pick) break;
    }
    if (!pick) pick = &cands.front();

    out.machine = *pick->name;
    if (auto sz = printable_area_size(resolve(out.machine, "printable_area"))) {
        out.size_x = sz->first;
        out.size_y = sz->second;
    }
    if (auto z = profile_number(resolve(out.machine, "printable_height")); z && *z > 0.)
        out.size_z = *z;
    const nlohmann::json nd = resolve(out.machine, "nozzle_diameter");
    if (nd.is_array() && !nd.empty())
        out.extruders = int(nd.size());
    else if (nd.is_string() || nd.is_number())
        out.extruders = 1;
    return out;
}

} // namespace Slic3r
