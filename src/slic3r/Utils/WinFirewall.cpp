#include "WinFirewall.hpp"

#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <chrono>
#include <cwctype>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <netfw.h>
#include <wrl/client.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace Slic3r {
namespace WinFirewall {

namespace fs = boost::filesystem;

const char* const RULE_HUB       = "EdgeSlicer";
const char* const RULE_DISCOVERY = "EdgeSlicer LAN discovery";
const char* const RULE_WEBRTC    = "EdgeSlicer WebRTC video";
const char* const RULE_FLASHFORGE = "EdgeSlicer FlashForge discovery";

static const char* const RULE_DESCRIPTION = "Added by EdgeSlicer (Help > Check Windows Firewall).";

static const int PROFILE_BITS[3] = { ProfileDomain, ProfilePrivate, ProfilePublic };

int profile_index(int bit)
{
    switch (bit) {
    case ProfileDomain: return 0;
    case ProfilePrivate: return 1;
    case ProfilePublic: return 2;
    default: return -1;
    }
}

std::string profiles_text(int profiles)
{
    static const char* const names[3] = { "Domain", "Private", "Public" };
    std::string out;
    for (int i = 0; i < 3; ++i)
        if (profiles & PROFILE_BITS[i]) {
            if (!out.empty()) out += ", ";
            out += names[i];
        }
    return out;
}

static std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace((unsigned char) s[b])) ++b;
    while (e > b && std::isspace((unsigned char) s[e - 1])) --e;
    return s.substr(b, e - b);
}

// ---- paths ----------------------------------------------------------------------------------

static bool process_env(const std::string& name, std::string& value)
{
    const char* v = boost::nowide::getenv(name.c_str());
    if (v == nullptr) return false;
    value = v;
    return true;
}

static std::string expand_env(const std::string& s, const EnvLookup& env)
{
    std::string out;
    size_t      i = 0;
    while (i < s.size()) {
        const size_t open = s.find('%', i);
        if (open == std::string::npos) { out += s.substr(i); break; }
        const size_t close = s.find('%', open + 1);
        if (close == std::string::npos) { out += s.substr(i); break; }
        out += s.substr(i, open - i);
        const std::string name = s.substr(open + 1, close - open - 1);
        std::string       value;
        const bool        found = !name.empty() && (env ? env(name, value) : process_env(name, value));
        if (found) {
            out += value;
            i = close + 1;
        } else {
            // Not a variable: keep the first '%' and look for a variable starting at the second.
            out += '%';
            i = open + 1;
        }
    }
    return out;
}

static std::string to_lower_path(const std::string& s)
{
    std::wstring w = boost::nowide::widen(s);
#ifdef _WIN32
    if (!w.empty()) ::CharLowerBuffW(&w[0], (DWORD) w.size());
#else
    for (wchar_t& c : w) c = (wchar_t) std::towlower(c);
#endif
    return boost::nowide::narrow(w);
}

std::string normalize_program_path(const std::string& path, const EnvLookup& env)
{
    // Quotes are never part of a Windows path; rules written by hand often carry them.
    std::string s;
    for (char c : path)
        if (c != '"') s += c;
    s = trim(expand_env(trim(s), env));
    std::replace(s.begin(), s.end(), '/', '\\');

    if (s.compare(0, 8, "\\\\?\\UNC\\") == 0) s = "\\\\" + s.substr(8);
    else if (s.compare(0, 4, "\\\\?\\") == 0) s = s.substr(4);

    const bool unc = s.compare(0, 2, "\\\\") == 0;
    std::vector<std::string> parts;
    std::string              part;
    std::istringstream       is(s);
    while (std::getline(is, part, '\\')) {
        part = trim(part);
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            // Never pop a drive ("c:") or, for UNC, the server and share.
            const size_t floor = unc ? 2 : (!parts.empty() && parts[0].find(':') != std::string::npos ? 1 : 0);
            if (parts.size() > floor) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out = unc ? "\\\\" : "";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '\\';
        out += parts[i];
    }
    return to_lower_path(out);
}

bool same_program(const std::string& a, const std::string& b, const EnvLookup& env)
{
    const std::string na = normalize_program_path(a, env);
    return !na.empty() && na == normalize_program_path(b, env);
}

// ---- ports ----------------------------------------------------------------------------------

