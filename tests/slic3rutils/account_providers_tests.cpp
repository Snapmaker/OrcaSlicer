#include <catch2/catch.hpp>

#include "slic3r/Utils/AccountProviders.hpp"

#include <set>
#include <string>

using namespace Slic3r::Accounts;

// The rule behind the yellow Account button in the title bar (GUI/AccountStatus.cpp): which
// provider a printer's vendor maps to, and when a signed-out account is worth a warning. The title
// bar drawing and the sign-in events are checked by hand.

namespace {

struct Fake
{
    bool                  bbl_in    = false;
    bool                  bbl_cloud = false;
    bool                  sm_in     = false;
    std::set<std::string> flags; // provider ids with the "signed in before" flag
    int                   cloud_asked = 0;

    Registry make()
    {
        Registry r;
        Provider bbl;
        bbl.id                           = "bbl";
        bbl.display_name                 = "Bambu Lab";
        bbl.vendor_ids                   = {"BBL"};
        bbl.is_signed_in                 = [this] { return bbl_in; };
        bbl.selected_printer_cloud_bound = [this] { ++cloud_asked; return bbl_cloud; };
        r.add(bbl);
        Provider sm;
        sm.id           = "snapmaker";
        sm.display_name = "Snapmaker";
        sm.vendor_ids   = {"Snapmaker"};
        sm.is_signed_in = [this] { return sm_in; };
        r.add(sm);
        return r;
    }
    std::function<bool(const std::string&)> was() { return [this](const std::string& id) { return flags.count(id) > 0; }; }
};

} // namespace

TEST_CASE("account providers: should_warn rule", "[AccountProviders]")
{
    Inputs in;
    CHECK_FALSE(should_warn(in)); // signed out, never signed in, not cloud-bound: a LAN-only user

    in.previously_signed_in = true;
    CHECK(should_warn(in)); // an expired session
    in.previously_signed_in = false;

    in.cloud_bound = true;
    CHECK(should_warn(in)); // a cloud printer cannot be reached without the account

    in.previously_signed_in = true;
    CHECK(should_warn(in));

    in.signed_in = true; // signed in beats everything
    CHECK_FALSE(should_warn(in));
    in.previously_signed_in = false;
    CHECK_FALSE(should_warn(in));
    in.cloud_bound = false;
    CHECK_FALSE(should_warn(in));
}

TEST_CASE("account providers: registry lookup by vendor and id", "[AccountProviders]")
{
    Fake     f;
    Registry r = f.make();

    REQUIRE(r.find("bbl") != nullptr);
    CHECK(r.find("bbl")->display_name == "Bambu Lab");
    CHECK(r.find("nope") == nullptr);

    REQUIRE(r.find_for_vendor("BBL") != nullptr);
    CHECK(r.find_for_vendor("BBL")->id == "bbl");
    REQUIRE(r.find_for_vendor("Snapmaker") != nullptr);
    CHECK(r.find_for_vendor("Snapmaker")->id == "snapmaker");
    CHECK(r.find_for_vendor("Creality") == nullptr);
    CHECK(r.find_for_vendor("Flashforge") == nullptr);
    CHECK(r.find_for_vendor("") == nullptr);
}

TEST_CASE("account providers: registry ignores incomplete providers and replaces by id", "[AccountProviders]")
{
    Registry r;
    Provider no_id;
    no_id.is_signed_in = [] { return true; };
    r.add(no_id);
    Provider no_fn;
    no_fn.id = "x";
    r.add(no_fn);
    CHECK(r.providers().empty());

    Provider a;
    a.id           = "ff";
    a.display_name = "FlashForge";
    a.vendor_ids   = {"Flashforge"};
    a.is_signed_in = [] { return false; };
    r.add(a);
    REQUIRE(r.providers().size() == 1);

    Provider b     = a;
    b.display_name = "FlashForge cloud";
    b.is_signed_in = [] { return true; };
    r.add(b); // same id: replaced
    REQUIRE(r.providers().size() == 1);
    CHECK(r.find("ff")->display_name == "FlashForge cloud");
    CHECK(r.find("ff")->is_signed_in());

    r.clear();
    CHECK(r.providers().empty());
}

