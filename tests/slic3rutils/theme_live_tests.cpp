#include <catch2/catch.hpp>

// Live UI-theme switching (GUI_App::apply_theme_live): the parts that do not need the slicer. What a
// theme hands over is reset and reloaded as one (Theme::apply), and the colours UpdateDarkUI painted
// on windows go back to what they were before the next theme is applied (WindowColourStash), because
// the colour table cannot be run backwards: a theme paints several stock colours with one colour.

#include <string>

#include <boost/filesystem.hpp>

#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/Theme.hpp"
#include "slic3r/GUI/WindowColourStash.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/Utils/ThemePack.hpp"

#include <wx/app.h>
#include <wx/frame.h>
#include <wx/init.h>
#include <wx/panel.h>
#include <wx/stattext.h>

namespace fs = boost::filesystem;
using namespace Slic3r;
using namespace Slic3r::GUI;

namespace {

// A data dir of its own holding the themes a case writes, and the stock look again at the end.
struct ThemeDir
{
    std::string old_dir;
    fs::path    dir;
    ThemeDir()
    {
        old_dir = data_dir();
        dir     = fs::temp_directory_path() / fs::unique_path("theme-live-%%%%%%%%");
        fs::create_directories(dir);
        set_data_dir(dir.string());
    }
    ~ThemeDir()
    {
        Theme::load(""); // the stock colours, radii and font faces
        set_data_dir(old_dir);
        boost::system::error_code ec;
        fs::remove_all(dir, ec);
    }
    // Writes an installed theme.
    void save(const std::string& id, const ThemePack::Spec& spec)
    {
        std::string error;
        REQUIRE(Theme::save(id, spec, fs::path(), {}, error));
        REQUIRE(error.empty());
    }
};

ThemePack::Spec dark_theme()
{
    ThemePack::Spec spec;
    spec.name                 = "Live A";
    spec.base                 = "dark";
    spec.palette["window_bg"] = "#112233"; // #FFFFFF and #F8F7F7 both
    spec.palette["canvas_bg"] = "#050505";
    spec.palette["accent"]    = "#AA3311";
    spec.button_radius        = 9;
    return spec;
}

ThemePack::Spec light_theme()
{
    ThemePack::Spec spec;
    spec.name                 = "Live B";
    spec.base                 = "light";
    spec.palette["panel_bg"]  = "#445566"; // #F8F8F8 and more, but not #FFFFFF
    spec.palette["window_bg"] = "#EEDDCC";
    return spec;
}

void ensure_wx_gui()
{
    if (wxApp::GetInstance() != nullptr)
        return;
    wxApp::SetInstance(new wxApp());
    static wxChar  arg0[] = wxT("slic3rutils_tests");
    static wxChar* argv[] = {arg0, nullptr};
    static int     argc   = 1;
    REQUIRE(wxEntryStart(argc, argv));
}

// What GUI_App::UpdateDarkUI does to a window in the light look, through the stash it keeps.
void theme_like_update_dark_ui(wxWindow* window)
{
    const WindowColourStash::Stock stock = WindowColourStash::snapshot(window);
    const wxColour bg = StateColor::themedColorFor(window->GetBackgroundColour(), window->GetBackgroundColour());
    const wxColour fg = StateColor::themedColorFor(window->GetForegroundColour(), window->GetForegroundColour());
    if (bg != window->GetBackgroundColour())
        window->SetBackgroundColour(bg);
    if (fg != window->GetForegroundColour())
        window->SetForegroundColour(fg);
    WindowColourStash::commit(window, stock);
}

} // namespace

TEST_CASE("theme apply: a theme replaces the last one, Default clears it", "[ThemeLive]")
{
    ThemeDir themes;
    themes.save("live-a", dark_theme());
    themes.save("live-b", light_theme());

    REQUIRE(Theme::apply("live-a"));
    CHECK(Theme::active());
    CHECK(Theme::active_id() == "live-a");
    CHECK(Theme::base_dark() == 1);
    CHECK(StateColor::HasTheme());
    CHECK(StateColor::themedColorFor(wxColour("#FFFFFF"), *wxBLACK) == wxColour("#112233"));
    CHECK(StateColor::themedColorFor(wxColour("#F8F7F7"), *wxBLACK) == wxColour("#112233"));
    CHECK(Theme::colour("canvas_bg", *wxBLACK) == wxColour("#050505"));
    CHECK(Theme::has_colour("accent"));

    // B leaves none of A behind: its roles, its base, its canvas colour, its accent.
    REQUIRE(Theme::apply("live-b"));
    CHECK(Theme::active_id() == "live-b");
    CHECK(Theme::base_dark() == 0);
    CHECK(StateColor::themedColorFor(wxColour("#FFFFFF"), *wxBLACK) == wxColour("#EEDDCC"));
    CHECK(StateColor::themedColorFor(wxColour("#F8F8F8"), *wxBLACK) == wxColour("#445566"));
    CHECK(Theme::colour("canvas_bg", *wxBLACK) == *wxBLACK);
    CHECK_FALSE(Theme::has_colour("accent"));
    CHECK(Theme::spec().button_radius == -1);

    // Back to Default: the stock look, nothing themed.
    REQUIRE(Theme::apply(""));
    CHECK_FALSE(Theme::active());
    CHECK(Theme::active_id().empty());
    CHECK(Theme::base_dark() == -1);
    CHECK_FALSE(StateColor::HasTheme());
    CHECK(StateColor::themedColorFor(wxColour("#FFFFFF"), *wxBLACK) == *wxBLACK);
    CHECK(Theme::spec().palette.empty());
    CHECK_FALSE(Theme::banner().IsOk());

    // A theme that is not there falls back to the stock look and says so.
    REQUIRE(Theme::apply("live-a"));
    CHECK_FALSE(Theme::apply("not-installed"));
    CHECK_FALSE(Theme::active());
    CHECK_FALSE(StateColor::HasTheme());
    CHECK(Theme::base_dark() == -1);
}