static bool parse_int(const std::string& s, int& out)
{
    const std::string t = trim(s);
    if (t.empty() || t.size() > 5) return false;
    for (char c : t)
        if (!std::isdigit((unsigned char) c)) return false;
    out = std::stoi(t);
    return out >= 0 && out <= 65535;
}

static bool any_ports(const std::string& rule_ports)
{
    const std::string t = trim(rule_ports);
    return t.empty() || t == "*" || to_lower_path(t) == "any";
}

bool ports_cover(const std::string& rule_ports, int port)
{
    if (any_ports(rule_ports)) return true;
    std::istringstream is(rule_ports);
    std::string        item;
    while (std::getline(is, item, ',')) {
        const size_t dash = item.find('-');
        int          lo = 0, hi = 0;
        if (dash == std::string::npos) {
            if (parse_int(item, lo) && lo == port) return true;
        } else if (parse_int(item.substr(0, dash), lo) && parse_int(item.substr(dash + 1), hi)) {
            if (port >= std::min(lo, hi) && port <= std::max(lo, hi)) return true;
        }
    }
    return false;
}

std::vector<int> expand_ports(const std::string& ports)
{
    std::vector<int>   out;
    std::istringstream is(ports);
    std::string        item;
    while (std::getline(is, item, ',')) {
        const size_t dash = item.find('-');
        int          lo = 0, hi = 0;
        if (dash == std::string::npos) {
            if (parse_int(item, lo)) out.push_back(lo);
        } else if (parse_int(item.substr(0, dash), lo) && parse_int(item.substr(dash + 1), hi)) {
            for (int p = std::min(lo, hi); p <= std::max(lo, hi); ++p) out.push_back(p);
        }
    }
    return out;
}

bool protocol_covers(int rule_protocol, int protocol)
{
    return rule_protocol == ProtoAny || rule_protocol == protocol;
}

static bool same_port_set(const std::string& a, const std::string& b)
{
    if (any_ports(a) || any_ports(b)) return any_ports(a) && any_ports(b);
    std::vector<int> x = expand_ports(a), y = expand_ports(b);
    std::sort(x.begin(), x.end());
    std::sort(y.begin(), y.end());
    x.erase(std::unique(x.begin(), x.end()), x.end());
    y.erase(std::unique(y.begin(), y.end()), y.end());
    return !x.empty() && x == y;
}

// ---- what EdgeSlicer needs ------------------------------------------------------------------

std::vector<ExpectedRule> expected_rules(const std::string& exe, const std::string& go2rtc)
{
    // Mirrors cmake/nsis/SnapmakerURLProtocols_install.nsh; keep the two in step.
    std::vector<ExpectedRule> out;
    out.push_back({ RULE_DISCOVERY, exe, ProtoUDP, "2021,1990", "discovery" });
    out.push_back({ RULE_HUB, exe, ProtoTCP, "13640-13659", "hub" });
    if (!go2rtc.empty()) {
        out.push_back({ RULE_WEBRTC, go2rtc, ProtoUDP, "8555-8574", "webrtc" });
        out.push_back({ RULE_WEBRTC, go2rtc, ProtoTCP, "8555-8574", "webrtc" });
    }
    // FlashForge's LAN search (Flashforge::discover_printers) broadcasts to UDP 48899 from, and
    // listens on, UDP 18007; the printers answer to 18007, which Windows Firewall drops unless
    // there is an inbound rule for it. Kept last so the other rules keep their positions.
    out.push_back({ RULE_FLASHFORGE, exe, ProtoUDP, "18007", "flashforge" });
    return out;
}

static int live_problems(const Diagnosis& d, bool discovery_only)
{
    int out = 0;
    for (int bit : PROFILE_BITS) {
        if (!(d.current_profiles & bit)) continue;
        const int idx = profile_index(bit);
        if (!d.firewall_on[idx]) continue;
        if (d.block_all_inbound[idx]) { out |= bit; continue; }
        for (const ExpectedStatus& e : d.expected) {
            if (discovery_only && e.rule.purpose != "discovery") continue;
            if ((e.blocked_profiles & bit) || !(e.allowed_profiles & bit)) out |= bit;
        }
    }
    return out;
}

int Diagnosis::problem_profiles() const { return ok ? live_problems(*this, false) : 0; }
int Diagnosis::discovery_problem_profiles() const { return ok ? live_problems(*this, true) : 0; }

int Diagnosis::enabled_blocks() const
{
    int n = 0;
    for (const Rule& r : blocks)
        if (r.enabled) ++n;
    return n;
}

