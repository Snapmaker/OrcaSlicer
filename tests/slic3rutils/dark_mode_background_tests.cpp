#include <catch2/catch.hpp>

// Dark mode's grey band behind labels (Device page, Preferences, most dialogs): UpdateDarkUI took
// the system button face (#F0F0F0) that an uncoloured wxStaticText / wxPanel reports, mapped it to
// #3F3F46 through gDarkColors and pinned that on the control, while the window behind it is
// #2D2D31. DarkModeBackground decides what a window's dark background is; these cases build real
// (never shown) windows so the MSW default colours and transparency rules are the real ones.
// Windows only: the band is an MSW artefact, and the other platforms' CI runs have no display.
#ifdef _WIN32

#include "slic3r/GUI/DarkModeBackground.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/Widgets/StaticBox.hpp"

#include <wx/app.h>
#include <wx/frame.h>
#include <wx/init.h>
#include <wx/panel.h>
#include <wx/settings.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/dcmemory.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/imagpng.h>
#include <wx/msw/wrapwin.h>

using namespace Slic3r::GUI;

namespace {

// A plain wxApp, not GUI_App: only window creation is needed, not the slicer.
void ensure_wx_gui()
{
    static bool started = false;
    // Another test file may have started the app already (theme_live_tests.cpp).
    if (started || wxApp::GetInstance() != nullptr)
        return;
    started = true;
    wxApp::SetInstance(new wxApp());
    static wxChar  arg0[] = wxT("slic3rutils_tests");
    static wxChar *argv[] = {arg0, nullptr};
    static int     argc   = 1;
    REQUIRE(wxEntryStart(argc, argv));
}

struct DarkModeOn
{
    DarkModeOn() { StateColor::SetDarkMode(true); }
    ~DarkModeOn() { StateColor::SetDarkMode(false); }
};

const wxColour kWindowDark("#2D2D31");  // dark twin of #FFFFFF
const wxColour kBtnFaceDark("#3F3F46"); // dark twin of #F0F0F0: the band

} // namespace

TEST_CASE("dark mode keeps uncoloured labels and panels on their parent's colour", "[DarkModeBackground]")
{
    ensure_wx_gui();
    const wxColour btnface = wxSystemSettings::GetColour(wxSYS_COLOUR_BTNFACE);

    auto *frame = new wxFrame(nullptr, wxID_ANY, "t");
    auto *outer = new wxPanel(frame);
    outer->SetBackgroundColour(*wxWHITE);                 // what the Device page / Preferences do
    auto *label       = new wxStaticText(outer, wxID_ANY, "Layer: 55/160");
    auto *inner       = new wxPanel(outer);               // StatusPanel's task_name_panel
    auto *inner_label = new wxStaticText(inner, wxID_ANY, "Cube");
    auto *ff_panel    = new wxPanel(outer);
    ff_panel->SetBackgroundColour(wxColour(240, 240, 240)); // FlashForge's deliberate grey panels
    auto *text = new wxTextCtrl(outer, wxID_ANY, "1.0");

    INFO("the premise: an uncoloured label reports the system button face");
    REQUIRE_FALSE(label->UseBgCol());
    REQUIRE(label->GetBackgroundColour() == btnface);

    {
        DarkModeOn dark;

        // UpdateDarkUI walks parents first; the explicitly white panel is mapped as before.
        DarkBackground outer_bg = dark_mode_background_for(outer);
        CHECK(outer_bg.action == DarkBackground::Action::Map);
        CHECK(outer_bg.colour == kWindowDark);
        outer->SetBackgroundColour(outer_bg.colour);

        DarkBackground label_bg = dark_mode_background_for(label);
        CHECK(label_bg.action == DarkBackground::Action::LeaveUnset);
        CHECK(label_bg.colour == kWindowDark);
        CHECK(label_bg.colour != kBtnFaceDark);

        DarkBackground inner_bg = dark_mode_background_for(inner);
        CHECK(inner_bg.action == DarkBackground::Action::SetParent);
        CHECK(inner_bg.colour == kWindowDark);
        inner->SetBackgroundColour(inner_bg.colour);

        DarkBackground inner_label_bg = dark_mode_background_for(inner_label);
        CHECK(inner_label_bg.action == DarkBackground::Action::LeaveUnset);
        CHECK(inner_label_bg.colour == kWindowDark);

        // An explicit #F0F0F0 is a colour somebody chose: it still maps to its dark twin.
        DarkBackground ff_bg = dark_mode_background_for(ff_panel);
        CHECK(ff_bg.action == DarkBackground::Action::Map);
        CHECK(ff_bg.colour == kBtnFaceDark);

        // Text fields default to the window colour, not the button face: still mapped.
        DarkBackground text_bg = dark_mode_background_for(text);
        CHECK(text_bg.action == DarkBackground::Action::Map);
    }

    frame->Destroy();
}

