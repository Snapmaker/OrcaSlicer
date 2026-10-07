#include "CostOverrides.hpp"

#include "Config.hpp"
#include "Preset.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <mutex>
#include <regex>
#include <sstream>

namespace Slic3r {
namespace CostOverrides {

using nlohmann::json;
namespace fs = boost::filesystem;

// ------------------------------------------------------------------ the key ----

static std::string collapse_spaces(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            space = !out.empty();
            continue;
        }
        if (space)
            out += ' ';
        space = false;
        out += char(c);
    }
    return out;
}

static std::string lowered(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

static std::string uppered(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s;
}

std::string family_name(const std::string &preset_name)
{
    std::string name = preset_name;
    // Printer / variant part: "Bambu PLA Basic @BBL X1C", "Afinia ABS@HS", "Generic PLA @System".
    if (const size_t at = name.find('@'); at != std::string::npos)
        name.erase(at);
    name = collapse_spaces(name);
    // Nozzle part without an '@': "Anker Generic ABS 0.2 nozzle", "... (0.6 nozzle)", "... 0.4mm nozzle".
    static const std::regex nozzle(R"(\s*[\(\[]?\s*\d+(?:\.\d+)?\s*(?:mm)?\s*nozzle\s*[\)\]]?\s*$)", std::regex::icase);
    for (;;) {
        std::smatch m;
        if (!std::regex_search(name, m, nozzle) || m.position(0) == 0)
            break;
        name = collapse_spaces(name.substr(0, size_t(m.position(0))));
    }
    return name;
}

std::string family_key(const std::string &vendor, const std::string &type, const std::string &family)
{
    return lowered(collapse_spaces(vendor)) + "|" + uppered(collapse_spaces(type)) + "|" + lowered(collapse_spaces(family));
}

static std::string first_string(const DynamicPrintConfig &config, const char *key, size_t idx = 0)
{
    if (const auto *opt = config.option<ConfigOptionStrings>(key); opt != nullptr && !opt->values.empty())
        return opt->get_at(idx);
    if (const auto *opt = config.option<ConfigOptionString>(key); opt != nullptr)
        return opt->value;
    return {};
}

static double first_price(const DynamicPrintConfig &config)
{
    const auto *opt = config.option<ConfigOptionFloats>("filament_cost");
    return opt != nullptr && !opt->values.empty() ? opt->get_at(0) : 0.;
}

static bool same_price(double a, double b) { return std::abs(a - b) < 1e-6; }

Identity identify(const Preset &preset, double price, const PresetCollection *filaments)
{
    Identity id;
    id.preset = preset.name;
    id.vendor = first_string(preset.config, "filament_vendor");
    id.type   = first_string(preset.config, "filament_type");

    const Preset *parent = nullptr;
    if (!preset.is_system && !preset.is_default && filaments != nullptr)
        parent = filaments->get_preset_parent(preset);
    const std::string &inherits = preset.inherits();
    // A user preset belongs to its parent's family; a system preset (whose inherits is cleared on
    // load) or a root user preset to its own name's.
    id.family = family_name(parent != nullptr ? parent->name : (!preset.is_system && !inherits.empty()) ? inherits : preset.name);
    if (parent != nullptr) {
        id.parent_price = first_price(parent->config);
        id.own_price    = !same_price(price, id.parent_price);
    }
    return id;
}

std::string printer_family_name(const std::string &preset_name)
{
    std::string name = collapse_spaces(preset_name);
    static const std::regex nozzle(R"(\s*[\(\[]?\s*\d+(?:\.\d+)?\s*(?:mm)?\s*nozzle\s*[\)\]]?\s*$)", std::regex::icase);
    for (;;) {
        std::smatch m;
        if (!std::regex_search(name, m, nozzle) || m.position(0) == 0)
            break;
        name = collapse_spaces(name.substr(0, size_t(m.position(0))));
    }
    return name;
}

std::string machine_key(const std::string &vendor, const std::string &model)
{
    return lowered(collapse_spaces(vendor)) + "|" + lowered(collapse_spaces(model));
}

static double time_cost_of(const DynamicPrintConfig &config)
{
    const auto *opt = config.option<ConfigOptionFloat>("time_cost");
    return opt != nullptr ? opt->value : 0.;
}

MachineIdentity identify_machine(const Preset &preset, double rate, const PresetCollection *printers)
{
    MachineIdentity id;
    id.preset = preset.name;
    const Preset *parent = nullptr;
    if (!preset.is_system && !preset.is_default && printers != nullptr)
        parent = printers->get_preset_parent(preset);
    // A user preset carries no vendor profile of its own: its parent's.
    if (preset.vendor != nullptr)
        id.vendor = preset.vendor->name;
    else if (parent != nullptr && parent->vendor != nullptr)
        id.vendor = parent->vendor->name;
    id.model = collapse_spaces(first_string(preset.config, "printer_model"));
    if (id.model.empty() && parent != nullptr)
        id.model = collapse_spaces(first_string(parent->config, "printer_model"));
    if (id.model.empty())
        id.model = printer_family_name(parent != nullptr ? parent->name : preset.name);
    if (parent != nullptr) {
        id.parent_rate = time_cost_of(parent->config);
        id.own_rate    = !same_price(rate, id.parent_rate);
    }
    return id;
}

// ---------------------------------------------------------------- the store ----

static long long now_s()
{
    return (long long) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string str_of(const json &j, const char *key)
{
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : std::string();
}

static const char *const ENTRY_KEYS[]   = {"scope", "preset", "vendor", "type", "family", "price_per_kg", "updated"};
static const char *const MACHINE_KEYS[] = {"scope", "preset", "vendor", "model", "rate_per_h", "updated"};
static const char *const DEFAULT_KEYS[] = {"rate_per_h", "updated"};

template<size_t N> static std::string unknown_fields(const json &e, const char *const (&known)[N])
{
    json unknown = json::object();
    for (auto it = e.begin(); it != e.end(); ++it)
        if (std::find(std::begin(known), std::end(known), it.key()) == std::end(known))
            unknown[it.key()] = it.value();
    return unknown.empty() ? std::string() : unknown.dump();
}

static void merge_fields(json &into, const std::string &extra)
{
    if (extra.empty())
        return;
    try {
        json e = json::parse(extra);
        if (e.is_object())
            for (auto it = e.begin(); it != e.end(); ++it)
                into[it.key()] = it.value();
    } catch (...) {}
}

bool Store::load(const std::string &path)
{
    m_entries.clear();
    m_machines.clear();
    m_has_default_rate = false;
    m_default_rate     = 0.;
    m_default_updated  = 0;
    m_default_extra.clear();
    m_extra.clear();
    m_version    = VERSION;
    m_load_error.clear();

    boost::system::error_code ec;
    if (!fs::exists(path, ec))
        return true;
    json j;
    try {
        boost::nowide::ifstream in(path);
        if (!in.good()) {
            m_load_error = "cannot open";
            return false;
        }
        in >> j;
    } catch (const std::exception &e) {
        m_load_error = e.what();
        return false;
    }
    if (!j.is_object() || (j.contains("filament") && !j["filament"].is_array()) || (j.contains("machine") && !j["machine"].is_array())) {
        m_load_error = "not a cost file";
        return false;
    }
    // Version 1 (filament prices only) is read as it is and written back as VERSION; a later
    // version keeps its number.
    if (j.contains("version") && j["version"].is_number_integer())
        m_version = std::max(int(VERSION), j["version"].get<int>());

    json extra = json::object();
    for (auto it = j.begin(); it != j.end(); ++it)
        if (it.key() != "version" && it.key() != "filament" && it.key() != "machine" && it.key() != "machine_default")
            extra[it.key()] = it.value();
    if (!extra.empty())
        m_extra = extra.dump();

    if (j.contains("filament")) {
        for (const json &e : j["filament"]) {
            if (!e.is_object() || !e.contains("price_per_kg") || !e["price_per_kg"].is_number())
                continue;
            Entry entry;
            const std::string scope = str_of(e, "scope");
            if (scope == "preset")
                entry.scope = Entry::Scope::Preset;
            else if (scope == "family" || scope.empty())
                entry.scope = Entry::Scope::Family;
            else
                continue;   // a scope from a later version: not ours to apply (and not kept, it has no key we understand)
            entry.preset       = str_of(e, "preset");
            entry.vendor       = str_of(e, "vendor");
            entry.type         = str_of(e, "type");
            entry.family       = str_of(e, "family");
            entry.price_per_kg = e["price_per_kg"].get<double>();
            if (!std::isfinite(entry.price_per_kg) || entry.price_per_kg < 0.)
                continue;
            if (e.contains("updated") && e["updated"].is_number_integer())
                entry.updated = e["updated"].get<long long>();
            if (entry.scope == Entry::Scope::Preset ? entry.preset.empty() : entry.family.empty())
                continue;
            entry.extra = unknown_fields(e, ENTRY_KEYS);
            // A duplicate key (hand edit): the later one wins, as it would on screen.
            auto same = std::find_if(m_entries.begin(), m_entries.end(), [&entry](const Entry &o) {
                return o.scope == entry.scope && (entry.scope == Entry::Scope::Preset ? o.preset == entry.preset : o.key() == entry.key());
            });
            if (same != m_entries.end())
                *same = std::move(entry);
            else
                m_entries.emplace_back(std::move(entry));
        }
    }

    if (j.contains("machine")) {
        for (const json &e : j["machine"]) {
            if (!e.is_object() || !e.contains("rate_per_h") || !e["rate_per_h"].is_number())
                continue;
            MachineEntry entry;
            const std::string scope = str_of(e, "scope");
            if (scope == "preset")
                entry.scope = MachineEntry::Scope::Preset;
            else if (scope == "model" || scope.empty())
                entry.scope = MachineEntry::Scope::Model;
            else
                continue;
            entry.preset     = str_of(e, "preset");
            entry.vendor     = str_of(e, "vendor");
            entry.model      = str_of(e, "model");
            entry.rate_per_h = e["rate_per_h"].get<double>();
            if (!std::isfinite(entry.rate_per_h) || entry.rate_per_h < 0.)
                continue;
            if (e.contains("updated") && e["updated"].is_number_integer())
                entry.updated = e["updated"].get<long long>();
            if (entry.scope == MachineEntry::Scope::Preset ? entry.preset.empty() : entry.model.empty())
                continue;
            entry.extra = unknown_fields(e, MACHINE_KEYS);
            auto same = std::find_if(m_machines.begin(), m_machines.end(), [&entry](const MachineEntry &o) {
                return o.scope == entry.scope && (entry.scope == MachineEntry::Scope::Preset ? o.preset == entry.preset : o.key() == entry.key());
            });
            if (same != m_machines.end())
                *same = std::move(entry);
            else
                m_machines.emplace_back(std::move(entry));
        }
    }
    if (j.contains("machine_default") && j["machine_default"].is_object()) {
        const json &d = j["machine_default"];
        if (d.contains("rate_per_h") && d["rate_per_h"].is_number()) {
            const double rate = d["rate_per_h"].get<double>();
            if (std::isfinite(rate) && rate >= 0.) {
                m_has_default_rate = true;
                m_default_rate     = rate;
                if (d.contains("updated") && d["updated"].is_number_integer())
                    m_default_updated = d["updated"].get<long long>();
                m_default_extra = unknown_fields(d, DEFAULT_KEYS);
            }
        }
    }
    return true;
}

bool Store::save(const std::string &path) const
{
    json j = json::object();
    if (!m_extra.empty()) {
        try {
            json extra = json::parse(m_extra);
            if (extra.is_object())
                for (auto it = extra.begin(); it != extra.end(); ++it)
                    j[it.key()] = it.value();
        } catch (...) {}
    }
    j["version"] = std::max(m_version, int(VERSION));
    json list    = json::array();
    for (const Entry &entry : m_entries) {
        json e = json::object();
        if (!entry.extra.empty()) {
            try {
                json extra = json::parse(entry.extra);
                if (extra.is_object())
                    for (auto it = extra.begin(); it != extra.end(); ++it)
                        e[it.key()] = it.value();
            } catch (...) {}
        }
        e["scope"] = entry.scope == Entry::Scope::Preset ? "preset" : "family";
        if (entry.scope == Entry::Scope::Preset)
            e["preset"] = entry.preset;
        e["vendor"]       = entry.vendor;
        e["type"]         = entry.type;
        e["family"]       = entry.family;
        e["price_per_kg"] = entry.price_per_kg;
        if (entry.updated != 0)
            e["updated"] = entry.updated;
        list.push_back(std::move(e));
    }
    j["filament"] = std::move(list);

    json machines = json::array();
    for (const MachineEntry &entry : m_machines) {
        json e = json::object();
        merge_fields(e, entry.extra);
        e["scope"] = entry.scope == MachineEntry::Scope::Preset ? "preset" : "model";
        if (entry.scope == MachineEntry::Scope::Preset)
            e["preset"] = entry.preset;
        e["vendor"]     = entry.vendor;
        e["model"]      = entry.model;
        e["rate_per_h"] = entry.rate_per_h;
        if (entry.updated != 0)
            e["updated"] = entry.updated;
        machines.push_back(std::move(e));
    }
    j["machine"] = std::move(machines);
    if (m_has_default_rate) {
        json d = json::object();
        merge_fields(d, m_default_extra);
        d["rate_per_h"] = m_default_rate;
        if (m_default_updated != 0)
            d["updated"] = m_default_updated;
        j["machine_default"] = std::move(d);
    }

    try {
        boost::system::error_code ec;
        const fs::path            final(path);
        if (final.has_parent_path())
            fs::create_directories(final.parent_path(), ec);
        const fs::path temp = final.parent_path() / (final.filename().string() + ".tmp");
        {
            boost::nowide::ofstream out(temp.string(), std::ios::binary | std::ios::trunc);
            if (!out.good())
                return false;
            out << j.dump(1) << "\n";
            out.flush();
            if (!out.good())
                return false;
        }
        // rename() replaces the target in one step (MoveFileEx REPLACE_EXISTING on Windows).
        fs::rename(temp, final, ec);
        if (ec) {
            fs::remove(final, ec);
            ec.clear();
            fs::rename(temp, final, ec);
        }
        if (ec) {
            BOOST_LOG_TRIVIAL(warning) << "[CostOverrides] could not write " << path << ": " << ec.message();
            fs::remove(temp, ec);
            return false;
        }
        return true;
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(warning) << "[CostOverrides] could not write " << path << ": " << e.what();
        return false;
    }
}

const Entry *Store::find_family(const std::string &key) const
{
    for (const Entry &e : m_entries)
        if (e.scope == Entry::Scope::Family && e.key() == key)
            return &e;
    return nullptr;
}

const Entry *Store::find_preset(const std::string &preset_name) const
{
    for (const Entry &e : m_entries)
        if (e.scope == Entry::Scope::Preset && e.preset == preset_name)
            return &e;
    return nullptr;
}

static bool valid_price(double p) { return std::isfinite(p) && p >= 0.; }

bool Store::set_family(const std::string &vendor, const std::string &type, const std::string &family, double price_per_kg)
{
    if (!valid_price(price_per_kg) || collapse_spaces(family).empty())
        return false;
    const std::string key = family_key(vendor, type, family);
    Entry *entry = const_cast<Entry *>(find_family(key));
    if (entry == nullptr) {
        m_entries.emplace_back();
        entry        = &m_entries.back();
        entry->scope = Entry::Scope::Family;
    }
    entry->vendor       = collapse_spaces(vendor);
    entry->type         = collapse_spaces(type);
    entry->family       = collapse_spaces(family);
    entry->price_per_kg = price_per_kg;
    entry->updated      = now_s();
    return true;
}

bool Store::set_preset(const std::string &preset_name, const Identity &id, double price_per_kg)
{
    if (!valid_price(price_per_kg) || preset_name.empty())
        return false;
    Entry *entry = const_cast<Entry *>(find_preset(preset_name));
    if (entry == nullptr) {
        m_entries.emplace_back();
        entry         = &m_entries.back();
        entry->scope  = Entry::Scope::Preset;
        entry->preset = preset_name;
    }
    entry->vendor       = id.vendor;
    entry->type         = id.type;
    entry->family       = id.family;
    entry->price_per_kg = price_per_kg;
    entry->updated      = now_s();
    return true;
}

bool Store::clear_family(const std::string &key)
{
    const auto before = m_entries.size();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&key](const Entry &e) { return e.scope == Entry::Scope::Family && e.key() == key; }),
                    m_entries.end());
    return m_entries.size() != before;
}