bool Diagnosis::on_public_network() const { return (current_profiles & ProfilePublic) && firewall_on[2]; }

bool Diagnosis::fix_would_help(bool allow_public) const
{
    if (!ok) return false;
    if (!blocks.empty()) return true; // the fix removes every Block rule for our programs
    const int target = ProfilePrivate | ProfileDomain | (allow_public ? ProfilePublic : 0);
    for (const ExpectedStatus& e : expected)
        if ((e.allowed_profiles & target) != target) return true;
    return false;
}

Diagnosis diagnose(const Snapshot& s, const std::string& exe, const std::string& go2rtc, const EnvLookup& env)
{
    Diagnosis d;
    d.exe              = exe;
    d.go2rtc           = go2rtc;
    d.ok               = s.ok;
    d.error            = s.error;
    d.current_profiles = s.current_profiles & ProfileAll;
    for (int i = 0; i < 3; ++i) {
        d.firewall_on[i]       = s.firewall_on[i];
        d.block_all_inbound[i] = s.block_all_inbound[i];
    }
    if (!s.ok) return d;

    // Rules for our two programs only, normalised once.
    std::vector<const Rule*> mine;
    const std::string        n_exe = normalize_program_path(exe, env);
    const std::string        n_go  = go2rtc.empty() ? std::string() : normalize_program_path(go2rtc, env);
    std::vector<std::string> n_prog;
    for (const Rule& r : s.rules) {
        const std::string p = normalize_program_path(r.program, env);
        if (p.empty() || (p != n_exe && p != n_go)) continue;
        mine.push_back(&r);
        n_prog.push_back(p);
        if (!r.allow) d.blocks.push_back(r);
    }

    for (const ExpectedRule& e : expected_rules(exe, go2rtc)) {
        ExpectedStatus st;
        st.rule                   = e;
        const std::string n_e     = normalize_program_path(e.program, env);
        const std::vector<int> ps = expand_ports(e.ports);
        int allowed = ps.empty() ? 0 : ProfileAll;
        for (int port : ps) {
            int mask = 0;
            for (size_t i = 0; i < mine.size(); ++i) {
                const Rule& r = *mine[i];
                if (n_prog[i] != n_e || !r.inbound || !r.enabled || !protocol_covers(r.protocol, e.protocol) || !ports_cover(r.local_ports, port))
                    continue;
                if (r.allow) mask |= r.profiles & ProfileAll;
                else st.blocked_profiles |= r.profiles & ProfileAll;
            }
            allowed &= mask;
        }
        st.allowed_profiles = allowed;
        d.expected.push_back(st);
    }
    return d;
}

static const char* protocol_name(int p)
{
    return p == ProtoTCP ? "TCP" : p == ProtoUDP ? "UDP" : p == ProtoAny ? "any protocol" : "other protocol";
}

static std::string or_none(const std::string& s) { return s.empty() ? std::string("none") : s; }

std::vector<std::string> describe_for_log(const Diagnosis& d)
{
    std::vector<std::string> out;
    out.push_back("exe: " + d.exe);
    out.push_back("go2rtc: " + (d.go2rtc.empty() ? std::string("not found next to this exe") : d.go2rtc));
    if (!d.ok) {
        out.push_back("could not read Windows Firewall: " + d.error);
        return out;
    }
    std::string on, block_all;
    for (int i = 0; i < 3; ++i) {
        if (d.firewall_on[i]) on += (on.empty() ? "" : ", ") + profiles_text(PROFILE_BITS[i]);
        if (d.block_all_inbound[i]) block_all += (block_all.empty() ? "" : ", ") + profiles_text(PROFILE_BITS[i]);
    }
    out.push_back("network profile now: " + or_none(profiles_text(d.current_profiles)) + "; firewall on for: " + or_none(on) +
                  "; block all incoming: " + or_none(block_all));
    for (const ExpectedStatus& e : d.expected)
        out.push_back("rule '" + e.rule.name + "' (" + protocol_name(e.rule.protocol) + " " + e.rule.ports + "): allowed on " +
                      or_none(profiles_text(e.allowed_profiles)) + "; blocked on " + or_none(profiles_text(e.blocked_profiles)));
    for (const Rule& r : d.blocks)
        out.push_back("block rule '" + r.name + "' (" + (r.inbound ? "inbound" : "outbound") + ", " + (r.enabled ? "enabled" : "disabled") + ", " +
                      protocol_name(r.protocol) + " " + (r.local_ports.empty() ? std::string("*") : r.local_ports) + ", " +
                      or_none(profiles_text(r.profiles)) + ") program " + r.program);
    const int p = d.problem_profiles();
    out.push_back(std::string("verdict: ") + (d.has_problems() ? "problems" : "ok") + "; affected live profiles: " + or_none(profiles_text(p)) +
                  "; discovery affected on: " + or_none(profiles_text(d.discovery_problem_profiles())) +
                  "; enabled block rules: " + std::to_string(d.enabled_blocks()));
    return out;
}

