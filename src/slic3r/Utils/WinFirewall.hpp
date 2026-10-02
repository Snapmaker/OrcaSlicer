#pragma once

// Windows Firewall: what it does to EdgeSlicer, and the one-UAC-prompt fix.
//
// Bambu printers announce themselves on the LAN over UDP 2021 / 1990. When Windows Firewall drops
// those packets for EdgeSlicer.exe the Device list stays empty and nothing says why. The usual
// reasons: the installer's rules are missing (a portable copy never had any - Windows keys rules on
// the exe PATH), Windows wrote a Block rule when the user cancelled its "allow access" prompt, or
// the network is filed as Public while our rules only cover Private and Domain.
//
// This module reads the rules through the Windows Firewall COM API (INetFwPolicy2) - property values
// are the same in every display language, unlike netsh's text - and works out which of the rules
// cmake/nsis/SnapmakerURLProtocols_install.nsh creates exist for *this running exe's path*, which
// Block rules hit it or its go2rtc.exe, and which network profiles are live. The fix runs the same
// exe elevated (`EdgeSlicer.exe --fix-firewall`), which deletes only Block rules whose program is this
// exe or its go2rtc.exe and recreates the allow rules for this path. It never touches another
// program's rules and never switches the firewall off.
//
// Everything that decides something is pure and takes a Snapshot, so slic3rutils_tests can pin it
// without touching the real firewall; the COM calls sit behind Backend.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Slic3r {
namespace WinFirewall {

// NET_FW_PROFILE_TYPE2 bits.
enum Profile : int {
    ProfileDomain  = 1,
    ProfilePrivate = 2,
    ProfilePublic  = 4,
    ProfileAll     = 7,
};

// NET_FW_IP_PROTOCOL values.
enum Protocol : int {
    ProtoTCP = 6,
    ProtoUDP = 17,
    ProtoAny = 256,
};

// The installer's rule names. The fix reuses them, so an install/uninstall cleans up after it too.
extern const char* const RULE_HUB;       // "EdgeSlicer"               TCP 13640-13659, the phone hub
extern const char* const RULE_DISCOVERY; // "EdgeSlicer LAN discovery" UDP 2021,1990, Bambu SSDP
extern const char* const RULE_WEBRTC;    // "EdgeSlicer WebRTC video"  go2rtc.exe, UDP and TCP 8555-8574

// One firewall rule, as much of it as matters here.
struct Rule
{
    std::string name;
    std::string program;      // as stored: may carry quotes or %environment% variables
    std::string local_ports;  // "*", "" (any), "2021,1990", "13640-13659", or a keyword like "RPC"
    std::string description;
    int         protocol { ProtoAny };
    int         profiles { ProfileAll }; // Profile bits
    bool        inbound { true };
    bool        enabled { true };
    bool        allow { true };
};

// What the firewall looks like right now.
struct Snapshot
{
    bool              ok { false };
    std::string       error;                  // why `ok` is false
    std::vector<Rule> rules;
    int               current_profiles { 0 }; // Profile bits of the networks the PC is on now
    bool              firewall_on[3] { true, true, true };            // Domain, Private, Public
    bool              block_all_inbound[3] { false, false, false };   // "Block all incoming connections"
};

// 0 for Domain, 1 for Private, 2 for Public; -1 for anything else.
int profile_index(int single_profile_bit);
// "Domain, Private, Public" order, "" for 0. All three is spelled out too (the dialog says
// "Domain, Private and Public" itself if it wants to).
std::string profiles_text(int profiles);

// ---- paths ----------------------------------------------------------------------------------
// Looks up an environment variable for %VAR% expansion; false if it is not set. Empty function:
// the process environment.
using EnvLookup = std::function<bool(const std::string& name, std::string& value)>;

// A program path in the form rules are compared in: surrounding quotes and blanks stripped,
// %VAR% expanded, '/' turned into '\', a \\?\ prefix dropped, repeated separators collapsed
// (a leading \\ of a UNC path kept), "." and ".." resolved, no trailing separator, lower case.
std::string normalize_program_path(const std::string& path, const EnvLookup& env = {});
bool        same_program(const std::string& a, const std::string& b, const EnvLookup& env = {});

// ---- ports ----------------------------------------------------------------------------------
// Whether a rule's LocalPorts value lets `port` through: "", "*" and "Any" cover everything,
// otherwise a comma list of ports and lo-hi ranges. Keywords (RPC, IPHTTPS, ...) cover nothing.
bool ports_cover(const std::string& rule_ports, int port);
// The ports an expected rule lists, expanded ("2021,1990" -> 2021, 1990; "13640-13659" -> 20).
std::vector<int> expand_ports(const std::string& ports);
// Whether a rule's protocol lets `protocol` through.
bool protocol_covers(int rule_protocol, int protocol);

// ---- what EdgeSlicer needs ------------------------------------------------------------------
struct ExpectedRule
{
    std::string name;     // the installer's rule name
    std::string program;  // this copy's exe or go2rtc.exe
    int         protocol;
    std::string ports;
    std::string purpose;  // "hub" | "discovery" | "webrtc": the dialog words it
};

// The four rules the installer creates, for these paths. `go2rtc` empty (not shipped with this
// copy): no WebRTC rules.
std::vector<ExpectedRule> expected_rules(const std::string& exe, const std::string& go2rtc);

struct ExpectedStatus
{
    ExpectedRule rule;
    int          allowed_profiles { 0 }; // profiles where enabled inbound allow rules cover every port
    int          blocked_profiles { 0 }; // profiles where an enabled inbound Block rule hits it
};

struct Diagnosis
{
    bool                        ok { false };
    std::string                 error;
    std::string                 exe;
    std::string                 go2rtc;
    std::vector<ExpectedStatus> expected;
    std::vector<Rule>           blocks;        // every Block rule (any direction, enabled or not) for exe / go2rtc
    int                         current_profiles { 0 };
    bool                        firewall_on[3] { true, true, true };
    bool                        block_all_inbound[3] { false, false, false };