TEST_CASE("custom widgets copy the colour an uncoloured parent panel shows", "[DarkModeBackground]")
{
    ensure_wx_gui();

    auto *frame = new wxFrame(nullptr, wxID_ANY, "t");
    auto *outer = new wxPanel(frame);
    outer->SetBackgroundColour(*wxWHITE);
    auto *inner = new wxPanel(outer); // no colour of its own: MSW paints it white

    // Label, StaticBox and SwitchButton take this in their constructors; it used to be #F0F0F0,
    // a light-grey band on white in light mode and a #3F3F46 one in dark mode.
    CHECK(StaticBox::GetParentBackgroundColor(inner) == *wxWHITE);
    CHECK(visible_parent_background(inner) == *wxWHITE);

    frame->Destroy();
}

// Paints a (never shown) panel with a label through WM_PRINT, the way Windows draws it, and reads
// a pixel inside the label below its text: its background as the user sees it. Writes the pictures
// to %TEMP%\darkmode_label_{old,new}.png for a look.
namespace {

wxColour paint_and_sample(bool old_behaviour, const wxString &png_name)
{
    auto *frame = new wxFrame(nullptr, wxID_ANY, "t", wxDefaultPosition, wxSize(400, 200));
    auto *outer = new wxPanel(frame, wxID_ANY, wxPoint(0, 0), wxSize(300, 60));
    outer->SetBackgroundColour(*wxWHITE);
    auto *label = new wxStaticText(outer, wxID_ANY, "Layer: 55/160", wxPoint(10, 10), wxSize(200, 30));

    wxColour sampled;
    {
        DarkModeOn dark;
        outer->SetBackgroundColour(dark_mode_background_for(outer).colour);
        const DarkBackground d = dark_mode_background_for(label);
        if (old_behaviour)
            label->SetBackgroundColour(StateColor::darkModeColorFor(label->GetBackgroundColour())); // pre-fix UpdateDarkUI
        else if (d.action != DarkBackground::Action::LeaveUnset)
            label->SetBackgroundColour(d.colour);
        label->SetForegroundColour(wxColour("#EFEFF0"));

        wxBitmap   bmp(300, 60, 24);
        wxMemoryDC mdc(bmp);
        mdc.SetBackground(*wxRED_BRUSH);
        mdc.Clear();
        HDC hdc = (HDC) mdc.GetHDC();
        ::SendMessage((HWND) outer->GetHWND(), WM_PRINT, (WPARAM) hdc, PRF_CLIENT | PRF_CHILDREN | PRF_ERASEBKGND);
        mdc.GetPixel(12, 36, &sampled); // inside the label, below the text
        mdc.SelectObject(wxNullBitmap);
        if (!wxImage::FindHandler(wxBITMAP_TYPE_PNG))
            wxImage::AddHandler(new wxPNGHandler);
        bmp.SaveFile(wxFileName::GetTempDir() + "\\" + png_name, wxBITMAP_TYPE_PNG);
    }
    frame->Destroy();
    return sampled;
}

} // namespace

TEST_CASE("a dark-mode label is painted in its panel's colour", "[DarkModeBackground]")
{
    ensure_wx_gui();
    const wxColour before = paint_and_sample(true, "darkmode_label_old.png");
    const wxColour after  = paint_and_sample(false, "darkmode_label_new.png");
    INFO("old label background " << before.GetAsString(wxC2S_HTML_SYNTAX).ToStdString() << ", new "
                                 << after.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
    if (before == *wxRED && after == *wxRED) {
        WARN("WM_PRINT painted nothing on this machine; pixel check skipped");
        return;
    }
    CHECK(before == kBtnFaceDark); // the band, reproduced
    CHECK(after == kWindowDark);   // gone: the panel's own colour
}

#endif // _WIN32