// ---- the fix --------------------------------------------------------------------------------

FixPlan plan_fix(const Snapshot& s, const std::string& exe, const std::string& go2rtc, bool allow_public, const EnvLookup& env)
{
    FixPlan plan;
    const std::vector<ExpectedRule> expected = expected_rules(exe, go2rtc);
    const std::string n_exe = normalize_program_path(exe, env);
    const std::string n_go  = go2rtc.empty() ? std::string() : normalize_program_path(go2rtc, env);
    if (s.ok && !n_exe.empty()) {
        for (const Rule& r : s.rules) {
            const std::string p = normalize_program_path(r.program, env);
            if (p.empty() || (p != n_exe && p != n_go)) continue;
            if (!r.allow) {
                plan.remove.push_back(r); // every Block rule for our programs
                continue;
            }
            // Our own allow rules (installer name, same program, protocol and ports) are recreated
            // below; anything else allowing this exe - a broader rule the user made, Windows' own
            // rule from an "Allow" click - is left alone.
            if (!r.inbound) continue;
            for (const ExpectedRule& e : expected)
                if (r.name == e.name && p == normalize_program_path(e.program, env) && r.protocol == e.protocol &&
                    same_port_set(r.local_ports, e.ports)) {
                    plan.remove.push_back(r);
                    break;
                }
        }
    }
    for (const ExpectedRule& e : expected) {
        Rule r;
        r.name        = e.name;
        r.program     = e.program;
        r.local_ports = e.ports;
        r.description = RULE_DESCRIPTION;
        r.protocol    = e.protocol;
        r.profiles    = ProfilePrivate | ProfileDomain | (allow_public ? ProfilePublic : 0);
        r.inbound     = true;
        r.enabled     = true;
        r.allow       = true;
        plan.add.push_back(r);
    }
    return plan;
}

static std::string rule_words(const Rule& r)
{
    return "'" + r.name + "' (" + (r.allow ? "allow" : "block") + ", " + (r.inbound ? "inbound" : "outbound") + ", " + protocol_name(r.protocol) +
           " " + (r.local_ports.empty() ? std::string("*") : r.local_ports) + ", " + or_none(profiles_text(r.profiles)) + ", program " + r.program + ")";
}

FixResult apply_fix(Backend& backend, const FixPlan& plan)
{
    FixResult res;
    auto step = [&res](const std::string& s) {
        BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: " << s;
        res.steps.push_back(s);
    };
    step("plan: remove " + std::to_string(plan.remove.size()) + " rule(s), add " + std::to_string(plan.add.size()) + " rule(s)");
    for (const Rule& r : plan.remove) {
        std::string err;
        if (backend.remove(r, err)) {
            ++res.removed;
            step("removed " + rule_words(r));
        } else {
            ++res.failed;
            step("FAILED to remove " + rule_words(r) + ": " + err);
        }
    }
    for (const Rule& r : plan.add) {
        std::string err;
        if (backend.add(r, err)) {
            ++res.added;
            step("added " + rule_words(r));
        } else {
            ++res.failed;
            step("FAILED to add " + rule_words(r) + ": " + err);
        }
    }
    step("done: removed " + std::to_string(res.removed) + ", added " + std::to_string(res.added) + ", failed " + std::to_string(res.failed));
    return res;
}

// ---- the real firewall ------------------------------------------------------------------------

#ifdef _WIN32
using Microsoft::WRL::ComPtr;

