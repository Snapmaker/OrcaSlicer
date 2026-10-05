// Windows Firewall check and fix (slic3r/Utils/WinFirewall): matching rules to this exe's path,
// what the rule set means for printer discovery / the hub / video, and exactly which rules the
// elevated fix removes and adds.
//
// Everything here runs on hand-built Snapshots and a fake Backend. Nothing reads or changes the
// real Windows Firewall: this runs on the owner's PC.

#include <catch2/catch.hpp>

#include <algorithm>
#include <map>
#include <string>

#include "slic3r/Utils/WinFirewall.hpp"

using namespace Slic3r::WinFirewall;

namespace {

const std::string EXE    = "C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe";
const std::string GO2RTC = "C:\\Program Files\\EdgeSlicer\\resources\\tools\\go2rtc\\go2rtc.exe";
const std::string OTHER  = "C:\\Tools\\EdgeSlicer-portable\\EdgeSlicer.exe";

EnvLookup fake_env(std::map<std::string, std::string> vars)
{
    return [vars](const std::string& name, std::string& value) {
        auto it = vars.find(name);
        if (it == vars.end()) return false;
        value = it->second;
        return true;
    };
}

Rule allow(const std::string& name, const std::string& program, int protocol, const std::string& ports, int profiles = ProfilePrivate | ProfileDomain)
{
    Rule r;
    r.name        = name;
    r.program     = program;
    r.protocol    = protocol;
    r.local_ports = ports;
    r.profiles    = profiles;
    return r;
}

// What Windows writes when its "allow access" prompt is cancelled: one Block rule per protocol,
// named after the program, any port, for the profiles that were ticked.
Rule prompt_block(const std::string& program, int protocol, int profiles = ProfilePrivate | ProfilePublic)
{
    Rule r = allow("EdgeSlicer.exe", program, protocol, "", profiles);
    r.allow = false;
    return r;
}

// The installer's rules, exactly as cmake/nsis/SnapmakerURLProtocols_install.nsh writes them.
std::vector<Rule> installer_rules(const std::string& exe = EXE, const std::string& go2rtc = GO2RTC)
{
    return { allow(RULE_HUB, exe, ProtoTCP, "13640-13659"), allow(RULE_DISCOVERY, exe, ProtoUDP, "2021,1990"),
             allow(RULE_WEBRTC, go2rtc, ProtoUDP, "8555-8574"), allow(RULE_WEBRTC, go2rtc, ProtoTCP, "8555-8574"),
             allow(RULE_FLASHFORGE, exe, ProtoUDP, "18007") };
}

Snapshot snapshot(std::vector<Rule> rules, int current = ProfilePrivate)
{
    Snapshot s;
    s.ok               = true;
    s.rules            = std::move(rules);
    s.current_profiles = current;
    return s;
}

const ExpectedStatus& status(const Diagnosis& d, const std::string& purpose, int protocol)
{
    for (const ExpectedStatus& e : d.expected)
        if (e.rule.purpose == purpose && e.rule.protocol == protocol) return e;
    FAIL("no expected rule " << purpose);
    return d.expected.front();
}

bool same_fields(const Rule& a, const Rule& b)
{
    return a.name == b.name && a.program == b.program && a.inbound == b.inbound && a.allow == b.allow && a.protocol == b.protocol &&
           a.local_ports == b.local_ports && a.profiles == b.profiles && a.enabled == b.enabled;
}

// Applies remove/add to an in-memory rule list, like the COM backend does to the real one.
class FakeBackend : public Backend
{
public:
    explicit FakeBackend(Snapshot s) : state(std::move(s)) {}
    Snapshot read() override { return state; }
    bool     remove(const Rule& r, std::string& error) override
    {
        if (fail_removes) { error = "access denied"; return false; }
        auto it = std::find_if(state.rules.begin(), state.rules.end(), [&r](const Rule& x) { return same_fields(x, r); });
        if (it == state.rules.end()) { error = "not found"; return false; }
        state.rules.erase(it); // exactly one, even when names repeat
        return true;
    }
    bool add(const Rule& r, std::string&) override
    {
        state.rules.push_back(r);
        return true;
    }
    Snapshot state;
    bool     fail_removes { false };
};

} // namespace

// ---- paths ----------------------------------------------------------------------------------