bool Store::clear_preset(const std::string &preset_name)
{
    const auto before = m_entries.size();
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&preset_name](const Entry &e) { return e.scope == Entry::Scope::Preset && e.preset == preset_name; }),
                    m_entries.end());
    return m_entries.size() != before;
}

Resolved Store::resolve(const Identity &id, double preset_price) const
{
    Resolved r;
    r.preset_price = preset_price;
    r.price        = preset_price;
    if (const Entry *e = id.preset.empty() ? nullptr : find_preset(id.preset)) {
        r.price  = e->price_per_kg;
        r.source = Source::PresetOnly;
        return r;
    }
    const Entry *family = id.family.empty() ? nullptr : find_family(id.key());
    if (id.own_price) {
        r.source = Source::OwnPrice;
        if (family != nullptr) {
            r.family_shadowed = true;
            r.family_price    = family->price_per_kg;
        }
        return r;
    }
    if (family != nullptr) {
        r.price  = family->price_per_kg;
        r.source = Source::Family;
    }
    return r;
}

const MachineEntry *Store::find_machine_model(const std::string &key) const
{
    for (const MachineEntry &e : m_machines)
        if (e.scope == MachineEntry::Scope::Model && e.key() == key)
            return &e;
    return nullptr;
}

const MachineEntry *Store::find_machine_preset(const std::string &preset_name) const
{
    for (const MachineEntry &e : m_machines)
        if (e.scope == MachineEntry::Scope::Preset && e.preset == preset_name)
            return &e;
    return nullptr;
}

