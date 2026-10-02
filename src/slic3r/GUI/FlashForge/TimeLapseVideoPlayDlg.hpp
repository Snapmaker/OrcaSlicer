#ifndef slic3r_GUI_TimeLapseVideoPlayDlg_hpp_
#define slic3r_GUI_TimeLapseVideoPlayDlg_hpp_

#include <wx/wx.h>
#include <wx/webview.h>
#include "slic3r/GUI/GUI_Utils.hpp"

namespace Slic3r { namespace GUI {


    class TimeLapseVideoPlayDlg : public wxDialog
    {
    public:
        TimeLapseVideoPlayDlg(wxWindow* parent, const wxString& filepath, const wxString& video_url, int width, int height);
        ~TimeLapseVideoPlayDlg();

    private:
        bool generate_html();
    private:
        wxWebView*  m_webview{nullptr};
        wxString    m_video_url;
        wxString    m_filepath;
        int         m_width;
        int         m_height;
    };

}}

#endif