TEST_CASE("program paths match case-insensitively, through quotes, slashes and dot segments", "[WinFirewall]")
{
    CHECK(normalize_program_path("C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe") == "c:\\program files\\edgeslicer\\edgeslicer.exe");
    CHECK(same_program(EXE, "c:\\program files\\edgeslicer\\EDGESLICER.EXE"));
    CHECK(same_program(EXE, "\"C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe\""));
    CHECK(same_program(EXE, "  \"C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe\"  "));
    CHECK(same_program(EXE, "C:/Program Files/EdgeSlicer/EdgeSlicer.exe"));
    CHECK(same_program(EXE, "C:\\Program Files\\\\EdgeSlicer\\.\\EdgeSlicer.exe"));
    CHECK(same_program(EXE, "C:\\Program Files\\EdgeSlicer\\resources\\..\\EdgeSlicer.exe"));
    CHECK(same_program(EXE, "\\\\?\\C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe"));
    CHECK(normalize_program_path("C:\\..\\..\\x.exe") == "c:\\x.exe"); // never above the drive
}

TEST_CASE("program paths with %environment% variables expand before comparing", "[WinFirewall]")
{
    const EnvLookup env = fake_env({ { "ProgramFiles", "C:\\Program Files" }, { "LOCALAPPDATA", "C:\\Users\\me\\AppData\\Local" } });
    CHECK(same_program(EXE, "%ProgramFiles%\\EdgeSlicer\\EdgeSlicer.exe", env));
    CHECK(same_program("C:\\Users\\me\\AppData\\Local\\Edge\\EdgeSlicer.exe", "%LOCALAPPDATA%\\Edge\\EdgeSlicer.exe", env));
    // An unknown variable stays literal and does not match.
    CHECK_FALSE(same_program(EXE, "%NOPE%\\EdgeSlicer\\EdgeSlicer.exe", env));
    CHECK(normalize_program_path("C:\\100%\\a.exe", env) == "c:\\100%\\a.exe");
}

TEST_CASE("different copies and empty programs never match", "[WinFirewall]")
{
    CHECK_FALSE(same_program(EXE, OTHER));
    CHECK_FALSE(same_program(EXE, "C:\\Program Files\\EdgeSlicer\\EdgeSlicer.exe.bak"));
    CHECK_FALSE(same_program(EXE, "C:\\Program Files\\EdgeSlicer2\\EdgeSlicer.exe"));
    CHECK_FALSE(same_program("", ""));       // a rule for "any program" is not a rule for us
    CHECK_FALSE(same_program(EXE, ""));
    CHECK(same_program("\\\\nas\\share\\EdgeSlicer.exe", "//NAS/share/EdgeSlicer.exe"));
}

// ---- ports ----------------------------------------------------------------------------------

TEST_CASE("rule port lists cover the ports EdgeSlicer listens on", "[WinFirewall]")
{
    CHECK(ports_cover("", 2021));
    CHECK(ports_cover("*", 2021));
    CHECK(ports_cover("Any", 2021));
    CHECK(ports_cover("2021,1990", 1990));
    CHECK(ports_cover("2021, 1990", 1990));
    CHECK_FALSE(ports_cover("2021", 1990));
    CHECK(ports_cover("13640-13659", 13659));
    CHECK_FALSE(ports_cover("13640-13659", 13660));
    CHECK(ports_cover("80,8000-9000", 8555));
    CHECK_FALSE(ports_cover("RPC", 2021));
    CHECK_FALSE(ports_cover("20210", 2021)); // no prefix matching
    CHECK((expand_ports("2021,1990") == std::vector<int>{ 2021, 1990 }));
    CHECK(expand_ports("13640-13659").size() == 20);
    CHECK(protocol_covers(ProtoAny, ProtoUDP));
    CHECK_FALSE(protocol_covers(ProtoTCP, ProtoUDP));
}

// ---- diagnosis ------------------------------------------------------------------------------

