#include <wx/wx.h>

#include "slic3r/GUI/Widgets/PopupWindow.hpp"

#include <cstdio>

class LifecycleApp : public wxApp
{
public:
    bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(LifecycleApp);

int main(int argc, char **argv)
{
    if (!wxEntryStart(argc, argv) || !wxTheApp->CallOnInit())
        return 1;

    // Normal parent destruction and detached children in both lifetime orders.
    for (int order = 0; order < 3; ++order) {
        auto *frame = new wxFrame(nullptr, wxID_ANY, "Popup lifecycle regression");
        auto *popup = new PopupWindow(frame);
        if (order == 0) {
            delete frame;
        } else {
            frame->RemoveChild(popup);
            if (order == 1) {
                delete popup;
                delete frame;
            } else {
                delete frame;
                delete popup;
            }
        }
    }
    std::puts("Parent destruction and both detached-popup lifetime orders: PASS");
    wxTheApp->OnExit();
    wxEntryCleanup();
    return 0;
}
