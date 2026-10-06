#ifndef _Slic3r_GUI_PrinterErrorMsgDlg_hpp_
#define _Slic3r_GUI_PrinterErrorMsgDlg_hpp_

#include <map>
#include <string>
#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include "slic3r/GUI/FlashForge/MultiComDef.hpp"
#include "slic3r/GUI/FlashForge/MultiComEvent.hpp"
#include "slic3r/GUI/Widgets/FFButton.hpp"

namespace Slic3r { namespace GUI {

class PrinterErrorMsgDlg : public wxDialog
{
public:
    PrinterErrorMsgDlg(wxWindow *parent, com_id_t comId, const std::string &errorCode);

    static bool isErrorCodeHandled(const std::string &errorCode);

private:
    struct error_code_data_t {
        wxString message;
        std::string wikiUrl;
    };
    using error_code_data_map_t = std::map<std::string, error_code_data_t>;

    void setupErrorCode(const std::string &errorCode);

    void onOperator1(wxCommandEvent &event);

    void onOperator2(wxCommandEvent &event);

    void onConnectionExit(ComConnectionExitEvent &event);

    void onDevDetailUpdate(ComDevDetailUpdateEvent &event);

    static void initErrorCodeDataMap();

private:
    com_id_t m_comId;
    std::string m_errorCode;
    wxStaticText *m_titleLbl;
    wxStaticText *m_msgLbl;
    FFButton *m_operator1Btn;
    FFButton *m_operator2Btn;
    static error_code_data_map_t s_errorCodeDataMap;
};

}} // namespace Slic3r::GUI

#endif
