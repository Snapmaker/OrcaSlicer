#ifndef slic3r_TimelapseDownloadDialog_hpp_
#define slic3r_TimelapseDownloadDialog_hpp_

#include <string>

#include "slic3r/GUI/GUI_Utils.hpp"
#include <wx/dialog.h>

class wxBoxSizer;
class wxPanel;
class wxStaticText;
class wxTextCtrl;

namespace Slic3r {
namespace GUI {

class TimelapseDownloadDialog : public DPIDialog
{
public:
    TimelapseDownloadDialog(const std::string& file_url,
                            const std::string& file_name,
                            const std::string& sn,
                            int file_count,
                            wxWindow* parent = nullptr);

    ~TimelapseDownloadDialog();

    int ShowModal() override;
    std::string get_save_path() const { return m_download_path; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void setup_ui();
    void on_browse_clicked(wxCommandEvent& event);

    std::string m_file_url;
    std::string m_file_name;
    std::string m_sn;
    std::string m_download_path;
    int m_file_count{1};

    wxTextCtrl* m_path_text{nullptr};
};

}} // namespace Slic3r::GUI

#endif // slic3r_TimelapseDownloadDialog_hpp_