namespace {

// COM for the calling thread: the UI thread already has an STA (S_FALSE, still balanced), a
// detached thread gets one here. A thread already in the MTA cannot switch; COM still works there.
struct ComScope
{
    HRESULT hr;
    ComScope() : hr(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}
    ~ComScope()
    {
        if (SUCCEEDED(hr)) ::CoUninitialize();
    }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

std::string hr_text(HRESULT hr)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%08lX", (unsigned long) hr);
    std::string out = buf;
    if (hr == E_ACCESSDENIED) out += " (access denied - needs administrator rights)";
    return out;
}

std::string take_bstr(BSTR b)
{
    if (b == nullptr) return {};
    std::string s = boost::nowide::narrow(std::wstring(b, ::SysStringLen(b)));
    ::SysFreeString(b);
    return s;
}

struct Bstr
{
    BSTR b;
    explicit Bstr(const std::string& s) : b(::SysAllocString(boost::nowide::widen(s).c_str())) {}
    ~Bstr() { ::SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
};

HRESULT open_policy(ComPtr<INetFwPolicy2>& pol)
{
    return ::CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pol));
}

Rule to_rule(INetFwRule* fr)
{
    Rule r;
    BSTR b = nullptr;
    if (SUCCEEDED(fr->get_Name(&b))) r.name = take_bstr(b);
    b = nullptr;
    if (SUCCEEDED(fr->get_ApplicationName(&b))) r.program = take_bstr(b);
    b = nullptr;
    if (SUCCEEDED(fr->get_LocalPorts(&b))) r.local_ports = take_bstr(b);
    b = nullptr;
    if (SUCCEEDED(fr->get_Description(&b))) r.description = take_bstr(b);
    long l = 0;
    if (SUCCEEDED(fr->get_Protocol(&l))) r.protocol = (int) l;
    l = 0;
    if (SUCCEEDED(fr->get_Profiles(&l))) r.profiles = (int) (l & ProfileAll);
    NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_IN;
    if (SUCCEEDED(fr->get_Direction(&dir))) r.inbound = dir == NET_FW_RULE_DIR_IN;
    VARIANT_BOOL en = VARIANT_FALSE;
    if (SUCCEEDED(fr->get_Enabled(&en))) r.enabled = en != VARIANT_FALSE;
    NET_FW_ACTION act = NET_FW_ACTION_ALLOW;
    if (SUCCEEDED(fr->get_Action(&act))) r.allow = act == NET_FW_ACTION_ALLOW;
    return r;
}

// Calls `fn(INetFwRule*, const Rule&)` for every rule until it returns true.
template<class Fn> HRESULT for_each_rule(INetFwRules* rules, Fn&& fn)
{
    ComPtr<IUnknown> unk;
    HRESULT          hr = rules->get__NewEnum(&unk);
    if (FAILED(hr)) return hr;
    ComPtr<IEnumVARIANT> en;
    hr = unk.As(&en);
    if (FAILED(hr)) return hr;
    VARIANT v;
    ::VariantInit(&v);
    ULONG fetched = 0;
    while (en->Next(1, &v, &fetched) == S_OK && fetched == 1) {
        bool stop = false;
        if (v.vt == VT_DISPATCH && v.pdispVal != nullptr) {
            ComPtr<INetFwRule> fr;
            if (SUCCEEDED(v.pdispVal->QueryInterface(IID_PPV_ARGS(&fr))) && fr) stop = fn(fr.Get(), to_rule(fr.Get()));
        }
        ::VariantClear(&v);
        if (stop) break;
    }
    return S_OK;
}

bool same_rule(const Rule& a, const Rule& b)
{
    return a.name == b.name && same_program(a.program, b.program) && a.inbound == b.inbound && a.allow == b.allow &&
           a.protocol == b.protocol && a.local_ports == b.local_ports && (a.profiles & ProfileAll) == (b.profiles & ProfileAll) &&
           a.enabled == b.enabled;
}