TEST_CASE("installer rules for this exe: everything allowed on a Private network", "[WinFirewall]")
{
    const Diagnosis d = diagnose(snapshot(installer_rules()), EXE, GO2RTC);
    REQUIRE(d.ok);
    REQUIRE(d.expected.size() == 5);
    CHECK(status(d, "discovery", ProtoUDP).allowed_profiles == (ProfilePrivate | ProfileDomain));
    CHECK(status(d, "hub", ProtoTCP).allowed_profiles == (ProfilePrivate | ProfileDomain));
    CHECK(status(d, "webrtc", ProtoTCP).allowed_profiles == (ProfilePrivate | ProfileDomain));
    CHECK(d.blocks.empty());
    CHECK_FALSE(d.has_problems());
}

TEST_CASE("the installed copy's rules do nothing for a portable copy", "[WinFirewall]")
{
    const Diagnosis d = diagnose(snapshot(installer_rules()), OTHER, "");
    CHECK(d.expected.size() == 3); // no go2rtc.exe next to it: no video rules expected
    CHECK(status(d, "discovery", ProtoUDP).allowed_profiles == 0);
    CHECK(d.problem_profiles() == ProfilePrivate);
    CHECK(d.discovery_problem_profiles() == ProfilePrivate);
}

TEST_CASE("a Block rule from a cancelled prompt wins over the allow rules", "[WinFirewall]")
{
    std::vector<Rule> rules = installer_rules();
    rules.push_back(prompt_block(EXE, ProtoUDP));
    rules.push_back(prompt_block(EXE, ProtoTCP));
    rules.push_back(prompt_block(OTHER, ProtoUDP)); // another copy's: not ours to report
    const Diagnosis d = diagnose(snapshot(rules), EXE, GO2RTC);
    CHECK(d.blocks.size() == 2);
    CHECK(d.enabled_blocks() == 2);
    CHECK(status(d, "discovery", ProtoUDP).blocked_profiles == (ProfilePrivate | ProfilePublic));
    CHECK(status(d, "webrtc", ProtoUDP).blocked_profiles == 0); // the block is for EdgeSlicer.exe, not go2rtc.exe
    CHECK(d.discovery_problem_profiles() == ProfilePrivate);
    CHECK(d.has_problems());
}

TEST_CASE("a disabled Block rule is listed but blocks nothing", "[WinFirewall]")
{
    std::vector<Rule> rules = installer_rules();
    Rule b = prompt_block(EXE, ProtoUDP);
    b.enabled = false;
    rules.push_back(b);
    const Diagnosis d = diagnose(snapshot(rules), EXE, GO2RTC);
    CHECK(d.blocks.size() == 1);
    CHECK(d.enabled_blocks() == 0);
    CHECK(status(d, "discovery", ProtoUDP).blocked_profiles == 0);
    CHECK_FALSE(d.has_problems());
}

TEST_CASE("on a Public network the Private/Domain rules are not enough", "[WinFirewall]")
{
    const Diagnosis d = diagnose(snapshot(installer_rules(), ProfilePublic), EXE, GO2RTC);
    CHECK(d.on_public_network());
    CHECK(d.discovery_problem_profiles() == ProfilePublic);
    CHECK(d.has_problems());

    // ... unless the firewall is off for Public.
    Snapshot off = snapshot(installer_rules(), ProfilePublic);
    off.firewall_on[2] = false;
    const Diagnosis d_off = diagnose(off, EXE, GO2RTC);
    CHECK_FALSE(d_off.on_public_network());
    CHECK_FALSE(d_off.has_problems());
}

TEST_CASE("Block all incoming connections overrides every allow rule", "[WinFirewall]")
{
    Snapshot s = snapshot(installer_rules(), ProfilePrivate);
    s.block_all_inbound[1] = true;
    CHECK(diagnose(s, EXE, GO2RTC).discovery_problem_profiles() == ProfilePrivate);
}

