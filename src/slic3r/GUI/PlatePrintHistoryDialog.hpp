#ifndef slic3r_GUI_PlatePrintHistoryDialog_hpp_
#define slic3r_GUI_PlatePrintHistoryDialog_hpp_

#include <string>

#include <wx/scrolwin.h>

#include "GUI_Utils.hpp"
#include "libslic3r/PlatePrintHistory.hpp"

namespace Slic3r { namespace GUI {

class PartPlate;

// The pop-up behind a plate's history icon: when, where and on which machine the plate was sent,
// newest first, with a "Clear history" button.
class PlatePrintHistoryDialog : public DPIDialog
{
public:
    PlatePrintHistoryDialog(wxWindow *parent, PartPlate *plate, const wxString &plate_label);
    ~PlatePrintHistoryDialog() override;

    void on_dpi_changed(const wxRect &suggested_rect) override;

    // Display names, shared with the unit-free parts of the recorder.
    static wxString connection_label(const std::string &connection_key);
    static wxString action_label(PlateHistory::Action action);

private:
    void rebuild();
    void on_clear();

    PartPlate        *m_plate { nullptr };
    wxScrolledWindow *m_list { nullptr };
    wxWindow         *m_clear_button { nullptr };
};

}} // namespace Slic3r::GUI

#endif