class ComBackend : public Backend
{
public:
    Snapshot read() override
    {
        Snapshot s;
        ComScope com;
        if (!com.usable()) {
            s.error = "COM could not start: " + hr_text(com.hr);
            return s;
        }
        ComPtr<INetFwPolicy2> pol;
        HRESULT hr = open_policy(pol);
        if (FAILED(hr)) {
            s.error = "INetFwPolicy2 unavailable: " + hr_text(hr);
            return s;
        }
        long cur = 0;
        if (SUCCEEDED(pol->get_CurrentProfileTypes(&cur))) s.current_profiles = (int) (cur & ProfileAll);
        for (int i = 0; i < 3; ++i) {
            VARIANT_BOOL v = VARIANT_TRUE;
            if (SUCCEEDED(pol->get_FirewallEnabled((NET_FW_PROFILE_TYPE2) PROFILE_BITS[i], &v))) s.firewall_on[i] = v != VARIANT_FALSE;
            v = VARIANT_FALSE;
            if (SUCCEEDED(pol->get_BlockAllInboundTraffic((NET_FW_PROFILE_TYPE2) PROFILE_BITS[i], &v))) s.block_all_inbound[i] = v != VARIANT_FALSE;
        }
        ComPtr<INetFwRules> rules;
        hr = pol->get_Rules(&rules);
        if (FAILED(hr)) {
            s.error = "the rule list could not be read: " + hr_text(hr);
            return s;
        }
        hr = for_each_rule(rules.Get(), [&s](INetFwRule*, const Rule& r) {
            s.rules.push_back(r);
            return false;
        });
        if (FAILED(hr)) {
            s.error = "the rule list could not be enumerated: " + hr_text(hr);
            return s;
        }
        s.ok = true;
        return s;
    }

    // INetFwRules::Remove goes by name, and Windows' own prompt rules share names (one TCP and
    // one UDP rule per program, and often our installer's "EdgeSlicer" too). So the exact rule is
    // renamed to a name nobody else has first, and that name is removed.
    bool remove(const Rule& target, std::string& error) override
    {
        ComScope com;
        if (!com.usable()) { error = "COM could not start: " + hr_text(com.hr); return false; }
        ComPtr<INetFwPolicy2> pol;
        HRESULT hr = open_policy(pol);
        if (FAILED(hr)) { error = "INetFwPolicy2 unavailable: " + hr_text(hr); return false; }
        ComPtr<INetFwRules> rules;
        hr = pol->get_Rules(&rules);
        if (FAILED(hr)) { error = "the rule list could not be read: " + hr_text(hr); return false; }

        const std::string unique = "EdgeSlicer firewall fix (removing) " +
            std::to_string(::GetCurrentProcessId()) + "-" + std::to_string(::GetTickCount64()) + "-" + std::to_string(++m_serial);
        bool    found = false;
        HRESULT rename_hr = S_OK;
        for_each_rule(rules.Get(), [&](INetFwRule* fr, const Rule& r) {
            if (!same_rule(r, target)) return false;
            found = true;
            Bstr name(unique);
            rename_hr = fr->put_Name(name.b);
            return true;
        });
        if (!found) { error = "the rule is no longer there"; return false; }
        if (FAILED(rename_hr)) { error = hr_text(rename_hr); return false; }
        Bstr name(unique);
        hr = rules->Remove(name.b);
        if (FAILED(hr)) {
            error = hr_text(hr);
            // Put the original name back rather than leave a renamed rule behind.
            for_each_rule(rules.Get(), [&](INetFwRule* fr, const Rule& r) {
                if (r.name != unique) return false;
                Bstr orig(target.name);
                fr->put_Name(orig.b);
                return true;
            });
            return false;
        }
        return true;
    }

    bool add(const Rule& r, std::string& error) override
    {
        ComScope com;
        if (!com.usable()) { error = "COM could not start: " + hr_text(com.hr); return false; }
        ComPtr<INetFwPolicy2> pol;
        HRESULT hr = open_policy(pol);
        if (FAILED(hr)) { error = "INetFwPolicy2 unavailable: " + hr_text(hr); return false; }
        ComPtr<INetFwRules> rules;
        hr = pol->get_Rules(&rules);
        if (FAILED(hr)) { error = "the rule list could not be read: " + hr_text(hr); return false; }
        ComPtr<INetFwRule> fr;
        hr = ::CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fr));
        if (FAILED(hr)) { error = "NetFwRule unavailable: " + hr_text(hr); return false; }
        Bstr name(r.name), desc(r.description), app(r.program), ports(r.local_ports), ifaces("All");
        // Protocol before ports: LocalPorts is refused while the protocol is "any".
        if (FAILED(hr = fr->put_Name(name.b)) || FAILED(hr = fr->put_Description(desc.b)) || FAILED(hr = fr->put_ApplicationName(app.b)) ||
            FAILED(hr = fr->put_Protocol(r.protocol)) || (!r.local_ports.empty() && FAILED(hr = fr->put_LocalPorts(ports.b))) ||
            FAILED(hr = fr->put_Direction(r.inbound ? NET_FW_RULE_DIR_IN : NET_FW_RULE_DIR_OUT)) ||
            FAILED(hr = fr->put_Action(r.allow ? NET_FW_ACTION_ALLOW : NET_FW_ACTION_BLOCK)) || FAILED(hr = fr->put_Profiles(r.profiles & ProfileAll)) ||
            FAILED(hr = fr->put_InterfaceTypes(ifaces.b)) || FAILED(hr = fr->put_Enabled(r.enabled ? VARIANT_TRUE : VARIANT_FALSE))) {
            error = "the rule could not be built: " + hr_text(hr);
            return false;
        }
        hr = rules->Add(fr.Get());
        if (FAILED(hr)) { error = hr_text(hr); return false; }
        return true;
    }