TEST_CASE("account providers: evaluate for a Bambu printer", "[AccountProviders]")
{
    Fake     f;
    Registry r = f.make();

    SECTION("signed out, LAN-only user who never signed in: no warning")
    {
        Status st = evaluate(r, "BBL", f.was());
        REQUIRE(st.provider != nullptr);
        CHECK(st.provider->id == "bbl");
        CHECK_FALSE(st.signed_in);
        CHECK_FALSE(st.warn);
    }
    SECTION("signed out after having signed in: warning")
    {
        f.flags.insert("bbl");
        Status st = evaluate(r, "BBL", f.was());
        CHECK_FALSE(st.signed_in);
        CHECK(st.warn);
    }
    SECTION("signed out with a cloud-bound printer selected: warning")
    {
        f.bbl_cloud = true;
        CHECK(evaluate(r, "BBL", f.was()).warn);
    }
    SECTION("the Snapmaker flag does not warn for a Bambu printer")
    {
        f.flags.insert("snapmaker");
        CHECK_FALSE(evaluate(r, "BBL", f.was()).warn);
    }
    SECTION("signed in: no warning, and the cloud lookup is not made")
    {
        f.bbl_in    = true;
        f.bbl_cloud = true;
        f.flags.insert("bbl");
        Status st = evaluate(r, "BBL", f.was());
        CHECK(st.signed_in);
        CHECK_FALSE(st.warn);
        CHECK(f.cloud_asked == 0);
    }
}

TEST_CASE("account providers: evaluate for a Snapmaker printer", "[AccountProviders]")
{
    Fake     f;
    Registry r = f.make();

    // Snapmaker has no cloud-bound printers: only the "signed in before" flag matters.
    Status st = evaluate(r, "Snapmaker", f.was());
    REQUIRE(st.provider != nullptr);
    CHECK(st.provider->id == "snapmaker");
    CHECK_FALSE(st.warn);

    f.flags.insert("snapmaker");
    CHECK(evaluate(r, "Snapmaker", f.was()).warn);

    f.sm_in = true;
    st      = evaluate(r, "Snapmaker", f.was());
    CHECK(st.signed_in);
    CHECK_FALSE(st.warn);

    // The Bambu account being signed out says nothing about a Snapmaker printer.
    f.sm_in = true;
    f.flags.insert("bbl");
    CHECK_FALSE(evaluate(r, "Snapmaker", f.was()).warn);
}

TEST_CASE("account providers: a vendor with no account never warns", "[AccountProviders]")
{
    Fake f;
    f.flags     = {"bbl", "snapmaker"};
    f.bbl_cloud = true;
    Registry r  = f.make();

    for (const char* vendor : {"Creality", "Flashforge", "Custom", ""}) {
        Status st = evaluate(r, vendor, f.was());
        CHECK(st.provider == nullptr);
        CHECK_FALSE(st.warn);
        CHECK_FALSE(st.signed_in);
    }
}

TEST_CASE("account providers: a provider added later is picked up", "[AccountProviders]")
{
    Fake     f;
    Registry r     = f.make();
    bool     ff_in = false;

    Provider ff;
    ff.id           = "flashforge";
    ff.display_name = "FlashForge";
    ff.vendor_ids   = {"Flashforge"};
    ff.is_signed_in = [&] { return ff_in; };
    r.add(ff);

    f.flags.insert("flashforge");
    CHECK(evaluate(r, "Flashforge", f.was()).warn);
    ff_in = true;
    CHECK_FALSE(evaluate(r, "Flashforge", f.was()).warn);
}

TEST_CASE("account providers: a missing was_signed_in answers no", "[AccountProviders]")
{
    Fake     f;
    Registry r  = f.make();
    Status   st = evaluate(r, "BBL", nullptr);
    CHECK_FALSE(st.warn);
}

TEST_CASE("account providers: flag key", "[AccountProviders]")
{
    CHECK(signed_in_flag_key("bbl") == "account_signed_in_bbl");
    CHECK(signed_in_flag_key("snapmaker") == "account_signed_in_snapmaker");
}
