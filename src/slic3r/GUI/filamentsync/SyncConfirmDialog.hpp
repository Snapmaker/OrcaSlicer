#pragma once

#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/MainFrame.hpp"

namespace Slic3r
{
namespace GUI
{

// Helper dialog for filament sync — always uses "Synchronize printer information" as the title.
// Only the message content and button style need to be provided.
class SyncConfirmDialog : public MessageDialog
{
public:
    SyncConfirmDialog(wxWindow* parent, const wxString& message, long style = wxOK);
    ~SyncConfirmDialog() override = default;
};

// Rich version — supports SetYesNoLabels / ShowCheckBox etc.
class SyncRichConfirmDialog : public RichMessageDialog
{
public:
    SyncRichConfirmDialog(wxWindow* parent, const wxString& message, long style = wxOK);
    ~SyncRichConfirmDialog() override = default;

    /// Navigate to the specified tab (e.g., MainFrame::tpMonitor, MainFrame::tpPreview).
    void navigateToTab(MainFrame::TabPosition pos);
};

} // namespace GUI
} // namespace Slic3r