bool Store::set_machine_model(const std::string &vendor, const std::string &model, double rate_per_h)
{
    if (!valid_price(rate_per_h) || collapse_spaces(model).empty())
        return false;
    MachineEntry *entry = const_cast<MachineEntry *>(find_machine_model(machine_key(vendor, model)));
    if (entry == nullptr) {
        m_machines.emplace_back();
        entry        = &m_machines.back();
        entry->scope = MachineEntry::Scope::Model;
    }
    entry->vendor     = collapse_spaces(vendor);
    entry->model      = collapse_spaces(model);
    entry->rate_per_h = rate_per_h;
    entry->updated    = now_s();
    return true;
}

bool Store::set_machine_preset(const std::string &preset_name, const MachineIdentity &id, double rate_per_h)
{
    if (!valid_price(rate_per_h) || preset_name.empty())
        return false;
    MachineEntry *entry = const_cast<MachineEntry *>(find_machine_preset(preset_name));
    if (entry == nullptr) {
        m_machines.emplace_back();
        entry         = &m_machines.back();
        entry->scope  = MachineEntry::Scope::Preset;
        entry->preset = preset_name;
    }
    entry->vendor     = id.vendor;
    entry->model      = id.model;
    entry->rate_per_h = rate_per_h;
    entry->updated    = now_s();
    return true;
}

