#include "FFPrinterSources.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace Slic3r { namespace GUI {

namespace {

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b])) ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

bool is_ipv4(const std::string& s)
{
    int    parts = 0;
    size_t i     = 0;
    while (i <= s.size()) {
        size_t j = s.find('.', i);
        if (j == std::string::npos) j = s.size();
        const std::string part = s.substr(i, j - i);
        if (part.empty() || part.size() > 3) return false;
        int v = 0;
        for (char c : part) {
            if (!std::isdigit((unsigned char) c)) return false;
            v = v * 10 + (c - '0');
        }
        if (v > 255) return false;
        ++parts;
        if (j == s.size()) break;
        i = j + 1;
    }
    return parts == 4;
}

int completeness(const FFPrinterEntry& e) { return 3 - (int) e.missing.size(); }

} // namespace

std::string FFPrinterEntry::missing_text() const
{
    std::string out;
    for (size_t i = 0; i < missing.size(); ++i) {
        if (i > 0) out += (i + 1 == missing.size()) ? " and " : ", ";
        out += missing[i];
    }
    return out;
}

bool ff_parse_address(const std::string& address, std::string& ip, unsigned short& port)
{
    std::string s      = trim(address);
    const size_t scheme = s.find("://");
    if (scheme != std::string::npos) s = s.substr(scheme + 3);
    const size_t slash = s.find_first_of("/?#");
    if (slash != std::string::npos) s = s.substr(0, slash);
    port               = 8898;
    const size_t colon = s.rfind(':');
    if (colon != std::string::npos) {
        const std::string p = s.substr(colon + 1);
        s                   = s.substr(0, colon);
        if (!p.empty() && p.size() <= 5 && std::all_of(p.begin(), p.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
            const int v = std::stoi(p);
            if (v > 0 && v < 65536 && v != 8899) port = (unsigned short) v;
        }
    }
    s = trim(s);
    if (!is_ipv4(s)) return false;
    ip = s;
    return true;
}

std::vector<FFPrinterEntry> ff_merge_printer_sources(const std::vector<FFPrinterSource>& sources)
{
    std::vector<FFPrinterEntry> candidates;
    for (const FFPrinterSource& src : sources) {
        FFPrinterEntry e;
        e.serial                  = trim(src.serial);
        e.check_code              = trim(src.check_code);
        e.origin                  = src.origin;
        const std::string address = trim(src.address);
        const std::string name    = trim(src.name);
        e.name                    = name.empty() ? (address.empty() ? e.serial : address) : name;
        if (address.empty() && e.serial.empty() && e.check_code.empty())
            continue; // not a printer
        const bool ip_ok = !address.empty() && ff_parse_address(address, e.ip, e.port);
        if (!ip_ok) e.missing.push_back(address.empty() ? "IP address" : "IP address (a host name does not work)");
        if (e.serial.empty()) e.missing.push_back("serial number");
        if (e.check_code.empty()) e.missing.push_back("check code");
        e.state = e.missing.empty() ? FFPrinterState::Ready : FFPrinterState::NeedsSetup;
        e.key   = !e.serial.empty() ? e.serial : !e.ip.empty() ? "addr:" + e.ip : "name:" + e.name;
        candidates.push_back(std::move(e));
    }

    // Best first: most complete, then by name, then origin - so the winner of a tie is a property
    // of the entries, not of the order they were listed in.
    std::stable_sort(candidates.begin(), candidates.end(), [](const FFPrinterEntry& a, const FFPrinterEntry& b) {
        if (completeness(a) != completeness(b)) return completeness(a) > completeness(b);
        if (lower(a.name) != lower(b.name)) return lower(a.name) < lower(b.name);
        return a.origin < b.origin;
    });

    std::map<std::string, FFPrinterEntry> by_key;
    for (FFPrinterEntry& e : candidates)
        by_key.emplace(e.key, std::move(e)); // emplace keeps the first, i.e. the best

    // A serial-less entry at the address of a printer that has a serial is that printer.
    std::set<std::string> serial_ips;
    for (const auto& kv : by_key)
        if (!kv.second.serial.empty() && !kv.second.ip.empty()) serial_ips.insert(kv.second.ip);

    std::vector<FFPrinterEntry> out;
    for (auto& kv : by_key) {
        const FFPrinterEntry& e = kv.second;
        if (e.serial.empty() && !e.ip.empty() && serial_ips.count(e.ip)) continue;
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const FFPrinterEntry& a, const FFPrinterEntry& b) {
        if (lower(a.name) != lower(b.name)) return lower(a.name) < lower(b.name);
        return a.key < b.key;
    });
    return out;
}

std::vector<std::string> ff_stale_setting_tiles(const std::set<std::string>&       previous,
                                                const std::vector<FFPrinterEntry>& now,
                                                const std::set<std::string>&       saved_serials)
{
    std::set<std::string> current;
    for (const FFPrinterEntry& e : now) current.insert(e.key);
    std::vector<std::string> stale;
    for (const std::string& key : previous)
        if (!current.count(key) && !saved_serials.count(key)) stale.push_back(key);
    return stale;
}

}} // namespace Slic3r::GUI