private:
    int m_serial { 0 };
};

} // namespace

std::unique_ptr<Backend> make_system_backend() { return std::make_unique<ComBackend>(); }

#else // !_WIN32

namespace {
class NoBackend : public Backend
{
public:
    Snapshot read() override
    {
        Snapshot s;
        s.error = "Windows Firewall exists only on Windows";
        return s;
    }
    bool remove(const Rule&, std::string& error) override { error = "not Windows"; return false; }
    bool add(const Rule&, std::string& error) override { error = "not Windows"; return false; }
};
} // namespace

std::unique_ptr<Backend> make_system_backend() { return std::make_unique<NoBackend>(); }

#endif

Snapshot read_system_snapshot() { return make_system_backend()->read(); }

// ---- this copy --------------------------------------------------------------------------------

std::string this_exe_path()
{
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD  n = ::GetModuleFileNameW(nullptr, &buf[0], (DWORD) buf.size());
    if (n == 0 || n >= buf.size()) return fs::path(boost::dll::program_location()).make_preferred().string();
    buf.resize(n);
    // Rules carry long names; a launch through an 8.3 path would otherwise never match.
    std::wstring longer(32768, L'\0');
    const DWORD  m = ::GetLongPathNameW(buf.c_str(), &longer[0], (DWORD) longer.size());
    if (m > 0 && m < longer.size()) {
        longer.resize(m);
        buf = longer;
    }
    return boost::nowide::narrow(buf);
#else
    return boost::dll::program_location().string();
#endif
}

std::string go2rtc_path_for(const std::string& exe)
{
    if (exe.empty()) return {};
    // Through wide strings: this also runs in the elevated helper, before anything has switched
    // boost::filesystem to UTF-8.
    fs::path p = fs::path(boost::nowide::widen(exe)).parent_path() / L"resources" / L"tools" / L"go2rtc" / L"go2rtc.exe";
    boost::system::error_code ec;
    if (!fs::exists(p, ec)) return {};
    p.make_preferred();
    return boost::nowide::narrow(p.wstring());
}

Diagnosis diagnose_this_copy()
{
    const std::string exe = this_exe_path();
    const auto        t0  = std::chrono::steady_clock::now();
    Diagnosis         d   = diagnose(read_system_snapshot(), exe, go2rtc_path_for(exe));
    const auto        ms  = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    BOOST_LOG_TRIVIAL(warning) << "[Firewall] check (" << ms << " ms):";
    for (const std::string& line : describe_for_log(d)) BOOST_LOG_TRIVIAL(warning) << "[Firewall]   " << line;
    return d;
}

// ---- elevated helper --------------------------------------------------------------------------

bool is_fix_cli(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (argv[i] != nullptr && std::string(argv[i]) == "--fix-firewall") return true;
    return false;
}

bool is_valid_fix_log_path(const std::string& path)
{
    if (path.empty()) return false;
    const fs::path p(boost::nowide::widen(path));
    if (!p.is_absolute()) return false;
    const std::string name   = boost::nowide::narrow(p.filename().wstring());
    const std::string prefix = "edgeslicer_firewall_fix_";
    const std::string suffix = ".log";
    if (name.size() <= prefix.size() + suffix.size()) return false;
    if (to_lower_path(name.substr(0, prefix.size())) != prefix) return false;
    if (to_lower_path(name.substr(name.size() - suffix.size())) != suffix) return false;
    for (size_t i = prefix.size(); i < name.size() - suffix.size(); ++i)
        if (!std::isdigit((unsigned char) name[i])) return false;
    return true;
}