bool Store::clear_machine_model(const std::string &key)
{
    const auto before = m_machines.size();
    m_machines.erase(std::remove_if(m_machines.begin(), m_machines.end(),
                                    [&key](const MachineEntry &e) { return e.scope == MachineEntry::Scope::Model && e.key() == key; }),
                     m_machines.end());
    return m_machines.size() != before;
}

bool Store::clear_machine_preset(const std::string &preset_name)
{
    const auto before = m_machines.size();
    m_machines.erase(std::remove_if(m_machines.begin(), m_machines.end(),
                                    [&preset_name](const MachineEntry &e) {
                                        return e.scope == MachineEntry::Scope::Preset && e.preset == preset_name;
                                    }),
                     m_machines.end());
    return m_machines.size() != before;
}

bool Store::set_default_rate(double rate_per_h)
{
    if (!valid_price(rate_per_h))
        return false;
    m_has_default_rate = true;
    m_default_rate     = rate_per_h;
    m_default_updated  = now_s();
    return true;
}

bool Store::clear_default_rate()
{
    const bool had     = m_has_default_rate;
    m_has_default_rate = false;
    return had;
}

MachineResolved Store::resolve_machine(const MachineIdentity &id, double preset_rate) const
{
    MachineResolved r;
    r.preset_rate = preset_rate;
    r.rate        = preset_rate;
    if (const MachineEntry *e = id.preset.empty() ? nullptr : find_machine_preset(id.preset)) {
        r.rate   = e->rate_per_h;
        r.source = MachineSource::PresetOnly;
        return r;
    }
    const MachineEntry *model = id.model.empty() ? nullptr : find_machine_model(id.key());
    if (id.own_rate) {
        r.source = MachineSource::OwnRate;
        if (model != nullptr || m_has_default_rate) {
            r.shadowed      = true;
            r.shadowed_rate = model != nullptr ? model->rate_per_h : m_default_rate;
        }
        return r;
    }
    if (model != nullptr) {
        r.rate   = model->rate_per_h;
        r.source = MachineSource::Model;
    } else if (m_has_default_rate) {
        r.rate   = m_default_rate;
        r.source = MachineSource::Default;
    }
    return r;
}

