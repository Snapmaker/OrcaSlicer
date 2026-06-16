#include "SyncConfirmDialog.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r
{
namespace GUI
{

SyncConfirmDialog::SyncConfirmDialog(wxWindow* parent, const wxString& message, long style)
    : MessageDialog(parent, message, _L("Synchronize printer information"), style)
{
}

SyncRichConfirmDialog::SyncRichConfirmDialog(wxWindow* parent, const wxString& message, long style)
    : RichMessageDialog(parent, message, _L("Synchronize printer information"), style)
{
}

void SyncRichConfirmDialog::navigateToTab(MainFrame::TabPosition pos)
{
    if (wxGetApp().mainframe)
        wxGetApp().mainframe->select_tab(pos);
}

} // namespace GUI
} // namespace Slic3r
