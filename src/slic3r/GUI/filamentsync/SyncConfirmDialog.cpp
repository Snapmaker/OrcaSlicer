#include "SyncConfirmDialog.hpp"

#include "slic3r/GUI/I18N.hpp"

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

} // namespace GUI
} // namespace Slic3r