// ------------------------------------------------------------ the global one ----

static std::mutex                   s_mutex;
static std::string                  s_path_override;
static std::shared_ptr<const Store> s_global;
static unsigned                     s_revision = 0;

std::string store_path()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_path_override.empty())
        return s_path_override;
    return (fs::path(data_dir()) / "cost" / "filament_overrides.json").string();
}

void set_store_path(const std::string &path)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_path_override = path;
    s_global.reset();
}

std::shared_ptr<const Store> global()
{
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_global)
            return s_global;
    }
    auto store = std::make_shared<Store>();
    const std::string path = store_path();
    if (!store->load(path))
        BOOST_LOG_TRIVIAL(warning) << "[CostOverrides] " << path << " is unreadable (" << store->load_error()
                                   << "); starting without prices of your own. It is kept as .unreadable on the next save.";
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_global)
        s_global = store;
    return s_global;
}

bool save_global(const Store &store)
{
    const std::string path = store_path();
    // A file we could not read is not overwritten without a copy.
    {
        Store probe;
        boost::system::error_code ec;
        if (!probe.load(path) && fs::exists(path, ec)) {
            fs::remove(path + ".unreadable", ec);
            fs::copy_file(path, path + ".unreadable", ec);
        }
    }
    const bool ok = store.save(path);
    std::lock_guard<std::mutex> lock(s_mutex);
    s_global = std::make_shared<Store>(store);
    ++s_revision;
    return ok;
}