    // Live profiles (current_profiles) where the firewall is on and something drops our traffic:
    // a Block rule, a missing/narrow allow rule, or "Block all incoming connections".
    int  problem_profiles() const;
    // Same, for printer discovery alone (what makes the Device list empty).
    int  discovery_problem_profiles() const;
    bool has_problems() const { return problem_profiles() != 0 || enabled_blocks() != 0; }
    int  enabled_blocks() const;
    // Whether Public is live and the firewall is on for it.
    bool on_public_network() const;
};

Diagnosis diagnose(const Snapshot& snapshot, const std::string& exe, const std::string& go2rtc, const EnvLookup& env = {});
// One line per fact, in English, for the log.
std::vector<std::string> describe_for_log(const Diagnosis& d);

// ---- the fix --------------------------------------------------------------------------------
struct FixPlan
{
    std::vector<Rule> remove; // Block rules for exe / go2rtc, and our own named allow rules for them
    std::vector<Rule> add;    // the expected rules, recreated for this path
};

// Only rules whose program is `exe` or `go2rtc` are ever in `remove`: every Block rule for them,
// plus the allow rules carrying one of the installer's names (they are recreated in `add`, so a
// stale port list or profile set is replaced). Added rules are inbound, enabled, Allow, for
// Private + Domain, plus Public when `allow_public`.
FixPlan plan_fix(const Snapshot& snapshot, const std::string& exe, const std::string& go2rtc, bool allow_public,
                 const EnvLookup& env = {});

// Where the COM calls live (the tests substitute a fake).
class Backend
{
public:
    virtual ~Backend() = default;
    virtual Snapshot read() = 0;
    // Removes exactly one rule equal to `r` (name, program, direction, action, protocol, ports,
    // profiles), even when other rules share its name.
    virtual bool remove(const Rule& r, std::string& error) = 0;
    virtual bool add(const Rule& r, std::string& error) = 0;
};

struct FixResult
{
    int                      removed { 0 };
    int                      added { 0 };
    int                      failed { 0 };
    std::vector<std::string> steps; // English, one per action, also logged
};

FixResult apply_fix(Backend& backend, const FixPlan& plan);

// The real firewall (COM). Off Windows: read() says so and remove()/add() fail.
std::unique_ptr<Backend> make_system_backend();
Snapshot                 read_system_snapshot();

// ---- this copy --------------------------------------------------------------------------------
// The running exe (EdgeSlicer.exe for the app), with native separators.
std::string this_exe_path();
// <exe dir>\resources\tools\go2rtc\go2rtc.exe when it exists, else "".
std::string go2rtc_path_for(const std::string& exe);
// diagnose() for this copy against the real firewall, logged at warning level.
Diagnosis diagnose_this_copy();

// ---- elevated helper --------------------------------------------------------------------------
// Command-line mode of the app: `--fix-firewall [--public] [--fix-firewall-log <file>]`.
bool is_fix_cli(int argc, char** argv);
// Runs the fix for this process's own exe; exit code: FIX_EXIT_*.
int run_fix_cli(int argc, char** argv);

enum FixExit : int {
    FIX_EXIT_OK          = 0,
    FIX_EXIT_PARTIAL     = 10, // some steps failed (see the log)
    FIX_EXIT_READ_FAILED = 11, // the firewall could not be read
    FIX_EXIT_BAD_ARGS    = 12,
    FIX_EXIT_UNSUPPORTED = 13, // not Windows
};

// The helper's log-file argument must look like this, so an elevated process never writes to an
// arbitrary path: <dir>\edgeslicer_firewall_fix_<digits>.log
bool is_valid_fix_log_path(const std::string& path);

enum class ElevatedResult { Done, Cancelled, Failed };

struct ElevatedOutcome
{
    ElevatedResult           result { ElevatedResult::Failed };
    int                      exit_code { -1 };
    std::string              error;
    std::vector<std::string> helper_log; // what the helper wrote, line by line
};

// Starts this exe elevated with --fix-firewall (one UAC prompt, owned by `parent_hwnd`) and waits
// for it. Blocking: call it off the UI thread.
ElevatedOutcome run_elevated_fix(void* parent_hwnd, bool allow_public);

} // namespace WinFirewall
} // namespace Slic3r