TEST_CASE("allow rules are matched by program, protocol and every port", "[WinFirewall]")
{
    const EnvLookup env = fake_env({ { "ProgramFiles", "C:\\Program Files" } });
    SECTION("a user's broad rule (any protocol, any port, quoted %ProgramFiles% path) counts")
    {
        const Diagnosis d = diagnose(snapshot({ allow("mine", "\"%ProgramFiles%\\EdgeSlicer\\EdgeSlicer.exe\"", ProtoAny, "*", ProfileAll) }), EXE, "", env);
        CHECK(status(d, "discovery", ProtoUDP).allowed_profiles == ProfileAll);
        CHECK(status(d, "hub", ProtoTCP).allowed_profiles == ProfileAll);
    }
    SECTION("a rule covering 2021 but not 1990 does not count")
    {
        const Diagnosis d = diagnose(snapshot({ allow("half", EXE, ProtoUDP, "2021") }), EXE, "");
        CHECK(status(d, "discovery", ProtoUDP).allowed_profiles == 0);
    }
    SECTION("two rules that split the ports and profiles: only the profiles both cover")
    {
        const Diagnosis d = diagnose(snapshot({ allow("a", EXE, ProtoUDP, "2021", ProfilePrivate | ProfilePublic),
                                                allow("b", EXE, ProtoUDP, "1990", ProfilePrivate) }),
                                     EXE, "");
        CHECK(status(d, "discovery", ProtoUDP).allowed_profiles == ProfilePrivate);
    }
    SECTION("the wrong protocol, an outbound rule or a disabled rule does not count")
    {
        Rule out = allow("out", EXE, ProtoUDP, "2021,1990");
        out.inbound = false;
        Rule off = allow("off", EXE, ProtoUDP, "2021,1990");
        off.enabled = false;
        const Diagnosis d = diagnose(snapshot({ allow("tcp", EXE, ProtoTCP, "2021,1990"), out, off }), EXE, "");
        CHECK(status(d, "discovery", ProtoUDP).allowed_profiles == 0);
    }
}

TEST_CASE("a firewall that cannot be read is reported, not guessed at", "[WinFirewall]")
{
    Snapshot s;
    s.error           = "INetFwPolicy2 unavailable";
    const Diagnosis d = diagnose(s, EXE, GO2RTC);
    CHECK_FALSE(d.ok);
    CHECK(d.expected.empty());
    CHECK_FALSE(d.has_problems());
    const std::vector<std::string> log = describe_for_log(d);
    CHECK(std::any_of(log.begin(), log.end(), [](const std::string& l) { return l.find("INetFwPolicy2 unavailable") != std::string::npos; }));
}

// ---- the fix --------------------------------------------------------------------------------

TEST_CASE("the fix removes only Block rules for this exe and its go2rtc.exe", "[WinFirewall]")
{
    std::vector<Rule> rules;
    rules.push_back(prompt_block(EXE, ProtoUDP));
    rules.push_back(prompt_block(EXE, ProtoTCP));
    rules.push_back(prompt_block("\"" + GO2RTC + "\"", ProtoUDP));
    Rule outbound_block = prompt_block(EXE, ProtoTCP);
    outbound_block.inbound = false;
    rules.push_back(outbound_block);
    rules.push_back(prompt_block(OTHER, ProtoUDP));                           // another copy
    rules.push_back(prompt_block("C:\\Windows\\System32\\svchost.exe", ProtoUDP)); // another program
    Rule any_program_block = prompt_block("", ProtoUDP);                      // applies to every program
    rules.push_back(any_program_block);

    const FixPlan plan = plan_fix(snapshot(rules), EXE, GO2RTC, false);
    CHECK(plan.remove.size() == 4);
    for (const Rule& r : plan.remove) {
        CHECK_FALSE(r.allow);
        CHECK((same_program(r.program, EXE) || same_program(r.program, GO2RTC)));
    }
}

TEST_CASE("the fix replaces our own allow rules and leaves other allow rules alone", "[WinFirewall]")
{
    std::vector<Rule> rules = installer_rules();
    rules.push_back(allow("EdgeSlicer", EXE, ProtoAny, "", ProfilePrivate));         // Windows' own "Allow" rule, broad
    rules.push_back(allow("EdgeSlicer", EXE, ProtoUDP, "48899", ProfilePrivate));    // our name, other ports: someone's
    rules.push_back(allow(RULE_DISCOVERY, OTHER, ProtoUDP, "2021,1990"));            // the portable copy's
    rules.push_back(allow("Chrome", "C:\\Program Files\\Google\\Chrome\\chrome.exe", ProtoUDP, "5353"));

    const FixPlan plan = plan_fix(snapshot(rules), EXE, GO2RTC, false);
    REQUIRE(plan.remove.size() == 5); // the installer's five for this copy
    for (const Rule& r : plan.remove) {
        CHECK(r.allow);
        CHECK(r.profiles == (ProfilePrivate | ProfileDomain));
        CHECK((r.name == RULE_HUB || r.name == RULE_DISCOVERY || r.name == RULE_WEBRTC || r.name == RULE_FLASHFORGE));
        CHECK((same_program(r.program, EXE) || same_program(r.program, GO2RTC)));
        CHECK(r.protocol != ProtoAny);
    }
}