void reset_global()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_global.reset();
}

unsigned revision()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_revision;
}

// ------------------------------------------------------------- the funnel ----

std::vector<Resolved> resolve_slots(const DynamicPrintConfig &config, const Store &store, const PresetCollection *filaments)
{
    std::vector<Resolved> out;
    const auto *names = config.option<ConfigOptionStrings>("filament_settings_id");
    const auto *costs = config.option<ConfigOptionFloats>("filament_cost");
    if (names == nullptr || costs == nullptr)
        return out;
    const auto  *inherits = config.option<ConfigOptionStrings>("inherits_group");
    const size_t n        = std::min(names->values.size(), costs->values.size());
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const std::string &name  = names->values[i];
        const double       price = costs->values[i];
        Identity           id;
        const Preset      *preset = filaments != nullptr && !name.empty() ? filaments->find_preset(name, false) : nullptr;
        if (preset != nullptr && preset->name == name) {
            id = identify(*preset, price, filaments);
        } else {
            // Not in the collection (a project the presets of which are not installed): what the config says.
            id.preset = name;
            // inherits_group: [print, filament 1..n, printer].
            const std::string parent = inherits != nullptr && i + 1 < inherits->values.size() ? inherits->values[i + 1] : std::string();
            id.family = family_name(parent.empty() ? name : parent);
        }
        // The slot's vendor / type as they will slice (an unsaved edit included).
        if (const std::string v = first_string(config, "filament_vendor", i); !v.empty())
            id.vendor = v;
        if (const std::string t = first_string(config, "filament_type", i); !t.empty())
            id.type = t;
        out.emplace_back(store.resolve(id, price));
    }
    return out;
}