int run_fix_cli(int argc, char** argv)
{
    bool        allow_public = false;
    std::string log_path;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i] ? argv[i] : "";
        if (a == "--public") allow_public = true;
        else if (a == "--fix-firewall-log" && i + 1 < argc) log_path = argv[++i] ? argv[i] : "";
    }
    if (!log_path.empty() && !is_valid_fix_log_path(log_path)) {
        BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: refusing log path " << log_path;
        return FIX_EXIT_BAD_ARGS;
    }
    boost::nowide::ofstream log_file;
    if (!log_path.empty()) log_file.open(log_path.c_str(), std::ios::app);
    auto say = [&log_file](const std::string& s) {
        BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: " << s;
        if (log_file.is_open()) log_file << s << "\n" << std::flush;
    };

#ifndef _WIN32
    say("Windows Firewall exists only on Windows");
    return FIX_EXIT_UNSUPPORTED;
#else
    const std::string exe    = this_exe_path();
    const std::string go2rtc = go2rtc_path_for(exe);
    say("elevated helper for " + exe + (allow_public ? " (Public networks too)" : " (Private and Domain networks)"));
    auto backend = make_system_backend();
    const Snapshot before = backend->read();
    if (!before.ok) {
        say("could not read Windows Firewall: " + before.error);
        return FIX_EXIT_READ_FAILED;
    }
    for (const std::string& line : describe_for_log(diagnose(before, exe, go2rtc))) say("before: " + line);
    const FixPlan   plan = plan_fix(before, exe, go2rtc, allow_public);
    const FixResult res  = apply_fix(*backend, plan);
    if (log_file.is_open())
        for (const std::string& s : res.steps) log_file << s << "\n";
    const Snapshot after = backend->read();
    if (after.ok)
        for (const std::string& line : describe_for_log(diagnose(after, exe, go2rtc))) say("after: " + line);
    return res.failed ? FIX_EXIT_PARTIAL : FIX_EXIT_OK;
#endif
}

ElevatedOutcome run_elevated_fix(void* parent_hwnd, bool allow_public)
{
    ElevatedOutcome out;
#ifndef _WIN32
    (void) parent_hwnd; (void) allow_public;
    out.error = "Windows Firewall exists only on Windows";
    return out;
#else
    const std::string exe = this_exe_path();
    boost::system::error_code ec;
    const fs::path log = fs::temp_directory_path(ec) / ("edgeslicer_firewall_fix_" + std::to_string(::GetCurrentProcessId()) + ".log");
    fs::remove(log, ec);

    std::wstring params = L"--fix-firewall";
    if (allow_public) params += L" --public";
    if (!log.empty()) params += L" --fix-firewall-log \"" + log.wstring() + L"\"";
    BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: asking Windows to run " << exe << " " << boost::nowide::narrow(params) << " as administrator";

    ComScope com; // ShellExecuteEx wants COM on the calling thread
    const std::wstring wexe = boost::nowide::widen(exe);
    SHELLEXECUTEINFOW  sei {};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd         = (HWND) parent_hwnd;
    sei.lpVerb       = L"runas";
    sei.lpFile       = wexe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow        = SW_HIDE;
    if (!::ShellExecuteExW(&sei)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_CANCELLED) {
            out.result = ElevatedResult::Cancelled;
            BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: the user cancelled the Windows permission prompt (UAC); nothing changed";
        } else {
            out.error = "Windows could not start the helper (error " + std::to_string(err) + ")";
            BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: " << out.error;
        }
        return out;
    }
    if (sei.hProcess == nullptr) {
        out.error = "Windows started the helper but returned no process handle";
        BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: " << out.error;
        return out;
    }
    const DWORD wait = ::WaitForSingleObject(sei.hProcess, 180000);
    DWORD       code = (DWORD) -1;
    if (wait == WAIT_OBJECT_0 && ::GetExitCodeProcess(sei.hProcess, &code)) {
        out.result    = ElevatedResult::Done;
        out.exit_code = (int) code;
    } else {
        out.error = "the helper did not finish within 3 minutes";
    }
    ::CloseHandle(sei.hProcess);

    boost::nowide::ifstream in(boost::nowide::narrow(log.wstring()).c_str());
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;
        out.helper_log.push_back(line);
        BOOST_LOG_TRIVIAL(warning) << "[Firewall] helper: " << line;
    }
    in.close();
    fs::remove(log, ec);
    BOOST_LOG_TRIVIAL(warning) << "[Firewall] fix: helper finished, exit code " << out.exit_code << (out.error.empty() ? "" : " - " + out.error);
    return out;
#endif
}

} // namespace WinFirewall
} // namespace Slic3r
