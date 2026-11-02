#ifndef slic3r_ModelMall_hpp_
#define slic3r_ModelMall_hpp_

#include "I18N.hpp"
#include "GUI_Utils.hpp"   // H2 3.1：DPIFrame 基类（原 Plater.hpp 传递）
namespace Slic3r { namespace GUI { class Plater; } }   // H2 3.1：Plater 仅指针形态——前向声明替代传递包含

#include <wx/font.h>
#include <wx/colour.h>
#include <wx/settings.h>
#include <wx/string.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/gauge.h>
#include <wx/button.h>
#include <wx/bitmap.h>
#include <wx/image.h>
#include <wx/icon.h>
#include <wx/dialog.h>

#if wxUSE_WEBVIEW_IE
#include "wx/msw/webview_ie.h"
#endif
#if wxUSE_WEBVIEW_EDGE
#include "wx/msw/webview_edge.h"
#endif

#include "Widgets/WebView.hpp"
#include "wx/webviewarchivehandler.h"
#include "wx/webviewfshandler.h"

#include <curl/curl.h>
#include <wx/webrequest.h>
#include "wxExtensions.hpp"
#include "Widgets/StepCtrl.hpp"
#include "Widgets/Button.hpp"


#define MODEL_MALL_PAGE_SIZE wxSize(FromDIP(1400 * 0.85), FromDIP(1040 * 0.75))
#define MODEL_MALL_PAGE_CONTROL_SIZE wxSize(FromDIP(1400 * 0.85), FromDIP(40 * 0.75))
#define MODEL_MALL_PAGE_WEB_SIZE wxSize(FromDIP(1400 * 0.85), FromDIP(1000 * 0.75))

namespace Slic3r { namespace GUI {

    class ModelMallDialog : public DPIFrame
    {
    public:
        ModelMallDialog(Plater* plater = nullptr);
        ~ModelMallDialog();

        void OnScriptMessage(wxWebViewEvent& evt);
        void on_dpi_changed(const wxRect& suggested_rect) override;
        void on_show(wxShowEvent& event);
        void on_back(wxMouseEvent& evt);
        void on_forward(wxMouseEvent& evt);
        void go_to_url(wxString url);
        void show_control(bool show);
        void go_to_mall(wxString url);
        void go_to_publish(wxString url);
        void on_refresh(wxMouseEvent& evt);
    public:
        wxPanel* m_web_control_panel{nullptr};
        wxWebView* m_browser{nullptr};
        wxString m_url;
    };

}} // namespace Slic3r::GUI

#endif