MachineResolved resolve_machine(const DynamicPrintConfig &config, const Store &store, const PresetCollection *printers)
{
    const double      rate = time_cost_of(config);
    const std::string name = first_string(config, "printer_settings_id");
    MachineIdentity   id;
    const Preset     *preset = printers != nullptr && !name.empty() ? printers->find_preset(name, false) : nullptr;
    if (preset != nullptr && preset->name == name) {
        id = identify_machine(*preset, rate, printers);
    } else {
        // Not in the collection: what the config says (no vendor, no parent).
        id.preset = name;
        id.model  = collapse_spaces(first_string(config, "printer_model"));
        if (id.model.empty()) {
            const auto       *inherits = config.option<ConfigOptionStrings>("inherits_group");
            const std::string parent   = inherits != nullptr && !inherits->values.empty() ? inherits->values.back() : std::string();
            id.model                   = printer_family_name(parent.empty() ? name : parent);
        }
    }
    return store.resolve_machine(id, rate);
}

std::vector<Resolved> apply(DynamicPrintConfig &config, const Store &store, const PresetCollection *filaments,
                            const PresetCollection *printers)
{
    std::vector<Resolved> slots = resolve_slots(config, store, filaments);
    if (store.empty())
        return slots;
    if (auto *costs = config.option<ConfigOptionFloats>("filament_cost"); costs != nullptr)
        for (size_t i = 0; i < slots.size() && i < costs->values.size(); ++i)
            costs->values[i] = slots[i].price;
    if (!store.machines().empty() || store.has_default_rate())
        if (auto *time_cost = config.option<ConfigOptionFloat>("time_cost"); time_cost != nullptr)
            time_cost->value = resolve_machine(config, store, printers).rate;
    return slots;
}

void strip_prices(DynamicPrintConfig &config)
{
    config.erase("filament_cost");
    config.erase("time_cost");
    if (auto *diff = config.option<ConfigOptionStrings>("different_settings_to_system"); diff != nullptr) {
        for (std::string &entry : diff->values) {
            std::vector<std::string> keys;
            if (!unescape_strings_cstyle(entry, keys))
                continue;
            const auto end = std::remove_if(keys.begin(), keys.end(),
                                            [](const std::string &k) { return k == "filament_cost" || k == "time_cost"; });
            if (end == keys.end())
                continue;
            keys.erase(end, keys.end());
            entry = keys.empty() ? std::string() : escape_strings_cstyle(keys);
        }
    }
}

} // namespace CostOverrides
} // namespace Slic3r