TEST_CASE("theme apply: saving over the running theme is read again", "[ThemeLive]")
{
    ThemeDir themes;
    themes.save("live-a", dark_theme());
    REQUIRE(Theme::apply("live-a"));
    CHECK(StateColor::themedColorFor(wxColour("#FFFFFF"), *wxBLACK) == wxColour("#112233"));

    ThemePack::Spec changed = dark_theme();
    changed.palette["window_bg"] = "#334455";
    changed.palette.erase("accent");
    themes.save("live-a", changed);
    REQUIRE(Theme::apply("live-a"));
    CHECK(StateColor::themedColorFor(wxColour("#FFFFFF"), *wxBLACK) == wxColour("#334455"));
    CHECK_FALSE(Theme::has_colour("accent"));
}

TEST_CASE("theme apply: fonts wait for a restart", "[ThemeLive]")
{
    ThemeDir themes;
    ThemePack::Spec fancy = light_theme();
    fancy.name            = "Live Fancy";
    fancy.body.face       = "Cinzel";
    fancy.heading.face    = "Cinzel";
    themes.save("live-a", dark_theme());
    themes.save("live-b", light_theme());
    themes.save("live-fancy", fancy);

    // Started on a theme without fonts: a colour-only switch has nothing to restart for.
    Theme::load("live-a");
    CHECK_FALSE(Theme::fonts_pending());
    Theme::apply("live-b");
    CHECK_FALSE(Theme::fonts_pending());
    Theme::apply("");
    CHECK_FALSE(Theme::fonts_pending());
    // One with fonts does: they are made at start.
    Theme::apply("live-fancy");
    CHECK(Theme::fonts_pending());
    Theme::apply("live-b");
    CHECK_FALSE(Theme::fonts_pending());

    // Started on the fancy one: its fonts are the running ones, leaving them is what waits.
    Theme::load("live-fancy");
    CHECK_FALSE(Theme::fonts_pending());
    Theme::apply("live-b");
    CHECK(Theme::fonts_pending());
    Theme::apply("");
    CHECK(Theme::fonts_pending());
    Theme::apply("live-fancy");
    CHECK_FALSE(Theme::fonts_pending());
}

TEST_CASE("theme colour map: one themed colour stands for several stock ones", "[ThemeLive]")
{
    // The reason windows keep their stock colours instead of the map being run backwards: both
    // stock whites become the same colour, so the colour alone does not say which it was.
    ThemePack::Spec spec = dark_theme();
    const auto      map  = ThemePack::colour_map(spec, {});
    REQUIRE(map.count("#FFFFFF") == 1);
    REQUIRE(map.count("#F8F7F7") == 1);
    CHECK(map.at("#FFFFFF") == map.at("#F8F7F7"));
}

// Windows only, like the other cases that build real windows (no display on the other CI runs).
#ifdef _WIN32