TEST_CASE("the fix adds the installer's rules for this path, Private and Domain unless asked", "[WinFirewall]")
{
    const FixPlan plan = plan_fix(snapshot({}), OTHER, "", false);
    REQUIRE(plan.add.size() == 3);
    for (const Rule& r : plan.add) {
        CHECK(r.program == OTHER);
        CHECK(r.allow);
        CHECK(r.inbound);
        CHECK(r.enabled);
        CHECK(r.profiles == (ProfilePrivate | ProfileDomain));
    }
    CHECK(plan.add[0].name == RULE_DISCOVERY);
    CHECK(plan.add[0].protocol == ProtoUDP);
    CHECK(plan.add[0].local_ports == "2021,1990");
    CHECK(plan.add[1].name == RULE_HUB);
    CHECK(plan.add[1].local_ports == "13640-13659");

    const FixPlan pub = plan_fix(snapshot({}), EXE, GO2RTC, true);
    REQUIRE(pub.add.size() == 5);
    for (const Rule& r : pub.add) CHECK(r.profiles == ProfileAll);
    CHECK(pub.add[2].program == GO2RTC);
    CHECK(pub.add[2].local_ports == "8555-8574");
}

TEST_CASE("a firewall that cannot be read: the fix removes nothing", "[WinFirewall]")
{
    Snapshot s;
    s.rules = { prompt_block(EXE, ProtoUDP) }; // not ok: must not be trusted
    CHECK(plan_fix(s, EXE, GO2RTC, false).remove.empty());
}

TEST_CASE("applying the fix clears a cancelled-prompt block and the check then passes", "[WinFirewall]")
{
    std::vector<Rule> rules = installer_rules();
    rules.push_back(prompt_block(EXE, ProtoUDP));
    rules.push_back(prompt_block(EXE, ProtoTCP));
    rules.push_back(prompt_block(OTHER, ProtoUDP));
    FakeBackend fw(snapshot(rules));
    REQUIRE(diagnose(fw.read(), EXE, GO2RTC).has_problems());

    const FixResult res = apply_fix(fw, plan_fix(fw.read(), EXE, GO2RTC, false));
    CHECK(res.failed == 0);
    CHECK(res.removed == 7);
    CHECK(res.added == 5);
    CHECK_FALSE(diagnose(fw.read(), EXE, GO2RTC).has_problems());
    // The other copy's block rule is still there; nothing doubled up.
    CHECK(fw.state.rules.size() == 6);
    CHECK(std::count_if(fw.state.rules.begin(), fw.state.rules.end(), [](const Rule& r) { return same_program(r.program, OTHER); }) == 1);

    // Running it again is a no-op in effect: same rules, still clean.
    const FixResult again = apply_fix(fw, plan_fix(fw.read(), EXE, GO2RTC, false));
    CHECK(again.removed == 5);
    CHECK(again.added == 5);
    CHECK(fw.state.rules.size() == 6);
}

TEST_CASE("a failed removal is counted and reported, not hidden", "[WinFirewall]")
{
    FakeBackend fw(snapshot({ prompt_block(EXE, ProtoUDP) }));
    fw.fail_removes = true;
    const FixResult res = apply_fix(fw, plan_fix(fw.read(), EXE, "", false));
    CHECK(res.failed == 1);
    CHECK(res.added == 3);
    CHECK(std::any_of(res.steps.begin(), res.steps.end(), [](const std::string& s) { return s.find("FAILED to remove") != std::string::npos; }));
}

// ---- the elevated helper's arguments -----------------------------------------------------------