TEST_CASE("window colour stash: theme A to theme B to Default leaves the stock colours", "[ThemeLive]")
{
    ensure_wx_gui();
    ThemeDir themes;
    themes.save("live-a", dark_theme());
    themes.save("live-b", light_theme());

    auto* frame  = new wxFrame(nullptr, wxID_ANY, "t");
    auto* white  = new wxPanel(frame);
    auto* offwht = new wxPanel(frame);
    auto* label  = new wxStaticText(frame, wxID_ANY, "x"); // follows its parent: no colour of its own
    auto* plain  = new wxPanel(frame);                      // neither is changed by any theme
    white->SetBackgroundColour(wxColour("#FFFFFF"));
    offwht->SetBackgroundColour(wxColour("#F8F7F7"));
    plain->SetBackgroundColour(wxColour("#123456"));
    white->SetForegroundColour(wxColour("#000000"));
    REQUIRE_FALSE(label->UseBgCol());
    const wxColour label_default = label->GetBackgroundColour();
    auto theme_all = [&] {
        for (wxWindow* w : {static_cast<wxWindow*>(white), static_cast<wxWindow*>(offwht), static_cast<wxWindow*>(label),
                            static_cast<wxWindow*>(plain)})
            theme_like_update_dark_ui(w);
    };
    auto check_stock = [&] {
        CHECK(white->GetBackgroundColour() == wxColour("#FFFFFF"));
        CHECK(white->GetForegroundColour() == wxColour("#000000"));
        CHECK(offwht->GetBackgroundColour() == wxColour("#F8F7F7"));
        CHECK(plain->GetBackgroundColour() == wxColour("#123456"));
        CHECK_FALSE(label->UseBgCol());
        CHECK(label->GetBackgroundColour() == label_default);
    };

    // Theme A: both whites become A's window colour (the colour cannot say which was which).
    REQUIRE(Theme::apply("live-a"));
    theme_all();
    CHECK(white->GetBackgroundColour() == wxColour("#112233"));
    CHECK(offwht->GetBackgroundColour() == wxColour("#112233"));
    CHECK(plain->GetBackgroundColour() == wxColour("#123456"));
    CHECK(WindowColourStash::has(white));
    CHECK_FALSE(WindowColourStash::has(plain)); // nothing changed, nothing kept

    // Themed twice over (UpdateDarkUI runs on the same window again and again): still reversible.
    theme_all();
    CHECK(white->GetBackgroundColour() == wxColour("#112233"));

    // Switching to B: put back, apply B. Without the stash A's #112233 would stay on both.
    WindowColourStash::restore_all();
    check_stock();
    CHECK(WindowColourStash::size() == 0);
    REQUIRE(Theme::apply("live-b"));
    theme_all();
    CHECK(white->GetBackgroundColour() == wxColour("#EEDDCC"));
    CHECK(offwht->GetBackgroundColour() == wxColour("#EEDDCC"));

    // And back to Default.
    WindowColourStash::restore_all();
    REQUIRE(Theme::apply(""));
    theme_all();
    check_stock();
    CHECK(WindowColourStash::size() == 0);

    frame->Destroy();
}

TEST_CASE("window colour stash: a colour set by somebody else is left alone", "[ThemeLive]")
{
    ensure_wx_gui();
    ThemeDir themes;
    themes.save("live-a", dark_theme());
    themes.save("live-b", light_theme());

    auto* frame = new wxFrame(nullptr, wxID_ANY, "t");
    auto* panel = new wxPanel(frame);
    panel->SetBackgroundColour(wxColour("#FFFFFF"));

    REQUIRE(Theme::apply("live-a"));
    theme_like_update_dark_ui(panel);
    REQUIRE(panel->GetBackgroundColour() == wxColour("#112233"));

    // The window's own code recolours it after it was themed: that is its colour now.
    panel->SetBackgroundColour(wxColour("#ABCDEF"));
    WindowColourStash::restore_all();
    CHECK(panel->GetBackgroundColour() == wxColour("#ABCDEF"));

    // ... and it is what the next theme starts from.
    REQUIRE(Theme::apply("live-b"));
    theme_like_update_dark_ui(panel);
    CHECK(panel->GetBackgroundColour() == wxColour("#ABCDEF"));

    frame->Destroy();
}

TEST_CASE("window colour stash: a window that had no colour of its own gets that back", "[ThemeLive]")
{
    ensure_wx_gui();
    auto* frame = new wxFrame(nullptr, wxID_ANY, "t");
    auto* panel = new wxPanel(frame);
    REQUIRE_FALSE(panel->UseBgCol());
    const wxColour inherited = panel->GetBackgroundColour();

    // What UpdateDarkUI does to a window that followed its parent: pin a colour on it.
    const WindowColourStash::Stock stock = WindowColourStash::snapshot(panel);
    CHECK_FALSE(stock.has_bg);
    panel->SetBackgroundColour(wxColour("#112233"));
    WindowColourStash::commit(panel, stock);
    CHECK(panel->UseBgCol());

    WindowColourStash::restore_all();
    CHECK_FALSE(panel->UseBgCol());
    CHECK(panel->GetBackgroundColour() == inherited);

    frame->Destroy();
}

TEST_CASE("window colour stash: an entry goes with its window", "[ThemeLive]")
{
    ensure_wx_gui();
    ThemeDir themes;
    themes.save("live-a", dark_theme());

    auto* frame = new wxFrame(nullptr, wxID_ANY, "t");
    auto* panel = new wxPanel(frame);
    panel->SetBackgroundColour(wxColour("#FFFFFF"));
    REQUIRE(Theme::apply("live-a"));

    const std::size_t before = WindowColourStash::size();
    theme_like_update_dark_ui(panel);
    CHECK(WindowColourStash::size() == before + 1);
    delete panel;
    CHECK(WindowColourStash::size() == before);
    WindowColourStash::restore_all(); // nothing left to touch, nothing to crash on

    frame->Destroy();
}

#endif // _WIN32