TEST_CASE("the helper only writes its log to a file it recognises", "[WinFirewall]")
{
    CHECK(is_valid_fix_log_path("C:\\Users\\me\\AppData\\Local\\Temp\\edgeslicer_firewall_fix_1234.log"));
    CHECK(is_valid_fix_log_path("C:\\Temp\\EdgeSlicer_Firewall_Fix_9.LOG"));
    CHECK_FALSE(is_valid_fix_log_path("edgeslicer_firewall_fix_1234.log"));                 // relative
    CHECK_FALSE(is_valid_fix_log_path("C:\\Windows\\System32\\drivers\\etc\\hosts"));
    CHECK_FALSE(is_valid_fix_log_path("C:\\Temp\\edgeslicer_firewall_fix_.log"));
    CHECK_FALSE(is_valid_fix_log_path("C:\\Temp\\edgeslicer_firewall_fix_12a.log"));
    CHECK_FALSE(is_valid_fix_log_path("C:\\Temp\\edgeslicer_firewall_fix_12.log.exe"));
    CHECK_FALSE(is_valid_fix_log_path(""));
}

TEST_CASE("--fix-firewall is recognised anywhere on the command line", "[WinFirewall]")
{
    char  a0[] = "EdgeSlicer.exe", a1[] = "--fix-firewall", a2[] = "--public", a3[] = "--fix-firewall-log";
    char* with[]    = { a0, a1, a2, nullptr };
    char* without[] = { a0, a2, a3, nullptr };
    CHECK(is_fix_cli(3, with));
    CHECK_FALSE(is_fix_cli(3, without));
}

TEST_CASE("profile words read in Domain, Private, Public order", "[WinFirewall]")
{
    CHECK(profiles_text(0).empty());
    CHECK(profiles_text(ProfilePublic | ProfilePrivate) == "Private, Public");
    CHECK(profiles_text(ProfileAll) == "Domain, Private, Public");
    CHECK(profile_index(ProfilePublic) == 2);
    CHECK(profile_index(3) == -1);
}

// ---- FlashForge discovery and the Public-network case -------------------------------------------

TEST_CASE("FlashForge's search needs UDP 18007 for this exe, and the fix adds it", "[WinFirewall]")
{
    // A 2.4.3.0 install: the Bambu, hub and video rules are there, the FlashForge one is not.
    std::vector<Rule> rules = installer_rules();
    rules.pop_back();
    const Diagnosis d = diagnose(snapshot(rules), EXE, GO2RTC);
    CHECK(status(d, "flashforge", ProtoUDP).allowed_profiles == 0);
    CHECK(d.problem_profiles() == ProfilePrivate);
    CHECK(d.discovery_problem_profiles() == 0); // the Bambu discovery rule is fine

    const FixPlan plan = plan_fix(snapshot(rules), EXE, GO2RTC, false);
    const auto    ff   = std::find_if(plan.add.begin(), plan.add.end(), [](const Rule& r) { return r.name == RULE_FLASHFORGE; });
    REQUIRE(ff != plan.add.end());
    CHECK(ff->protocol == ProtoUDP);
    CHECK(ff->local_ports == "18007");
    CHECK(ff->profiles == (ProfilePrivate | ProfileDomain));
}

TEST_CASE("the fix is offered only while it can change something", "[WinFirewall]")
{
    // The logged case: rules in place for Private and Domain, the PC on a Public network. The
    // verdict is "problems" on Public, and pressing the fix again only re-adds the same rules.
    const Diagnosis public_net = diagnose(snapshot(installer_rules(), ProfilePrivate | ProfilePublic), EXE, GO2RTC);
    CHECK(public_net.problem_profiles() == ProfilePublic);
    CHECK_FALSE(public_net.fix_would_help(false));
    CHECK(public_net.fix_would_help(true)); // ticking "Also allow on Public networks" does change things

    // Missing rules, or a Block rule, are what the fix is for.
    const Diagnosis missing = diagnose(snapshot({}), EXE, GO2RTC);
    CHECK(missing.fix_would_help(false));
    std::vector<Rule> blocked = installer_rules();
    blocked.push_back(prompt_block(EXE, ProtoUDP));
    CHECK(diagnose(snapshot(blocked), EXE, GO2RTC).fix_would_help(false));

    // Nothing wrong: nothing to fix. A firewall that could not be read: nothing the fix can know.
    CHECK_FALSE(diagnose(snapshot(installer_rules()), EXE, GO2RTC).fix_would_help(false));
    Snapshot unreadable;
    CHECK_FALSE(diagnose(unreadable, EXE, GO2RTC).fix_would_help(true));
}
