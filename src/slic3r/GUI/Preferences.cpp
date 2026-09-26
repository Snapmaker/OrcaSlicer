#include "Preferences.hpp"
#include "OptionsGroup.hpp"
#include "GUI_App.hpp"
#include "MainFrame.hpp"
#include "Plater.hpp"
#include "NotificationManager.hpp"
#include "MsgDialog.hpp"
#include "PresetMirror.hpp"
#include "I18N.hpp"
#include "libslic3r/AppConfig.hpp"
#include <wx/language.h>
#include <wx/notebook.h>
#include "Notebook.hpp"
#include "OG_CustomCtrl.hpp"
#include "wx/graphics.h"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/RadioBox.hpp"
#include "Widgets/TextInput.hpp"
#include <wx/listimpl.cpp>
#include <wx/display.h>
#include <wx/hyperlink.h>
#include <algorithm>
#include <map>
#include <memory>

#include "sentry_wrapper/SentryWrapper.hpp"
#include "SSWCP.hpp"

#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
#include "dark_mode.hpp"
#endif // _MSW_DARK_MODE
#endif //__WINDOWS__

namespace Slic3r { namespace GUI {

WX_DEFINE_LIST(RadioSelectorList);
wxDEFINE_EVENT(EVT_PREFERENCES_SELECT_TAB, wxCommandEvent);


class MyscrolledWindow : public wxScrolledWindow {
public:
    MyscrolledWindow(wxWindow* parent,
        wxWindowID id = wxID_ANY,
        const wxPoint& pos = wxDefaultPosition,
        const wxSize& size = wxDefaultSize,
        long style = wxVSCROLL) : wxScrolledWindow(parent, id, pos, size, style) {}

    bool ShouldScrollToChildOnFocus(wxWindow* child) override { return false; }
};


wxBoxSizer *PreferencesDialog::create_item_title(wxString title, wxWindow *parent, wxString tooltip)
{
    wxBoxSizer *m_sizer_title = new wxBoxSizer(wxHORIZONTAL);

    auto m_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, 0);
    m_title->SetForegroundColour(DESIGN_GRAY800_COLOR);
    m_title->SetFont(::Label::Head_13);
    m_title->Wrap(-1);
    //m_title->SetToolTip(tooltip);

    auto m_line = new wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, 1), wxTAB_TRAVERSAL);
    m_line->SetBackgroundColour(DESIGN_GRAY400_COLOR);

    m_sizer_title->Add(m_title, 0, wxALIGN_CENTER | wxALL, 3);
    m_sizer_title->Add(0, 0, 0,  wxLEFT, 9);
    //m_sizer_title->Add(m_line, 0, wxEXPAND, 0);
    wxBoxSizer *sizer_line = new wxBoxSizer(wxVERTICAL);
    sizer_line->Add( m_line, 0, wxEXPAND, 0 );
    m_sizer_title->Add( sizer_line, 1, wxALIGN_CENTER, 0 );
    //m_sizer_title->Add( 0, 0, 0, wxEXPAND|wxLEFT, 80 );

    return m_sizer_title;
}

std::tuple<wxBoxSizer*, ComboBox*> PreferencesDialog::create_item_combobox_base(wxString title, wxWindow* parent, wxString tooltip, std::string param, std::vector<wxString> vlist, unsigned int current_index)
{
    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

    auto combo_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_13);
    combo_title->SetToolTip(tooltip);
    combo_title->Wrap(-1);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER | wxALL, 3);

    auto combobox = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_13);
    combobox->GetDropDown().SetFont(::Label::Body_13);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) {
        combobox->Append(*iter);
    }

    combobox->SetSelection(current_index);

    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER, 0);

    return {m_sizer_combox, combobox};
}

wxBoxSizer* PreferencesDialog::create_item_combobox(wxString title, wxWindow* parent, wxString tooltip, std::string param, std::vector<wxString> vlist)
{
    unsigned int current_index = 0;

    auto current_setting = app_config->get(param);
    if (!current_setting.empty()) {
        current_index = atoi(current_setting.c_str());
    }

    auto [sizer, combobox] = create_item_combobox_base(title, parent, tooltip, param, vlist, current_index);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param](wxCommandEvent& e) {
        app_config->set(param, std::to_string(e.GetSelection()));
        e.Skip();
    });

    return sizer;
}

wxBoxSizer *PreferencesDialog::create_item_combobox(wxString title, wxWindow *parent, wxString tooltip, std::string param, std::vector<wxString> vlist, std::vector<std::string> config_name_index)
{
    assert(vlist.size() == config_name_index.size());
    unsigned int current_index = 0;

    auto current_setting = app_config->get(param);
    if (!current_setting.empty()) {
        auto compare  = [current_setting](string possible_setting) { return current_setting == possible_setting; };
        auto iterator = find_if(config_name_index.begin(), config_name_index.end(), compare);
        current_index = iterator - config_name_index.begin();
    }

    auto [sizer, combobox] = create_item_combobox_base(title, parent, tooltip, param, vlist, current_index);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, config_name_index](wxCommandEvent& e) {
        app_config->set(param, config_name_index[e.GetSelection()]);
        e.Skip();
    });

    return sizer;
}

wxBoxSizer *PreferencesDialog::create_item_language_combobox(
    wxString title, wxWindow *parent, wxString tooltip, int padding_left, std::string param, std::vector<const wxLanguageInfo *> vlist)
{
    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

    auto combo_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_13);
    combo_title->SetToolTip(tooltip);
    combo_title->Wrap(-1);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER | wxALL, 3);


    auto combobox = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_13);
    combobox->GetDropDown().SetFont(::Label::Body_13);
    auto language = app_config->get(param);
    m_current_language_selected = -1;
    std::vector<wxString>::iterator iter;
    for (size_t i = 0; i < vlist.size(); ++i) {
        auto language_name = vlist[i]->Description;

        if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CHINESE_SIMPLIFIED)) {
            language_name = wxString::FromUTF8("\xe4\xb8\xad\xe6\x96\x87\x28\xe7\xae\x80\xe4\xbd\x93\x29");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CHINESE)) {
            language_name = wxString::FromUTF8("\xe4\xb8\xad\xe6\x96\x87\x28\xe7\xb9\x81\xe4\xbd\x93\x29");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_SPANISH)) {
            language_name = wxString::FromUTF8("\x45\x73\x70\x61\xc3\xb1\x6f\x6c");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_GERMAN)) {
            language_name = wxString::FromUTF8("Deutsch");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CZECH)) {
            language_name = wxString::FromUTF8("Czech");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_SWEDISH)) {
            language_name = wxString::FromUTF8("\x53\x76\x65\x6e\x73\x6b\x61"); //Svenska
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_DUTCH)) {
            language_name = wxString::FromUTF8("Nederlands");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_FRENCH)) {
            language_name = wxString::FromUTF8("\x46\x72\x61\x6E\xC3\xA7\x61\x69\x73");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_HUNGARIAN)) {
            language_name = wxString::FromUTF8("Magyar");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_JAPANESE)) {
            language_name = wxString::FromUTF8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_ITALIAN)) {
            language_name = wxString::FromUTF8("\x69\x74\x61\x6c\x69\x61\x6e\x6f");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_KOREAN)) {
            language_name = wxString::FromUTF8("\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_RUSSIAN)) {
            language_name = wxString::FromUTF8("\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_UKRAINIAN)) {
            language_name = wxString::FromUTF8("Ukrainian");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_TURKISH)) {
            language_name = wxString::FromUTF8("Turkish");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_POLISH)) {
            language_name = wxString::FromUTF8("Polski");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_CATALAN)) {
            language_name = wxString::FromUTF8("Catalan");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_PORTUGUESE_BRAZILIAN)) {
            language_name = wxString::FromUTF8("Português (Brasil)");
        }
        else if (vlist[i] == wxLocale::GetLanguageInfo(wxLANGUAGE_LITHUANIAN)) {
            language_name = wxString::FromUTF8("Lietuvių");
        }

        if (app_config->get(param) == vlist[i]->CanonicalName) {
            m_current_language_selected = i;
        }
        combobox->Append(language_name);
    }
    if (m_current_language_selected == -1 && language.size() >= 5) {
        language = language.substr(0, 2);
        for (size_t i = 0; i < vlist.size(); ++i) {
            if (vlist[i]->CanonicalName.StartsWith(language)) {
                m_current_language_selected = i;
                break;
            }
        }
    }
    combobox->SetSelection(m_current_language_selected);

    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER, 0);

    combobox->Bind(wxEVT_LEFT_DOWN, [this, combobox](wxMouseEvent &e) {
        m_current_language_selected = combobox->GetSelection();
        e.Skip();
    });

    combobox->Bind(wxEVT_COMBOBOX, [this, param, vlist, combobox](wxCommandEvent &e) {
        if (combobox->GetSelection() == m_current_language_selected)
            return;

        if (e.GetString().mb_str() != app_config->get(param)) {
            {
                //check if the project has changed
                if (wxGetApp().plater()->is_project_dirty()) {
                    auto result = MessageDialog(static_cast<wxWindow*>(this), _L("The current project has unsaved changes, save it before continue?"),
                        wxString(SLIC3R_APP_FULL_NAME) + " - " + _L("Save"), wxYES_NO | wxCANCEL | wxYES_DEFAULT | wxCENTRE).ShowModal();

                    if (result == wxID_YES) {
                        wxGetApp().plater()->save_project();
                    }
                }


                // the dialog needs to be destroyed before the call to switch_language()
                // or sometimes the application crashes into wxDialogBase() destructor
                // so we put it into an inner scope
                MessageDialog msg_wingow(nullptr, _L("Switching the language requires application restart.\n") + "\n" + _L("Do you want to continue?"),
                                         L("Language selection"), wxICON_QUESTION | wxOK | wxCANCEL);
                if (msg_wingow.ShowModal() == wxID_CANCEL) {
                    combobox->SetSelection(m_current_language_selected);
                    return;
                }
            }

            auto check = [this](bool yes_or_no) {
                // if (yes_or_no)
                //    return true;
                int act_btns = ActionButtons::SAVE;
                return wxGetApp().check_and_keep_current_preset_changes(_L("Switching application language"),
                                                                        _L("Switching application language while some presets are modified."), act_btns);
            };

            m_current_language_selected = combobox->GetSelection();
            if (m_current_language_selected >= 0 && m_current_language_selected < vlist.size()) {
                app_config->set(param, vlist[m_current_language_selected]->CanonicalName.ToUTF8().data());

                wxGetApp().load_language(vlist[m_current_language_selected]->CanonicalName, false);
                Close();
                // Reparent(nullptr);
                GetParent()->RemoveChild(this);
                wxGetApp().recreate_GUI(_L("Changing application language"));
            }
        }

        e.Skip();
    });

    return m_sizer_combox;
}

wxBoxSizer *PreferencesDialog::create_item_region_combobox(wxString title, wxWindow *parent, wxString tooltip, std::vector<wxString> vlist)
{
    std::vector<wxString> local_regions = {"Asia-Pacific", "Chinese Mainland", "Europe", "North America", "Others"};

    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

    auto combo_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_13);
    combo_title->SetToolTip(tooltip);
    combo_title->Wrap(-1);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER | wxALL, 3);

    auto combobox = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_13);
    combobox->GetDropDown().SetFont(::Label::Body_13);
    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER, 0);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) { combobox->Append(*iter); }

    AppConfig * config       = GUI::wxGetApp().app_config;

    int         current_region = 0;
    if (!config->get("region").empty()) {
        std::string country_code = config->get("region");
        for (auto i = 0; i < vlist.size(); i++) {
            if (local_regions[i].ToStdString() == country_code) {
                combobox->SetSelection(i);
                current_region = i;
            }
        }
    }

    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this, combobox, current_region, local_regions](wxCommandEvent &e) {
        auto region_index = e.GetSelection();
        auto region       = local_regions[region_index];

        // snapmaker
        AppConfig* config = GUI::wxGetApp().app_config;
        combobox->SetSelection(region_index);
        auto info = wxGetApp().sm_get_userinfo();
        if (info && info->is_user_login()) {
            MessageDialog msg_wingow(this, _L("Changing the region will log out your account and restart.\n") + "\n" + _L("Do you want to continue?"),
                                     L("Region selection"), wxICON_QUESTION | wxOK | wxCANCEL);

            if (msg_wingow.ShowModal() == wxID_CANCEL) {
                combobox->SetSelection(current_region);
                return;
            } else {
                wxGetApp().sm_request_user_logout();
                config->set("region", region.ToStdString());
                EndModal(wxID_CANCEL);
            }

            Close();
            GetParent()->RemoveChild(this);
            wxGetApp().recreate_GUI(_L("Change Region"));
        } else {
            MessageDialog msg_wingow(nullptr,
                                     _L("Switching the region requires application restart.\n") + "\n" + _L("Do you want to continue?"),
                                     L("Region selection"), wxICON_QUESTION | wxOK | wxCANCEL);
            if (msg_wingow.ShowModal() == wxID_CANCEL) {
                combobox->SetSelection(current_region);
                return;
            }
            config->set("region", region.ToStdString());
            Close();
            GetParent()->RemoveChild(this);
            wxGetApp().recreate_GUI(_L("Change Region"));
        }

        wxGetApp().update_publish_status();
        e.Skip();
    });

    return m_sizer_combox;
}

wxBoxSizer *PreferencesDialog::create_item_loglevel_combobox(wxString title, wxWindow *parent, wxString tooltip, std::vector<wxString> vlist)
{
    wxBoxSizer *m_sizer_combox = new wxBoxSizer(wxHORIZONTAL);
    m_sizer_combox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

    auto combo_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    combo_title->SetFont(::Label::Body_13);
    combo_title->SetToolTip(tooltip);
    combo_title->Wrap(-1);
    m_sizer_combox->Add(combo_title, 0, wxALIGN_CENTER | wxALL, 3);

    auto                            combobox = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    combobox->SetFont(::Label::Body_13);
    combobox->GetDropDown().SetFont(::Label::Body_13);

    std::vector<wxString>::iterator iter;
    for (iter = vlist.begin(); iter != vlist.end(); iter++) { combobox->Append(*iter); }

    auto severity_level = app_config->get("log_severity_level");
    if (!severity_level.empty()) { combobox->SetValue(severity_level); }

    m_sizer_combox->Add(combobox, 0, wxALIGN_CENTER, 0);

    //// save config
    combobox->GetDropDown().Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
        auto level = Slic3r::get_string_logging_level(e.GetSelection());
        Slic3r::set_logging_level(Slic3r::level_string_to_boost(level));
        app_config->set("log_severity_level",level);
        e.Skip();
     });
    return m_sizer_combox;
}


wxBoxSizer *PreferencesDialog::create_item_multiple_combobox(
    wxString title, wxWindow *parent, wxString tooltip, int padding_left, std::string param, std::vector<wxString> vlista, std::vector<wxString> vlistb)
{
    std::vector<wxString> params;
    Split(app_config->get(param), "/", params);

    std::vector<wxString>::iterator iter;

   wxBoxSizer *m_sizer_tcombox= new wxBoxSizer(wxHORIZONTAL);
   m_sizer_tcombox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

   auto combo_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
   combo_title->SetToolTip(tooltip);
   combo_title->Wrap(-1);
   combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
   combo_title->SetFont(::Label::Body_13);
   m_sizer_tcombox->Add(combo_title, 0, wxALIGN_CENTER | wxALL, 3);

   auto combobox_left = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
   combobox_left->SetFont(::Label::Body_13);
   combobox_left->GetDropDown().SetFont(::Label::Body_13);


   for (iter = vlista.begin(); iter != vlista.end(); iter++) { combobox_left->Append(*iter); }
   combobox_left->SetValue(std::string(params[0].mb_str()));
   m_sizer_tcombox->Add(combobox_left, 0, wxALIGN_CENTER, 0);

   auto combo_title_add = new wxStaticText(parent, wxID_ANY, wxT("+"), wxDefaultPosition, wxDefaultSize, 0);
   combo_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
   combo_title->SetFont(::Label::Body_13);
   combo_title_add->Wrap(-1);
   m_sizer_tcombox->Add(combo_title_add, 0, wxALIGN_CENTER | wxALL, 3);

   auto combobox_right = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
   combobox_right->SetFont(::Label::Body_13);
   combobox_right->GetDropDown().SetFont(::Label::Body_13);

   for (iter = vlistb.begin(); iter != vlistb.end(); iter++) { combobox_right->Append(*iter); }
   combobox_right->SetValue(std::string(params[1].mb_str()));
   m_sizer_tcombox->Add(combobox_right, 0, wxALIGN_CENTER, 0);

    // save config
    combobox_left->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, combobox_right](wxCommandEvent &e) {
        auto config = e.GetString() + wxString("/") + combobox_right->GetValue();
        app_config->set(param, std::string(config.mb_str()));
        e.Skip();
    });

    combobox_right->GetDropDown().Bind(wxEVT_COMBOBOX, [this, param, combobox_left](wxCommandEvent &e) {
        auto config = combobox_left->GetValue() + wxString("/") + e.GetString();
        app_config->set(param, std::string(config.mb_str()));
        e.Skip();
    });

    return m_sizer_tcombox;
}

// Free-text variant of create_item_input (no digits-only validator).
wxBoxSizer *PreferencesDialog::create_item_text_input(wxString title, wxWindow *parent, wxString tooltip, std::string param)
{
    wxBoxSizer *sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto        input_title = new wxStaticText(parent, wxID_ANY, title);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_13);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(-1);

    auto       input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));

    sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    sizer_input->Add(input, 0, wxALIGN_CENTER_VERTICAL, 0);

    std::function<void()> commit = [this, param, input]() {
        app_config->set(param, std::string(input->GetTextCtrl()->GetValue().ToUTF8().data()));
        app_config->save();
    };
    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [commit](wxCommandEvent &e) { commit(); e.Skip(); });
    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [commit](wxFocusEvent &e) { commit(); e.Skip(); });

    return sizer_input;
}

wxBoxSizer *PreferencesDialog::create_item_input(wxString title, wxString title2, wxWindow *parent, wxString tooltip, std::string param, std::function<void(wxString)> onchange)
{
    wxBoxSizer *sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto        input_title   = new wxStaticText(parent, wxID_ANY, title);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_13);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(-1);

    auto       input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_DIGITS);
    input->GetTextCtrl()->SetValidator(validator);

    auto second_title = new wxStaticText(parent, wxID_ANY, title2, wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    second_title->SetFont(::Label::Body_13);
    second_title->SetToolTip(tooltip);
    second_title->Wrap(-1);

    sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    sizer_input->Add(input, 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 3);
    sizer_input->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, param, input, onchange](wxCommandEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        app_config->save();
        onchange(value);
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, param, input, onchange](wxFocusEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        app_config->set(param, std::string(value.mb_str()));
        onchange(value);
        e.Skip();
    });

    return sizer_input;
}

wxBoxSizer *PreferencesDialog::create_camera_orbit_mult_input(wxString title, wxWindow *parent, wxString tooltip)
{
    wxBoxSizer *sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto        input_title   = new wxStaticText(parent, wxID_ANY, title);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_13);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(-1);
    auto param = "camera_orbit_mult";

    auto       input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_NUMERIC);
    input->GetTextCtrl()->SetValidator(validator);

    sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    sizer_input->Add(input, 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 3);

    const double min = 0.05;
    const double max = 2.0;

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, param, input, min, max](wxCommandEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        double conv = 1.0;
        if (value.ToCDouble(&conv)) {
            conv = conv < min ? min : conv > max ? max : conv;
            auto strval = std::string(wxString::FromCDouble(conv, 2).mb_str());
            input->GetTextCtrl()->SetValue(strval);
            app_config->set(param, strval);
            app_config->save();
        }
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, param, input, min, max](wxFocusEvent &e) {
        auto value = input->GetTextCtrl()->GetValue();
        double conv = 1.0;
        if (value.ToCDouble(&conv)) {
            conv = conv < min ? min : conv > max ? max : conv;
            auto strval = std::string(wxString::FromCDouble(conv, 2).mb_str());
            input->GetTextCtrl()->SetValue(strval);
            app_config->set(param, strval);
        }
        e.Skip();
    });

    return sizer_input;
}

wxBoxSizer *PreferencesDialog::create_item_backup_input(wxString title, wxWindow *parent, wxString tooltip, std::string param)
{
    wxBoxSizer *m_sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto input_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, 0);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_13);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(-1);

    auto input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_DIGITS);
    input->GetTextCtrl()->SetValidator(validator);


    auto second_title = new wxStaticText(parent, wxID_ANY, _L("Second"), wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    second_title->SetFont(::Label::Body_13);
    second_title->SetToolTip(tooltip);
    second_title->Wrap(-1);

    m_sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    m_sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    m_sizer_input->Add(input, 0, wxALIGN_CENTER_VERTICAL, 0);
    m_sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 3);
    m_sizer_input->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);


    input->GetTextCtrl()->Bind(wxEVT_COMMAND_TEXT_UPDATED, [this, param, input](wxCommandEvent &e) {
        m_backup_interval_time = input->GetTextCtrl()->GetValue();
        e.Skip();
    });

    std::function<void()> backup_interval = [this, param, input]() {
        m_backup_interval_time = input->GetTextCtrl()->GetValue();
        app_config->set("backup_interval", std::string(m_backup_interval_time.mb_str()));
        app_config->save();
        long backup_interval = 0;
        m_backup_interval_time.ToLong(&backup_interval);
        Slic3r::set_backup_interval(backup_interval);
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [backup_interval](wxCommandEvent &e) {
        backup_interval();
        e.Skip();
    });

     input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [backup_interval](wxFocusEvent &e) {
        backup_interval();
        e.Skip();
    });

    if (app_config->get("backup_switch") == "true") {
        input->Enable(true);
        input->Refresh();
    } else {
        input->Enable(false);
        input->Refresh();
    }

    if (param == "backup_interval") { m_backup_interval_textinput = input; }
    return m_sizer_input;
}

// BBS: same layout as the backup interval row, but drives the project auto-save timer
wxBoxSizer *PreferencesDialog::create_item_autosave_input(wxString title, wxWindow *parent, wxString tooltip, std::string param)
{
    wxBoxSizer *m_sizer_input = new wxBoxSizer(wxHORIZONTAL);
    auto input_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, 0);
    input_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    input_title->SetFont(::Label::Body_13);
    input_title->SetToolTip(tooltip);
    input_title->Wrap(-1);

    auto input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_DIGITS);
    input->GetTextCtrl()->SetValidator(validator);

    auto second_title = new wxStaticText(parent, wxID_ANY, _L("Second"), wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    second_title->SetFont(::Label::Body_13);
    second_title->SetToolTip(tooltip);
    second_title->Wrap(-1);

    m_sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    m_sizer_input->Add(input_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    m_sizer_input->Add(input, 0, wxALIGN_CENTER_VERTICAL, 0);
    m_sizer_input->Add(0, 0, 0, wxEXPAND | wxLEFT, 3);
    m_sizer_input->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);

    std::function<void()> autosave_interval = [this, param, input]() {
        app_config->set(param, std::string(input->GetTextCtrl()->GetValue().mb_str()));
        app_config->save();
        wxGetApp().mainframe->update_autosave_timer();
    };

    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [autosave_interval](wxCommandEvent &e) {
        autosave_interval();
        e.Skip();
    });

    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [autosave_interval](wxFocusEvent &e) {
        autosave_interval();
        e.Skip();
    });

    input->Enable(app_config->get("autosave_switch") == "true");
    input->Refresh();

    if (param == "autosave_interval") { m_autosave_interval_textinput = input; }
    return m_sizer_input;
}

// Ultra (G-code archive): the folder the copies go to. Same shape as create_item_downloads - the
// current path, then a Browse button - but with its own label, and it follows the checkbox above.
wxWindow *PreferencesDialog::create_item_gcode_archive_dir(wxWindow *parent, wxString tooltip)
{
    auto item_panel = new wxWindow(parent, wxID_ANY);
    item_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);

    sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    auto label = new wxStaticText(item_panel, wxID_ANY, _L("Storage folder"));
    label->SetForegroundColour(DESIGN_GRAY900_COLOR);
    label->SetFont(::Label::Body_13);
    label->SetToolTip(tooltip);
    label->Wrap(-1);

    auto path_text = new wxStaticText(item_panel, wxID_ANY, wxString::FromUTF8(app_config->get("ultra_gcode_archive_dir")),
                                      wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    path_text->SetForegroundColour(DESIGN_GRAY600_COLOR);
    path_text->SetFont(::Label::Body_13);
    path_text->SetToolTip(tooltip);
    path_text->Wrap(-1);

    auto browse = new Button(item_panel, _L("Browse"));
    browse->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    browse->Bind(wxEVT_BUTTON, [this, path_text, item_panel](wxCommandEvent &e) {
        wxString    current = wxString::FromUTF8(app_config->get("ultra_gcode_archive_dir"));
        wxDirDialog dialog(this, _L("Choose the folder for stored G-code files"), current, wxDD_NEW_DIR_BUTTON);
        if (dialog.ShowModal() == wxID_OK) {
            wxString picked = dialog.GetPath();
            app_config->set("ultra_gcode_archive_dir", std::string(picked.ToUTF8().data()));
            app_config->save();
            path_text->SetLabelText(picked);
            item_panel->Layout();
        }
        e.Skip();
    });

    sizer->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    sizer->Add(path_text, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    sizer->Add(browse, 0, wxALL, FromDIP(5));

    item_panel->SetSizer(sizer);
    item_panel->Layout();
    item_panel->Enable(app_config->get_bool("ultra_gcode_archive"));
    m_gcode_archive_dir_item = item_panel;
    return item_panel;
}

// Ultra (G-code archive): how many sends to keep. Digits only, and clamped to 1..10000 when the
// field is left, so a stray value never makes the archive grow without bound or delete everything.
wxBoxSizer *PreferencesDialog::create_item_gcode_archive_max(wxWindow *parent, wxString tooltip)
{
    const std::string param = "ultra_gcode_archive_max";
    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    auto        title = new wxStaticText(parent, wxID_ANY, _L("Maximum Retention"));
    title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    title->SetFont(::Label::Body_13);
    title->SetToolTip(tooltip);
    title->Wrap(-1);

    auto       input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor input_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    input->SetBackgroundColor(input_bg);
    input->GetTextCtrl()->SetValue(app_config->get(param));
    wxTextValidator validator(wxFILTER_DIGITS);
    input->GetTextCtrl()->SetValidator(validator);

    auto second_title = new wxStaticText(parent, wxID_ANY, _L("files"), wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    second_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    second_title->SetFont(::Label::Body_13);
    second_title->SetToolTip(tooltip);
    second_title->Wrap(-1);

    sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    sizer->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    sizer->Add(input, 0, wxALIGN_CENTER_VERTICAL, 0);
    sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 3);
    sizer->Add(second_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);

    std::function<void()> commit = [this, param, input]() {
        long value = 100;
        if (!input->GetTextCtrl()->GetValue().ToLong(&value)) value = 100;
        value = std::max(1L, std::min(10000L, value));
        input->GetTextCtrl()->SetValue(wxString::Format("%ld", value));
        app_config->set(param, std::to_string(value));
        app_config->save();
    };
    input->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [commit](wxCommandEvent &e) { commit(); e.Skip(); });
    input->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [commit](wxFocusEvent &e) { commit(); e.Skip(); });

    input->Enable(app_config->get_bool("ultra_gcode_archive"));
    input->Refresh();
    m_gcode_archive_max_textinput = input;
    return sizer;
}

// Tool panels on the 3D view (Move/Rotate/Scale, paint palettes, Text, ...): background
// opacity, 30-100 %. Stored in AppConfig as "gizmo_panel_opacity" (0.3-1.0); read every
// frame by GLGizmoBase::gizmo_panel_opacity(), so no restart is needed.
//
// Slider + a manual numeric entry kept in sync both ways (owner request, alongside PR #85's
// glow-strength slider getting the same treatment): the entry commits on Enter/focus-loss and
// clamps to the slider's 30-100 range. Kept self-contained rather than sharing a helper with
// PR #85's create_item_selection_highlight() to avoid a merge dependency between the two
// branches; worth a shared "slider with numeric entry" helper once both land.
wxBoxSizer *PreferencesDialog::create_item_gizmo_panel_opacity(wxWindow *parent, wxString tooltip)
{
    const std::string param = "gizmo_panel_opacity";
    const int         min_percent = 30;
    const int         max_percent = 100;
    wxBoxSizer *sizer = new wxBoxSizer(wxHORIZONTAL);
    auto        title = new wxStaticText(parent, wxID_ANY, _L("Gizmo panel opacity"));
    title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    title->SetFont(::Label::Body_13);
    title->SetToolTip(tooltip);
    title->Wrap(-1);

    float opacity = 1.0f;
    try {
        opacity = std::clamp(std::stof(app_config->get(param)), 0.3f, 1.0f);
    } catch (...) {
        opacity = 1.0f;
    }
    const int start_percent = int(opacity * 100.0f + 0.5f);

    auto slider = new wxSlider(parent, wxID_ANY, start_percent, min_percent, max_percent,
                               wxDefaultPosition, FromDIP(wxSize(180, -1)), wxSL_HORIZONTAL);
    slider->SetToolTip(tooltip);

    auto entry = new ::TextInput(parent, wxString::Format("%d", start_percent), "%", wxEmptyString,
                                 wxDefaultPosition, DESIGN_INPUT_SIZE, wxTE_PROCESS_ENTER);
    StateColor entry_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled), std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    entry->SetBackgroundColor(entry_bg);
    entry->GetTextCtrl()->SetValue(wxString::Format("%d", start_percent));
    wxTextValidator entry_validator(wxFILTER_DIGITS);
    entry->GetTextCtrl()->SetValidator(entry_validator);
    entry->SetToolTip(tooltip);

    sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    sizer->Add(title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    sizer->Add(slider, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    sizer->Add(entry, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);

    // Applies the (already clamped) percent everywhere but back into whichever control just
    // produced it: AppConfig, canvas refresh, and the other control's displayed value.
    auto apply_percent = [this, param](int percent) {
        app_config->set(param, wxString::Format("%.2f", percent / 100.0).ToStdString());
        app_config->save();
        if (wxGetApp().plater() != nullptr)
            wxGetApp().plater()->get_current_canvas3D()->set_as_dirty();
    };

    slider->Bind(wxEVT_SLIDER, [slider, entry, apply_percent](wxCommandEvent &e) {
        const int percent = slider->GetValue();
        entry->GetTextCtrl()->ChangeValue(wxString::Format("%d", percent));
        apply_percent(percent);
        e.Skip();
    });

    auto commit_entry = [slider, entry, apply_percent, min_percent, max_percent]() {
        long value = 0;
        int  percent;
        if (entry->GetTextCtrl()->GetValue().ToLong(&value))
            percent = std::clamp(int(value), min_percent, max_percent);
        else
            percent = slider->GetValue();
        entry->GetTextCtrl()->ChangeValue(wxString::Format("%d", percent));
        slider->SetValue(percent);
        apply_percent(percent);
    };

    wxTextCtrl *entry_ctrl = entry->GetTextCtrl();
    entry_ctrl->Bind(wxEVT_TEXT_ENTER, [commit_entry, entry_ctrl](wxCommandEvent &e) {
        commit_entry();
        entry_ctrl->SelectAll();
        e.Skip();
    });
    entry_ctrl->Bind(wxEVT_KILL_FOCUS, [commit_entry](wxFocusEvent &e) {
        commit_entry();
        e.Skip();
    });
    // Focusing the box selects its text, so typing replaces the value instead of appending to it.
    // A click on the box's frame (outside the inner edit) also focuses the edit.
    entry_ctrl->Bind(wxEVT_SET_FOCUS, [entry_ctrl](wxFocusEvent &e) {
        entry_ctrl->CallAfter([entry_ctrl]() { entry_ctrl->SelectAll(); });
        e.Skip();
    });
    entry->Bind(wxEVT_LEFT_DOWN, [entry_ctrl](wxMouseEvent &e) {
        entry_ctrl->SetFocus();
        e.Skip();
    });

    return sizer;
}

// Selected-object highlight. "selection_highlight_style": "glow" (default: white/yellow edge with
// a soft halo that stays off neighbouring parts) or "thin" (a crisp 1-2 px outline, no halo).
// "selection_glow_strength": 0-200 % of the default halo. Both are read by GLCanvas3D every
// highlighted frame, so redrawing the 3D views is all a change needs.
wxBoxSizer *PreferencesDialog::create_item_selection_highlight(wxWindow *parent)
{
    const std::string style_param    = "selection_highlight_style";
    const std::string strength_param = "selection_glow_strength";

    auto redraw_3d_views = []() {
        Plater *plater = wxGetApp().plater();
        if (plater == nullptr)
            return;
        for (GLCanvas3D *canvas : { plater->get_view3D_canvas3D(), plater->get_assmeble_canvas3D() }) {
            if (canvas != nullptr) {
                canvas->set_as_dirty();
                canvas->request_extra_frame();
            }
        }
    };

    // Rows built here rather than with create_item_combobox_base(): its fixed 100 DIP title
    // column cuts "Selection highlight" off.
    auto make_title = [parent](const wxString &text, const wxString &tooltip) {
        auto title = new wxStaticText(parent, wxID_ANY, text);
        title->SetForegroundColour(DESIGN_GRAY900_COLOR);
        title->SetFont(::Label::Body_13);
        title->SetToolTip(tooltip);
        title->Wrap(-1);
        return title;
    };

    const bool thin = app_config->get(style_param) == "thin";
    const wxString style_tooltip = _L("How selected objects and parts are outlined in the 3D view.\nGlow: a white edge (yellow in the assembly view) with a soft halo over the background; the halo never covers neighbouring parts.\nThin outline: a crisp thin line around the selection, no halo - best for assemblies with touching parts.");
    wxBoxSizer *style_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto style_combo = new ::ComboBox(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, DESIGN_LARGE_COMBOBOX_SIZE, 0, nullptr, wxCB_READONLY);
    style_combo->SetFont(::Label::Body_13);
    style_combo->GetDropDown().SetFont(::Label::Body_13);
    style_combo->Append(_L("Glow"));
    style_combo->Append(_L("Thin outline"));
    style_combo->SetSelection(thin ? 1 : 0);
    style_combo->SetToolTip(style_tooltip);
    style_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    style_sizer->Add(make_title(_L("Selection highlight"), style_tooltip), 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    style_sizer->Add(style_combo, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(8));

    const wxString strength_tooltip = _L("Strength of the Glow halo around the selection, in percent of the default. 0 % leaves only the edge line. Only used by the Glow style.");
    float strength = 100.0f;
    try {
        strength = std::clamp(std::stof(app_config->get(strength_param)), 0.0f, 200.0f);
    } catch (...) {
        strength = 100.0f;
    }

    wxBoxSizer *strength_sizer = new wxBoxSizer(wxHORIZONTAL);
    auto        strength_title = make_title(_L("Selection glow strength"), strength_tooltip);

    auto slider = new wxSlider(parent, wxID_ANY, int(strength + 0.5f), 0, 200, wxDefaultPosition, FromDIP(wxSize(180, -1)), wxSL_HORIZONTAL);
    slider->SetToolTip(strength_tooltip);
    // Exact value box next to the slider (same TextInput as "Orbit speed multiplier"): whole
    // percent 0-200, committed on Enter or when the box loses focus, kept in sync with the slider.
    auto value_input = new ::TextInput(parent, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition,
                                       wxSize(FromDIP(72), -1), wxTE_PROCESS_ENTER);
    StateColor value_bg(std::pair<wxColour, int>(wxColour("#F0F0F1"), StateColor::Disabled),
                        std::pair<wxColour, int>(*wxWHITE, StateColor::Enabled));
    value_input->SetBackgroundColor(value_bg);
    wxTextCtrl *value_ctrl = value_input->GetTextCtrl();
    value_ctrl->SetValue(wxString::Format("%d", slider->GetValue()));
    value_ctrl->SetValidator(wxTextValidator(wxFILTER_DIGITS));
    value_input->SetToolTip(strength_tooltip);
    auto percent_label = make_title("%", strength_tooltip);

    strength_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    strength_sizer->Add(strength_title, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    strength_sizer->Add(slider, 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(8));
    strength_sizer->Add(value_input, 0, wxALIGN_CENTER_VERTICAL, 0);
    strength_sizer->Add(percent_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 3);
    // Both stay editable in Thin mode too (the value is kept for when Glow is picked again): a
    // greyed-out box read as "the box does not take input".

    // Saves a strength (already clamped) and redraws; shared by the slider and the value box.
    auto apply_strength = [this, strength_param, redraw_3d_views](int percent) {
        app_config->set(strength_param, std::to_string(percent));
        app_config->save();
        redraw_3d_views();
    };

    // Bound on the combo itself: it re-sends both a drop-down pick and an arrow-key change there.
    style_combo->Bind(wxEVT_COMBOBOX, [this, style_param, style_combo, redraw_3d_views](wxCommandEvent &e) {
        const bool thin_selected = style_combo->GetSelection() == 1;
        app_config->set(style_param, thin_selected ? "thin" : "glow");
        app_config->save();
        redraw_3d_views();
        e.Skip();
    });

    // Soft detent at the default: a move that lands within 3 % of 100 snaps to it, unless it
    // starts from 100 (so arrow keys can still step to 99 or 101).
    auto last_percent = std::make_shared<int>(slider->GetValue());
    slider->Bind(wxEVT_SLIDER, [slider, value_ctrl, apply_strength, last_percent](wxCommandEvent &e) {
        int percent = slider->GetValue();
        if (percent != 100 && *last_percent != 100 && std::abs(percent - 100) <= 3) {
            percent = 100;
            slider->SetValue(percent);
        }
        *last_percent = percent;
        value_ctrl->ChangeValue(wxString::Format("%d", percent)); // ChangeValue: no wxEVT_TEXT
        apply_strength(percent);
        e.Skip();
    });

    // Typing applies live: every in-range entry moves the slider and redraws at once, so the box
    // visibly "takes" the value; out-of-range or empty text waits for the commit below.
    value_ctrl->Bind(wxEVT_TEXT, [slider, value_ctrl, apply_strength, last_percent](wxCommandEvent &e) {
        long typed = 0;
        if (value_ctrl->GetValue().ToLong(&typed) && typed >= 0 && typed <= 200 && int(typed) != slider->GetValue()) {
            slider->SetValue(int(typed));
            *last_percent = int(typed);
            apply_strength(int(typed));
        }
        e.Skip();
    });

    // Commit on Enter or focus loss: digits only; empty or unparsable falls back to the slider's
    // value, anything else is clamped to 0-200 and written back so the box shows what is applied.
    auto commit_typed = [slider, value_ctrl, apply_strength, last_percent]() {
        long typed = slider->GetValue();
        if (!value_ctrl->GetValue().ToLong(&typed))
            typed = slider->GetValue();
        const int percent = int(std::clamp(typed, 0L, 200L));
        value_ctrl->ChangeValue(wxString::Format("%d", percent));
        if (percent != slider->GetValue()) {
            slider->SetValue(percent);
            *last_percent = percent;
            apply_strength(percent);
        }
    };
    value_ctrl->Bind(wxEVT_TEXT_ENTER, [commit_typed, value_ctrl](wxCommandEvent &e) {
        commit_typed();
        value_ctrl->SelectAll();
        e.Skip();
    });
    value_ctrl->Bind(wxEVT_KILL_FOCUS, [commit_typed](wxFocusEvent &e) { commit_typed(); e.Skip(); });
    // Focusing the box selects its text, so typing replaces "100" instead of appending to it
    // ("100" + "80" = "10080", clamped to 200, looked like the typing was ignored). A click on
    // the box's frame (outside the inner edit) also focuses the edit.
    value_ctrl->Bind(wxEVT_SET_FOCUS, [value_ctrl](wxFocusEvent &e) {
        value_ctrl->CallAfter([value_ctrl]() { value_ctrl->SelectAll(); });
        e.Skip();
    });
    value_input->Bind(wxEVT_LEFT_DOWN, [value_ctrl](wxMouseEvent &e) {
        value_ctrl->SetFocus();
        e.Skip();
    });

    wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(style_sizer, 0, 0, 0);
    sizer->Add(strength_sizer, 0, wxTOP, FromDIP(3));
    return sizer;
}


wxBoxSizer *PreferencesDialog::create_item_switch(wxString title, wxWindow *parent, wxString tooltip ,std::string param)
{
    wxBoxSizer *m_sizer_switch = new wxBoxSizer(wxHORIZONTAL);
    auto switch_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, DESIGN_TITLE_SIZE, 0);
    switch_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    switch_title->SetFont(::Label::Body_13);
    switch_title->SetToolTip(tooltip);
    switch_title->Wrap(-1);
    auto switchbox = new ::SwitchButton(parent, wxID_ANY);

    /*auto index = app_config->get(param);
    if (!index.empty()) { combobox->SetSelection(atoi(index.c_str())); }*/

    m_sizer_switch->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    m_sizer_switch->Add(switch_title, 0, wxALIGN_CENTER | wxALL, 3);
    m_sizer_switch->Add( 0, 0, 1, wxEXPAND, 0 );
    m_sizer_switch->Add(switchbox, 0, wxALIGN_CENTER, 0);
    m_sizer_switch->Add( 0, 0, 0, wxEXPAND|wxLEFT, 40 );

    //// save config
    switchbox->Bind(wxEVT_TOGGLEBUTTON, [this, param](wxCommandEvent &e) {
        /* app_config->set(param, std::to_string(e.GetSelection()));
         app_config->save();*/
         e.Skip();
    });
    return m_sizer_switch;
}

wxBoxSizer* PreferencesDialog::create_item_darkmode_checkbox(wxString title, wxWindow* parent, wxString tooltip, int padding_left, std::string param)
{
    wxBoxSizer* m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

    auto checkbox = new ::CheckBox(parent);
    checkbox->SetValue((app_config->get(param) == "1") ? true : false);
    m_dark_mode_ckeckbox = checkbox;

    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, 0);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_13);

    auto size = checkbox_title->GetTextExtent(title);
    checkbox_title->SetMinSize(wxSize(size.x + FromDIP(40), -1));
    checkbox_title->Wrap(-1);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);


    //// save config
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param](wxCommandEvent& e) {
        app_config->set(param, checkbox->GetValue() ? "1" : "0");
        app_config->save();
        wxGetApp().Update_dark_mode_flag();

        //dark mode
#ifdef _MSW_DARK_MODE
        wxGetApp().force_colors_update();
        wxGetApp().update_ui_from_settings();
        set_dark_mode();
#endif
        SimpleEvent evt = SimpleEvent(EVT_GLCANVAS_COLOR_MODE_CHANGED);
        wxPostEvent(wxGetApp().plater(), evt);
        e.Skip();
        });

    checkbox->SetToolTip(tooltip);
    return m_sizer_checkbox;
}

void PreferencesDialog::set_dark_mode()
{
#ifdef __WINDOWS__
#ifdef _MSW_DARK_MODE
    NppDarkMode::SetDarkExplorerTheme(this->GetHWND());
    NppDarkMode::SetDarkTitleBar(this->GetHWND());
    wxGetApp().UpdateDlgDarkUI(this);
    SetActiveWindow(wxGetApp().mainframe->GetHWND());
    SetActiveWindow(GetHWND());
#endif
#endif
}

wxBoxSizer *PreferencesDialog::create_item_checkbox(wxString title, wxWindow *parent, wxString tooltip, int padding_left, std::string param,
                                                  std::function<bool(bool new_val, bool old_val)> confirm_cb)
{
    wxBoxSizer *m_sizer_checkbox  = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

    auto checkbox = new ::CheckBox(parent);
    checkbox->SetValue(app_config->get_bool(param));

    m_sizer_checkbox->Add(checkbox, 0, wxALIGN_CENTER, 0);
    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, 0);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_13);

    auto size = checkbox_title->GetTextExtent(title);
    checkbox_title->SetMinSize(wxSize(size.x + FromDIP(5), -1));
    checkbox_title->Wrap(-1);
    m_sizer_checkbox->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);


     //// save config 
    auto reentry_guard = std::make_shared<bool>(false);
    checkbox->Bind(wxEVT_TOGGLEBUTTON, [this, checkbox, param, confirm_cb, reentry_guard](wxCommandEvent &e) {
        if (*reentry_guard)
        {
            e.Skip();
            return;
        }
        if (confirm_cb) {
            bool old_val = !checkbox->GetValue();
            bool new_val = checkbox->GetValue();
            bool final_val = confirm_cb(new_val, old_val);
            if (final_val != new_val) {
                *reentry_guard = true;
                checkbox->SetValue(final_val);
                *reentry_guard = false;
            }
        }
        app_config->set_bool(param, checkbox->GetValue());
        app_config->save();

        if (param == "allow_filament_temp_mixing" && wxGetApp().plater())
            wxGetApp().plater()->notify_filament_usage_changed();

        // Opt-in crash reports: takes effect at once, through Sentry's consent switch.
        if (param == "send_crash_reports") {
            setSentryUserConsent(checkbox->GetValue());
            BOOST_LOG_TRIVIAL(warning) << "crash reports switched " << (checkbox->GetValue() ? "on" : "off") << " in Preferences";
        }

        if (param == PRIVACY_POLICY_FLAGS)
            {
            app_config->set("app", PRIVACY_POLICY_FLAGS, checkbox->GetValue());
                BOOST_LOG_TRIVIAL(warning) <<"create_item_checkbox changed the privacy policy with: "<<(checkbox->GetValue()?"true" : "false");
                wxGetApp().user_update_privacy_notify(checkbox->GetValue());
            }
        // if (param == "staff_pick_switch") {
        //     bool pbool = app_config->get("staff_pick_switch") == "true";
        //     wxGetApp().switch_staff_pick(pbool);
        // }

         // backup
        if (param == "backup_switch") {
            bool pbool = app_config->get("backup_switch") == "true" ? true : false;
            std::string backup_interval = "10";
            app_config->get("backup_interval", backup_interval);
            Slic3r::set_backup_interval(pbool ? boost::lexical_cast<long>(backup_interval) : 0);
            if (m_backup_interval_textinput != nullptr) { m_backup_interval_textinput->Enable(pbool); }
        }

        // auto-save
        if (param == "autosave_switch") {
            bool pbool = app_config->get("autosave_switch") == "true" ? true : false;
            wxGetApp().mainframe->update_autosave_timer();
            if (m_autosave_interval_textinput != nullptr) { m_autosave_interval_textinput->Enable(pbool); }
        }

        // Print-by-object advisory notices: take effect immediately rather than waiting for the
        // next plate switch / slice attempt to re-evaluate.
        if (param == "show_print_by_object_caution" && wxGetApp().plater()) {
            if (checkbox->GetValue())
                wxGetApp().plater()->sync_print_seq_warning_notification();
            else
                wxGetApp().plater()->get_notification_manager()->bbl_close_seqprintinfo_notification();
        }

        // Ultra: the archive's folder and retention only mean anything while it is on.
        if (param == "ultra_gcode_archive") {
            bool pbool = checkbox->GetValue();
            if (m_gcode_archive_dir_item != nullptr) { m_gcode_archive_dir_item->Enable(pbool); }
            if (m_gcode_archive_max_textinput != nullptr) { m_gcode_archive_max_textinput->Enable(pbool); }
        }

        if (param == "sync_user_preset") {
            bool sync = app_config->get("sync_user_preset") == "true" ? true : false;
            if (sync) {
                wxGetApp().start_sync_user_preset();
            } else {
                wxGetApp().stop_sync_user_preset();
            }
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " sync_user_preset: " << (sync ? "true" : "false");
        }

        #ifdef __WXMSW__
        if (param == "associate_3mf") {
             bool pbool = app_config->get("associate_3mf") == "true" ? true : false;
             if (pbool) {
                 wxGetApp().associate_files(L"3mf");
             } else {
                 wxGetApp().disassociate_files(L"3mf");
             }
        }

        if (param == "associate_stl") {
            bool pbool = app_config->get("associate_stl") == "true" ? true : false;
            if (pbool) {
                wxGetApp().associate_files(L"stl");
            } else {
                wxGetApp().disassociate_files(L"stl");
            }
        }

        if (param == "associate_step") {
            bool pbool = app_config->get("associate_step") == "true" ? true : false;
            if (pbool) {
                wxGetApp().associate_files(L"step");
            } else {
                wxGetApp().disassociate_files(L"step");
            }
        }

        if (param == "installed_networking") {
            bool pbool = app_config->get_bool("installed_networking");
            // Ultra (plug-in guards): with UltraNet installed there is nothing to download - the
            // plug-in is already there, and this offer would replace it with Bambu's package.
            if (pbool && GUI::wxGetApp().is_ultranet_plugin_installed()) {
                BOOST_LOG_TRIVIAL(info) << "[UltraNet] UltraNet present, Bambu CDN download disabled (Preferences checkbox offer suppressed)";
            } else if (pbool) {
                GUI::wxGetApp().CallAfter([] { GUI::wxGetApp().ShowDownNetPluginDlg(); });
            }
            if (m_legacy_networking_ckeckbox != nullptr) { m_legacy_networking_ckeckbox->Enable(pbool); }
        }

#endif // __WXMSW__

        if (param == "developer_mode") {
            m_developer_mode_def = app_config->get("developer_mode");
            if (m_developer_mode_def == "true") {
                Slic3r::GUI::wxGetApp().save_mode(comDevelop);
            } else {
                Slic3r::GUI::wxGetApp().save_mode(comAdvanced);
            }
        }

        // webview  dump_vedio
        if (param == "internal_developer_mode") {
            m_internal_developer_mode_def = app_config->get("internal_developer_mode");
            if (m_internal_developer_mode_def == "true") {
                Slic3r::GUI::wxGetApp().update_internal_development();
                Slic3r::GUI::wxGetApp().mainframe->show_log_window();
            } else {
                Slic3r::GUI::wxGetApp().update_internal_development();
            }
        }

        e.Skip();
    });

    //// for debug mode
    if (param == "developer_mode") { m_developer_mode_ckeckbox = checkbox; }
    if (param == "internal_developer_mode") { m_internal_developer_mode_ckeckbox = checkbox; }

    if (param == "legacy_networking") {
        m_legacy_networking_ckeckbox = checkbox;
        bool pbool = app_config->get_bool("installed_networking");
        checkbox->Enable(pbool);
    }


    checkbox->SetToolTip(tooltip);
    return m_sizer_checkbox;
}

wxBoxSizer* PreferencesDialog::create_item_button(
    wxString title, wxString title2, wxWindow* parent, wxString tooltip, wxString tooltip2, std::function<void()> onclick, bool button_on_left/* = false*/)
{
    wxBoxSizer *m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    auto m_staticTextPath = new wxStaticText(parent, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    m_staticTextPath->SetMaxSize(wxSize(FromDIP(240), -1));
    m_staticTextPath->SetForegroundColour(DESIGN_GRAY900_COLOR);
    m_staticTextPath->SetFont(::Label::Body_13);
    m_staticTextPath->Wrap(-1);
    m_staticTextPath->SetToolTip(tooltip);

    auto m_button_download = new Button(parent, title2);
    m_button_download->SetStyle(ButtonStyle::Regular, ButtonType::Window);
    m_button_download->SetToolTip(tooltip2);

    m_button_download->Bind(wxEVT_BUTTON, [this, onclick](auto &e) { onclick(); });

    if (button_on_left) {
        m_sizer_checkbox->Add(m_button_download, 0, wxALL, FromDIP(5));
        m_sizer_checkbox->Add(m_staticTextPath, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    } else {
        m_sizer_checkbox->Add(m_staticTextPath, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
        m_sizer_checkbox->Add(m_button_download, 0, wxALL, FromDIP(5));
    }

    return m_sizer_checkbox;
}

wxWindow* PreferencesDialog::create_item_downloads(wxWindow* parent, int padding_left, std::string param)
{
    wxString download_path = wxString::FromUTF8(app_config->get("download_path"));
    auto item_panel = new wxWindow(parent, wxID_ANY);
    item_panel->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* m_sizer_checkbox = new wxBoxSizer(wxHORIZONTAL);

    m_sizer_checkbox->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);
    auto m_staticTextPath = new wxStaticText(item_panel, wxID_ANY, download_path, wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    //m_staticTextPath->SetMaxSize(wxSize(FromDIP(440), -1));
    m_staticTextPath->SetForegroundColour(DESIGN_GRAY600_COLOR);
    m_staticTextPath->SetFont(::Label::Body_13);
    m_staticTextPath->Wrap(-1);

    auto m_button_download = new Button(item_panel, _L("Browse"));
    m_button_download->SetStyle(ButtonStyle::Regular, ButtonType::Window);

    m_button_download->Bind(wxEVT_BUTTON, [this, m_staticTextPath, item_panel](auto& e) {
        wxString defaultPath = wxT("/");
        wxDirDialog dialog(this, _L("Choose Download Directory"), defaultPath, wxDD_NEW_DIR_BUTTON);

        if (dialog.ShowModal() == wxID_OK) {
            wxString download_path = dialog.GetPath();
            std::string download_path_str = download_path.ToUTF8().data();
            app_config->set("download_path", download_path_str);
            m_staticTextPath->SetLabelText(download_path);
            item_panel->Layout();
        }
        });

    m_sizer_checkbox->Add(m_staticTextPath, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP(5));
    m_sizer_checkbox->Add(m_button_download, 0, wxALL, FromDIP(5));

    item_panel->SetSizer(m_sizer_checkbox);
    item_panel->Layout();

    return item_panel;
}

wxWindow *PreferencesDialog ::create_item_radiobox(wxString title, wxWindow *parent, wxString tooltip, int padding_left, int groupid, std::string param)
{
    wxWindow *item = new wxWindow(parent, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(28)));
    item->SetBackgroundColour(*wxWHITE);

    RadioBox *radiobox = new RadioBox(item);
    radiobox->SetPosition(wxPoint(padding_left, (item->GetSize().GetHeight() - radiobox->GetSize().GetHeight()) / 2));
    radiobox->Bind(wxEVT_LEFT_DOWN, &PreferencesDialog::OnSelectRadio, this);

    RadioSelector *rs = new RadioSelector;
    rs->m_groupid     = groupid;
    rs->m_param_name  = param;
    rs->m_radiobox    = radiobox;
    rs->m_selected    = false;
    m_radio_group.Append(rs);

    wxStaticText *text = new wxStaticText(item, wxID_ANY, title, wxDefaultPosition, wxDefaultSize);
    text->SetPosition(wxPoint(padding_left + radiobox->GetSize().GetWidth() + 10, (item->GetSize().GetHeight() - text->GetSize().GetHeight()) / 2));

    radiobox->SetToolTip(tooltip);
    text->SetToolTip(tooltip);
    return item;
}

#ifdef WIN32
wxBoxSizer* PreferencesDialog::create_item_link_association(wxWindow* parent, wxString url_prefix, wxString website_name)
{
    wxString title = _L("Associate") + (boost::format(" %1%://") % url_prefix.c_str()).str();
    wxString tooltip = _L("Associate") + " " + url_prefix + ":// " + _L("with EdgeSlicer so that Orca can open models from") + " " + website_name;

    std::wstring registered_bin; // not used, just here to provide a ref to check fn
    bool reg_to_current_instance = wxGetApp().check_url_association(url_prefix.ToStdWstring(), registered_bin);

    auto* h_sizer = new wxBoxSizer(wxHORIZONTAL); // contains checkbox and other elements on the first line
    h_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 23);

    // build checkbox
    auto checkbox = new ::CheckBox(parent);
    checkbox->SetToolTip(tooltip);
    checkbox->SetValue(reg_to_current_instance); // If registered to the current instance, checkbox should be checked
    checkbox->Enable(!reg_to_current_instance); // Since unregistering isn't supported, checkbox is disabled when checked

    h_sizer->Add(checkbox, 0, wxALIGN_CENTER, 0);
    h_sizer->Add(0, 0, 0, wxEXPAND | wxLEFT, 8);

    // build text next to checkbox
    auto checkbox_title = new wxStaticText(parent, wxID_ANY, title);
    checkbox_title->SetToolTip(tooltip);
    checkbox_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    checkbox_title->SetFont(::Label::Body_13);
    auto size = checkbox_title->GetTextExtent(title);
    checkbox_title->SetMinSize({ size.x + FromDIP(5), -1 });
    checkbox_title->Wrap(-1);
    h_sizer->Add(checkbox_title, 0, wxALIGN_CENTER | wxALL, 3);

    auto* v_sizer = new wxBoxSizer(wxVERTICAL);
    v_sizer->Add(h_sizer);

    // build text below checkbox that indicates the instance currently registered to handle the link type
    auto* registered_instance_title = new wxStaticText(parent, wxID_ANY, "");
    registered_instance_title->SetForegroundColour(DESIGN_GRAY900_COLOR);
    registered_instance_title->SetFont(::Label::Body_13);
    registered_instance_title->Wrap(-1);

    // update the text below checkbox
    auto update_current_association_str = [=, &reg_to_current_instance](){
        // get registered binary for given link type
        std::wstring registered_bin;
        reg_to_current_instance = wxGetApp().check_url_association(url_prefix.wc_str(), registered_bin);

        // format registered binary to get only the path and remove excess chars
        if (!registered_bin.empty())
            // skip idx 0 because it is the first quotation mark
            registered_bin = registered_bin.substr(1, registered_bin.find(L'\"', 1) - 1);

        wxString current_association_str = _L("Current Association: ");
        if (reg_to_current_instance) {
            current_association_str += _L("Current Instance");
            registered_instance_title->SetToolTip(_L("Current Instance Path: ") + registered_bin);
        } else if (registered_bin.empty())
            current_association_str += _L("None");
        else
            current_association_str += registered_bin;

        registered_instance_title->SetLabel(current_association_str);
        auto size = registered_instance_title->GetTextExtent(current_association_str);
        registered_instance_title->SetMinSize({ size.x + FromDIP(5), -1 });
    };
    update_current_association_str();

    v_sizer->Add(registered_instance_title, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 60);

    checkbox->Bind(wxEVT_TOGGLEBUTTON, [=](wxCommandEvent& e) {
        wxGetApp().associate_url(url_prefix.ToStdWstring());
        checkbox->Disable();
        update_current_association_str();
        e.Skip();
    });

    return v_sizer;
}
#endif // WIN32

PreferencesDialog::PreferencesDialog(wxWindow *parent, wxWindowID id, const wxString &title, const wxPoint &pos, const wxSize &size, long style)
    : DPIDialog(parent, id, _L("Preferences"), pos, size, style)
{
    SetBackgroundColour(*wxWHITE);
    create();
    wxGetApp().UpdateDlgDarkUI(this);
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent& event) {
        try {
            NetworkAgent* agent = GUI::wxGetApp().getAgent();
            if (agent) {
                json j;
                std::string value;
                value = wxGetApp().app_config->get("auto_calculate");
                j["auto_flushing"] = value;
                value = wxGetApp().app_config->get("auto_calculate_when_filament_change");
                j["auto_calculate_when_filament_change"] = value;
                agent->track_event("preferences_changed", j.dump());
            }
        } catch(...) {}
        event.Skip();
        });
}

void PreferencesDialog::create()
{
    app_config             = get_app_config();
    m_backup_interval_time = app_config->get("backup_interval");

    // set icon for dialog
    std::string icon_path = (boost::format("%1%/images/Snapmaker_OrcaTitle.ico") % resources_dir()).str();
    SetIcon(wxIcon(encode_path(icon_path.c_str()), wxBITMAP_TYPE_ICO));
    SetSizeHints(wxDefaultSize, wxDefaultSize);

    auto main_sizer = new wxBoxSizer(wxVERTICAL);

    m_scrolledWindow = new MyscrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scrolledWindow->SetScrollRate(5, 5);

    m_sizer_body = new wxBoxSizer(wxVERTICAL);

    auto m_top_line = new wxPanel(m_scrolledWindow, wxID_ANY, wxDefaultPosition, wxSize(DESIGN_RESOUTION_PREFERENCES.x, 1), wxTAB_TRAVERSAL);
    m_top_line->SetBackgroundColour(DESIGN_GRAY400_COLOR);

    m_sizer_body->Add(m_top_line, 0, wxEXPAND, 0);

    auto general_page = create_general_page();
    auto ultra_page   = create_ultra_page();
#if !BBL_RELEASE_TO_PUBLIC
    auto debug_page   = create_debug_page();
#endif

    // Tab bar switching between the pages (BambuStudio-style tabbed preferences).
    std::vector<std::pair<wxString, wxWindow*>> pages = { { _L("General"), general_page }, { _L("Extras"), ultra_page } };
#if !BBL_RELEASE_TO_PUBLIC
    pages.emplace_back(_L("Develop"), debug_page);
#endif
    auto* tab_bar = new wxBoxSizer(wxHORIZONTAL);
    auto* tab_buttons = new std::vector<Button*>();
    for (size_t i = 0; i < pages.size(); ++i) {
        auto* btn = new Button(m_scrolledWindow, pages[i].first);
        btn->SetStyle(i == 0 ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Compact);
        tab_buttons->push_back(btn);
        tab_bar->Add(btn, 0, wxRIGHT, FromDIP(8));
        wxWindow* page = pages[i].second;
        page->Show(i == 0);
        btn->Bind(wxEVT_BUTTON, [this, i, pages, tab_buttons](wxCommandEvent&) {
            for (size_t j = 0; j < pages.size(); ++j) {
                pages[j].second->Show(j == i);
                (*tab_buttons)[j]->SetStyle(j == i ? ButtonStyle::Confirm : ButtonStyle::Regular, ButtonType::Compact);
            }
            m_scrolledWindow->Layout();
            m_sizer_body->Layout();
            Layout();
            Refresh();
        });
    }

    m_sizer_body->Add(0, 0, 0, wxTOP, FromDIP(14));
    m_sizer_body->Add(tab_bar, 0, wxLEFT | wxRIGHT, FromDIP(38));
    m_sizer_body->Add(0, 0, 0, wxTOP, FromDIP(14));
    m_sizer_body->Add(general_page, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
    m_sizer_body->Add(ultra_page, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
#if !BBL_RELEASE_TO_PUBLIC
    m_sizer_body->Add(debug_page, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(38));
#endif
    m_sizer_body->Add(0, 0, 0, wxBOTTOM, FromDIP(28));
    m_scrolledWindow->SetSizerAndFit(m_sizer_body);

    main_sizer->Add(m_scrolledWindow, 1, wxEXPAND);

    SetSizer(main_sizer);
    Layout();
    Fit();
    int screen_height = wxDisplay(m_parent).GetClientArea().GetHeight();
    if (this->GetSize().GetY() > screen_height)
        this->SetSize(this->GetSize().GetX() + FromDIP(40), screen_height * 4 / 5);

    CenterOnParent();
    wxPoint start_pos = this->GetPosition();
    if (start_pos.y < 0) { this->SetPosition(wxPoint(start_pos.x, 0)); }

    //select first
    auto event = wxCommandEvent(EVT_PREFERENCES_SELECT_TAB);
    event.SetInt(0);
    event.SetEventObject(this);
    wxPostEvent(this, event);
}

PreferencesDialog::~PreferencesDialog()
{
    m_radio_group.DeleteContents(true);
    m_hash_selector.clear();
}

void PreferencesDialog::on_dpi_changed(const wxRect &suggested_rect) { this->Refresh(); }

void PreferencesDialog::Split(const std::string &src, const std::string &separator, std::vector<wxString> &dest)
{
    std::string            str = src;
    std::string            substring;
    std::string::size_type start = 0, index;
    dest.clear();
    index = str.find_first_of(separator, start);
    do {
        if (index != std::string::npos) {
            substring = str.substr(start, index - start);
            dest.push_back(substring);
            start = index + separator.size();
            index = str.find(separator, start);
            if (start == std::string::npos) break;
        }
    } while (index != std::string::npos);

    substring = str.substr(start);
    dest.push_back(substring);
}

wxWindow* PreferencesDialog::create_general_page()
{
    auto page = new wxWindow(m_scrolledWindow, wxID_ANY);
    page->SetBackgroundColour(*wxWHITE);
    wxBoxSizer *sizer_page = new wxBoxSizer(wxVERTICAL);

    auto title_general_settings = create_item_title(_L("General Settings"), page, _L("General Settings"));

    // bbs supported languages
    wxLanguage supported_languages[]{
        wxLANGUAGE_ENGLISH,
        wxLANGUAGE_CHINESE_SIMPLIFIED,
        wxLANGUAGE_CHINESE,
        wxLANGUAGE_GERMAN,
        wxLANGUAGE_CZECH,
        wxLANGUAGE_FRENCH,
        wxLANGUAGE_SPANISH,
        wxLANGUAGE_SWEDISH,
        wxLANGUAGE_DUTCH,
        wxLANGUAGE_HUNGARIAN,
        wxLANGUAGE_JAPANESE,
        wxLANGUAGE_ITALIAN,
        wxLANGUAGE_KOREAN,
        wxLANGUAGE_RUSSIAN,
        wxLANGUAGE_UKRAINIAN,
        wxLANGUAGE_TURKISH,
        wxLANGUAGE_POLISH,
        wxLANGUAGE_CATALAN,
        wxLANGUAGE_PORTUGUESE_BRAZILIAN,
        wxLANGUAGE_LITHUANIAN,
    };

    auto translations = wxTranslations::Get()->GetAvailableTranslations(SLIC3R_APP_KEY);
    std::vector<const wxLanguageInfo *> language_infos;
    language_infos.emplace_back(wxLocale::GetLanguageInfo(wxLANGUAGE_ENGLISH));
    for (size_t i = 0; i < translations.GetCount(); ++i) {
        const wxLanguageInfo *langinfo = wxLocale::FindLanguageInfo(translations[i]);

        if (langinfo == nullptr) continue;
        int language_num = sizeof(supported_languages) / sizeof(supported_languages[0]);
        for (auto si = 0; si < language_num; si++) {
            if (langinfo == wxLocale::GetLanguageInfo(supported_languages[si])) {
                language_infos.emplace_back(langinfo);
            }
        }
        //if (langinfo != nullptr) language_infos.emplace_back(langinfo);
    }
    sort_remove_duplicates(language_infos);
    std::sort(language_infos.begin(), language_infos.end(), [](const wxLanguageInfo *l, const wxLanguageInfo *r) { return l->Description < r->Description; });
    auto item_language = create_item_language_combobox(_L("Language"), page, _L("Language"), 50, "language", language_infos);

    std::vector<wxString> Regions         = {_L("Asia-Pacific"), _L("Chinese Mainland"), _L("Europe"), _L("North America"), _L("Others")};
    auto                  item_region= create_item_region_combobox(_L("Login Region"), page, _L("Login Region"), Regions);

    // SM Beta: temporarily open the item_stealth_mode and close the network plugin

    /*auto item_stealth_mode = create_item_checkbox(_L("Stealth mode"), page, _L("This stops the transmission of data to Bambu's cloud services. Users who don't use BBL machines or use LAN mode only can safely turn on this function."), 50, "stealth_mode");
    /*auto item_stealth_mode = create_item_checkbox(_L("Stealth mode"), page, _L("This stops the transmission of data to Bambu's cloud services. Users who don't use BBL machines or use LAN mode only can safely turn on this function."), 50, "stealth_mode");
    auto item_enable_plugin = create_item_checkbox(_L("Enable network plugin"), page, _L("Enable network plugin"), 50, "installed_networking");
    auto item_legacy_network_plugin = create_item_checkbox(_L("Use legacy network plugin (Takes effect after restarting Orca)"), page, _L("Disable to use latest network plugin that supports new BambuLab firmwares."), 50, "legacy_networking");
    */
    // Ultra: the SM Beta force-off of the Bambu network plugin is lifted; both
    // switches now live on the Ultra tab (Bambu Network section) as opt-ins.


    //auto item_check_stable_version_only = create_item_checkbox(_L("Check for stable updates only"), page, _L("Check for stable updates only"), 50, "check_stable_update_only");

    std::vector<wxString> Units         = {_L("Metric") + " (mm, g)", _L("Imperial") + " (in, oz)"};
    auto item_currency = create_item_combobox(_L("Units"), page, _L("Units"), "use_inches", Units);
    auto item_single_instance = create_item_checkbox(_L("Allow only one EdgeSlicer instance"), page,
    #if __APPLE__
            _L("On OSX there is always only one instance of app running by default. However it is allowed to run multiple instances "
                "of same app from the command line. In such case this settings will allow only one instance."),
    #else
            _L("If this is enabled, when starting EdgeSlicer and another instance of the same EdgeSlicer is already running, that instance will be reactivated instead."),
    #endif
            50, "single_instance");

    std::vector<wxString> DefaultPage = {_L("Home"), _L("Prepare")};
    auto item_default_page = create_item_combobox(_L("Default Page"), page, _L("Set the page opened on startup."), "default_page", DefaultPage);

    std::vector<wxString> CameraNavStyle = {_L("Default"), _L("Touchpad")};
    auto item_camera_navigation_style = create_item_combobox(_L("Camera style"), page, _L("Select camera navigation style.\nDefault: LMB+move for rotation, RMB/MMB+move for panning.\nTouchpad: Alt+move for rotation, Shift+move for panning."), "camera_navigation_style", CameraNavStyle);

    auto item_mouse_zoom_settings = create_item_checkbox(_L("Zoom to mouse position"), page, _L("Zoom in towards the mouse pointer's position in the 3D view, rather than the 2D window center."), 50, "zoom_to_mouse");
    auto item_use_free_camera_settings = create_item_checkbox(_L("Use free camera"), page, _L("If enabled, use free camera. If not enabled, use constrained camera."), 50, "use_free_camera");
    auto swap_pan_rotate = create_item_checkbox(_L("Swap pan and rotate mouse buttons"), page, _L("If enabled, swaps the left and right mouse buttons pan and rotate functions."), 50, "swap_mouse_buttons");
    auto reverse_mouse_zoom = create_item_checkbox(_L("Reverse mouse zoom"), page, _L("If enabled, reverses the direction of zoom with mouse wheel."), 50, "reverse_mouse_wheel_zoom");
    auto allow_filament_temp_mixing = create_item_checkbox(_L("Allow high/low temperature filament mixing"), page, _L("If enabled, allows printing with both high-temperature and low-temperature filaments simultaneously."), 50, "allow_filament_temp_mixing",
        [this](bool new_val, bool old_val) -> bool {
            // Only confirm when turning ON; allow turning OFF without dialog.
            if (!new_val)
                return false;

            wxString msg = _L("Mixing materials with significantly different printing temperatures may result in:\n"
                              "· Extruder clogging\n"
                              "· Nozzle damage\n"
                              "· Layer adhesion issues\n\n"
                              "Do you want to enable this feature?");
            MessageDialog dlg(this, msg, _L("High and Low Temperature Material Mixing Risk"), wxICON_WARNING | wxOK | wxCANCEL);
            dlg.SetButtonLabel(wxID_OK, _L("Confirm"));
            dlg.SetButtonLabel(wxID_CANCEL, _L("Cancel"));
            return dlg.ShowModal() == wxID_OK;
        });
    // Was built on an unreachable "GUI" preferences page (create_gui_page(), never added to the
    // dialog's tab list) so it was inaccessible from the UI; moved here since the row itself works
    // and AppConfig["gizmo_panel_opacity"] is read live by GLGizmoBase::gizmo_panel_opacity().
    // Placed before "Orbit speed multiplier" (rather than after, alongside PR #85's own insertion
    // point) so this row and PR #85's selection-highlight rows land on disjoint lines and merge
    // cleanly in either order.
    auto item_panel_opacity = create_item_gizmo_panel_opacity(page,
        _L("Background opacity of the tool panels on the 3D view. Lower values let you see the model behind a docked panel. Applies immediately."));
    auto camera_orbit_mult = create_camera_orbit_mult_input(_L("Orbit speed multiplier"), page, _L("Multiplies the orbit speed for finer or coarser camera movement."));
    auto item_selection_highlight = create_item_selection_highlight(page);

    auto item_show_splash_screen = create_item_checkbox(_L("Show splash screen"), page, _L("Show the splash screen during startup."), 50, "show_splash_screen");
    auto item_hints = create_item_checkbox(_L("Show \"Tip of the day\" notification after start"), page, _L("If enabled, useful hints are displayed at startup."), 50, "show_hints");
    auto item_check_updates = create_item_checkbox(_L("Check for new versions on startup"), page,
        _L("If enabled, EdgeSlicer asks GitHub once per start whether a newer release has been published and offers it. "
           "Help > Check for Update works either way."),
        50, "check_for_updates_on_startup");
    std::vector<wxString>    UpdateChannels      = { _L("Stable"), _L("Nightly") };
    std::vector<std::string> UpdateChannelValues = { "stable", "nightly" };
    auto item_update_channel = create_item_combobox(_L("Update channel"), page,
        _L("Stable: offer new EdgeSlicer releases.\n"
           "Nightly: offer the nightly build, made automatically from the latest source whenever it changed. "
           "Nightly builds are untested and may be broken; switch back to Stable and install the latest release to return."),
        "update_channel", UpdateChannels, UpdateChannelValues);
    auto item_sm_auto_login = create_item_checkbox(_L("Sign in to my Snapmaker account automatically at startup"), page,
        _L("If enabled, EdgeSlicer quietly reuses the Snapmaker account session saved from your last sign-in when it starts, "
           "so you do not have to sign in again. Nothing is shown; if there is no saved session you simply stay signed out."),
        50, "snapmaker_auto_login");

    auto item_calc_mode = create_item_checkbox(_L("Flushing volumes: Auto-calculate every time the color changed."), page, _L("If enabled, auto-calculate every time the color changed."), 50, "auto_calculate");
    auto item_calc_in_long_retract = create_item_checkbox(_L("Flushing volumes: Auto-calculate every time when the filament is changed."), page, _L("If enabled, auto-calculate every time when filament is changed"), 50, "auto_calculate_when_filament_change");
    auto item_remember_printer_config = create_item_checkbox(_L("Remember printer configuration"), page, _L("If enabled, Orca will remember and switch filament/process configuration for each printer automatically."), 50, "remember_printer_config");
    auto item_step_mesh_setting = create_item_checkbox(_L("Show the step mesh parameter setting dialog."), page, _L("If enabled,a parameter settings dialog will appear during STEP file import."), 50, "enable_step_mesh_setting");
    auto item_multi_machine = create_item_checkbox(_L("Multi-device Management (Take effect after restarting EdgeSlicer)."), page, _L("With this option enabled, you can send a task to multiple devices at the same time and manage multiple devices."), 50, "enable_multi_machine");
    auto item_auto_arrange  = create_item_checkbox(_L("Auto arrange plate after cloning"), page, _L("Auto arrange plate after object cloning"), 50, "auto_arrange");
    auto title_presets = create_item_title(_L("Presets"), page, _L("Presets"));
    //auto title_network = create_item_title(_L("Network"), page, _L("Network"));
    auto item_user_sync        = create_item_checkbox(_L("Auto sync user presets (Printer/Filament/Process)"), page, _L("User Sync"), 50, "sync_user_preset");
    auto item_system_sync        = create_item_checkbox(_L("Update built-in Presets automatically."), page, _L("System Sync"), 50, "sync_system_preset");
    auto item_save_presets = create_item_button(_L("Clear my choice on the unsaved presets."), _L("Clear"), page, L"", _L("Clear my choice on the unsaved presets."), []() {
        wxGetApp().app_config->set("save_preset_choise", "");
    });

#ifdef _WIN32
    auto title_associate_file = create_item_title(_L("Associate files to EdgeSlicer"), page, _L("Associate files to EdgeSlicer"));

    // associate file
    auto item_associate_3mf  = create_item_checkbox(_L("Associate .3mf files to EdgeSlicer"), page,
                                                        _L("If enabled, sets EdgeSlicer as default application to open .3mf files"), 50, "associate_3mf");
    auto item_associate_stl  = create_item_checkbox(_L("Associate .stl files to EdgeSlicer"), page,
                                                        _L("If enabled, sets EdgeSlicer as default application to open .stl files"), 50, "associate_stl");
    auto item_associate_step = create_item_checkbox(_L("Associate .step/.stp files to EdgeSlicer"), page,
                                                         _L("If enabled, sets EdgeSlicer as default application to open .step files"), 50, "associate_step");

    auto title_associate_url = create_item_title(_L("Associate web links to EdgeSlicer"), page, _L("Associate URLs to EdgeSlicer"));

    auto associate_url_orcaslicer  = create_item_link_association(page, L"orcaslicer", "Printables.com (Orca links)");
    auto associate_url_prusaslicer = create_item_link_association(page, L"prusaslicer", "Printables.com");
    auto associate_url_bambustudio = create_item_link_association(page, L"bambustudio", "Makerworld.com");
    auto associate_url_cura        = create_item_link_association(page, L"cura", "Thingiverse.com");
#endif // _WIN32

    // auto title_modelmall = create_item_title(_L("Online Models"), page, _L("Online Models"));
    // auto item_backup = create_item_switch(_L("Backup switch"), page, _L("Backup switch"), "units");
    // auto item_modelmall = create_item_checkbox(_L("Show online staff-picked models on the home page"), page, _L("Show online staff-picked models on the home page"), 50, "staff_pick_switch");

    auto title_project = create_item_title(_L("Project"), page, "");

    std::vector<wxString> projectLoadSettingsBehaviourOptions = {_L("Load All"), _L("Ask When Relevant"), _L("Always Ask"), _L("Load Geometry Only")};
    std::vector<string> projectLoadSettingsConfigOptions = { OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_ALL, OPTION_PROJECT_LOAD_BEHAVIOUR_ASK_WHEN_RELEVANT, OPTION_PROJECT_LOAD_BEHAVIOUR_ALWAYS_ASK, OPTION_PROJECT_LOAD_BEHAVIOUR_LOAD_GEOMETRY };
    auto item_project_load_behaviour = create_item_combobox(_L("Load Behaviour"), page, _L("Should printer/filament/process settings be loaded when opening a .3mf?"), SETTING_PROJECT_LOAD_BEHAVIOUR, projectLoadSettingsBehaviourOptions, projectLoadSettingsConfigOptions);

    auto item_max_recent_count = create_item_input(_L("Maximum recent files"), "", page, _L("Maximum count of recent files"), "max_recent_count", [](wxString value) {
        long max = 0;
        if (value.ToLong(&max))
            wxGetApp().mainframe->set_max_recent_count(max);
    });

    auto item_recent_models = create_item_checkbox(_L("Add model files (stl/step) to recent file list."), page, _L("Add model files (stl/step) to recent file list."), 50, "recent_models");

    auto item_save_choise = create_item_button(_L("Clear my choice on the unsaved projects."), _L("Clear"), page, L"", _L("Clear my choice on the unsaved projects."), []() {
        wxGetApp().app_config->set("save_project_choise", "");
    });
    // auto item_backup = create_item_switch(_L("Backup switch"), page, _L("Backup switch"), "units");
    auto item_gcodes_warning = create_item_checkbox(_L("No warnings when loading 3MF with modified G-code"), page, _L("No warnings when loading 3MF with modified G-code"), 50, "no_warn_when_modified_gcodes");
    auto item_backup  = create_item_checkbox(_L("Auto-Backup"), page,_L("Backup your project periodically for restoring from the occasional crash."), 50, "backup_switch");
    auto item_backup_interval = create_item_backup_input(_L("every"), page, _L("The period of backup in seconds."), "backup_interval");

    //downloads
    auto title_downloads = create_item_title(_L("Downloads"), page, _L("Downloads"));
    auto item_downloads = create_item_downloads(page,50,"download_path");

    // "Export & Open in Bambu Studio" looks for an installed Bambu Studio automatically
    // (registry / file association / default path); this lets a user whose install it cannot
    // find point at bambu-studio.exe directly. Left blank, discovery runs as usual.
    auto title_bambu_studio = create_item_title(_L("Bambu Studio"), page, _L("Bambu Studio"));
    auto item_bambu_studio_path = create_item_text_input(_L("Bambu Studio path"), page,
        _L("Only needed if \"Export & Open in Bambu Studio\" cannot find your Bambu Studio installation automatically. "
           "Full path to bambu-studio.exe (Windows), the BambuStudio.app bundle (macOS), or the executable (Linux)."),
        "bambu_studio_path");

    //dark mode
#ifdef _WIN32
    auto title_darkmode = create_item_title(_L("Dark Mode"), page, _L("Dark Mode"));
    auto item_darkmode = create_item_darkmode_checkbox(_L("Enable Dark mode"), page,_L("Enable dark mode"), 50, "dark_color_mode");
#endif

    // The "User Experience" section ("Join Customer Experience Improvement Program", linking to
    // Snapmaker's privacy policy) is gone: EdgeSlicer runs no such program. What replaced it is
    // the one thing EdgeSlicer can send, opt-in crash reports (docs/privacy.md).
    auto title_privacy = create_item_title(_L("Privacy"), page, _L("Privacy"));
    const wxString crash_reports_what =
        _L("When EdgeSlicer crashes, send a crash report to its developers (via Sentry): the crash stack trace, "
           "the app version, your operating system and the last warning and error lines of the app log. IP addresses, "
           "host names, printer serial numbers, access codes, passwords, tokens, e-mail addresses and the user name in "
           "file paths are removed from the log lines first. Nothing is sent while this is off, and EdgeSlicer sends no "
           "usage statistics either way.");
    auto item_crash_reports = create_item_checkbox(_L("Send crash reports"), page, crash_reports_what, 50, "send_crash_reports");
    auto crash_reports_link = new wxHyperlinkCtrl(page, wxID_ANY, _L("What is sent (privacy note)"),
                                                  "https://github.com/aceRage/EdgeSlicer/blob/main/docs/privacy.md");
    crash_reports_link->SetFont(Label::Head_13);
    item_crash_reports->Add(crash_reports_link, 0, wxALIGN_CENTER | wxLEFT, FromDIP(10));
    auto crash_reports_text = new wxStaticText(page, wxID_ANY, crash_reports_what);
    crash_reports_text->SetForegroundColour(DESIGN_GRAY600_COLOR);
    crash_reports_text->SetFont(::Label::Body_12);
    crash_reports_text->Wrap(FromDIP(560));
    if (!sentryCrashReportsAvailable()) {
        // A local or fork build has no destination: say so rather than offer a switch that
        // could not send anything.
        if (wxSizerItem* cb = item_crash_reports->GetItem(size_t(1)); cb != nullptr && cb->GetWindow() != nullptr)
            cb->GetWindow()->Disable();
        crash_reports_text->SetLabel(crash_reports_what + "\n" +
                                     _L("This build was made without a crash-report destination, so it cannot send any."));
        crash_reports_text->Wrap(FromDIP(560));
    }

    auto title_develop_mode = create_item_title(_L("Develop mode"), page, _L("Develop mode"));
    auto item_develop_mode  = create_item_checkbox(_L("Develop mode"), page, _L("Develop mode"), 50, "developer_mode");
    auto item_skip_ams_blacklist_check  = create_item_checkbox(_L("Skip AMS blacklist check"), page, _L("Skip AMS blacklist check"), 50, "skip_ams_blacklist_check");

    sizer_page->Add(title_general_settings, 0, wxEXPAND, 0);
    sizer_page->Add(item_language, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_region, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_currency, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_default_page, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_camera_navigation_style, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_single_instance, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_mouse_zoom_settings, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_use_free_camera_settings, 0, wxTOP, FromDIP(3));
    sizer_page->Add(swap_pan_rotate, 0, wxTOP, FromDIP(3));
    sizer_page->Add(reverse_mouse_zoom, 0, wxTOP, FromDIP(3));
    sizer_page->Add(allow_filament_temp_mixing, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_panel_opacity, 0, wxTOP, FromDIP(3));
    sizer_page->Add(camera_orbit_mult, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_selection_highlight, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_show_splash_screen, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_hints, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_check_updates, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_update_channel, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_sm_auto_login, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_calc_in_long_retract, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_multi_machine, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_step_mesh_setting, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_auto_arrange, 0, wxTOP, FromDIP(3));
    sizer_page->Add(title_presets, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_calc_mode, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_user_sync, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_system_sync, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_remember_printer_config, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_save_presets, 0, wxTOP, FromDIP(3));
    //sizer_page->Add(title_network, 0, wxTOP | wxEXPAND, FromDIP(20));
    //sizer_page->Add(item_check_stable_version_only, 0, wxTOP, FromDIP(3));

    // SM Beta: temporarily open the item_stealth_mode and close the network plugin

    /*sizer_page->Add(item_stealth_mode, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_enable_plugin, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_legacy_network_plugin, 0, wxTOP, FromDIP(3));
    */
#ifdef _WIN32
    sizer_page->Add(title_associate_file, 0, wxTOP| wxEXPAND, FromDIP(20));
    sizer_page->Add(item_associate_3mf, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_associate_stl, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_associate_step, 0, wxTOP, FromDIP(3));
    sizer_page->Add(title_associate_url, 0, wxTOP| wxEXPAND, FromDIP(20));
    sizer_page->Add(associate_url_orcaslicer, 0, wxTOP, FromDIP(3));
    sizer_page->Add(associate_url_prusaslicer, 0, wxTOP, FromDIP(3));
    sizer_page->Add(associate_url_bambustudio, 0, wxTOP, FromDIP(3));
    sizer_page->Add(associate_url_cura, 0, wxTOP, FromDIP(3));
#endif // _WIN32
    // auto item_title_modelmall = sizer_page->Add(title_modelmall, 0, wxTOP | wxEXPAND, FromDIP(20));
    // auto item_item_modelmall = sizer_page->Add(item_modelmall, 0, wxTOP, FromDIP(3));
    // auto update_modelmall = [this, item_title_modelmall, item_item_modelmall] (wxEvent & e) {
    //     bool has_model_mall = wxGetApp().has_model_mall();
    //     item_title_modelmall->Show(has_model_mall);
    //     item_item_modelmall->Show(has_model_mall);
    //     Layout();
    //     Fit();
    // };
    // wxCommandEvent eee(wxEVT_COMBOBOX);
    // update_modelmall(eee);
    // item_region->GetItem(size_t(2))->GetWindow()->Bind(wxEVT_COMBOBOX, update_modelmall);
    sizer_page->Add(title_project, 0, wxTOP| wxEXPAND, FromDIP(20));
    sizer_page->Add(item_project_load_behaviour, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_max_recent_count, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_recent_models, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_save_choise, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_gcodes_warning, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_backup, 0, wxTOP,FromDIP(3));
    item_backup->Add(item_backup_interval, 0, wxLEFT, 0);

    sizer_page->Add(title_downloads, 0, wxTOP| wxEXPAND, FromDIP(20));
    sizer_page->Add(item_downloads, 0, wxEXPAND, FromDIP(3));

    sizer_page->Add(title_bambu_studio, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_bambu_studio_path, 0, wxTOP, FromDIP(3));

#ifdef _WIN32
    sizer_page->Add(title_darkmode, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_darkmode, 0, wxEXPAND, FromDIP(3));
#endif

    sizer_page->Add(title_privacy, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_crash_reports, 0, wxTOP, FromDIP(3));
    sizer_page->Add(crash_reports_text, 0, wxLEFT | wxTOP, FromDIP(50));
    sizer_page->Add(title_develop_mode, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_develop_mode, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_skip_ams_blacklist_check, 0, wxTOP, FromDIP(3));

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
    return page;
}

// Ultra: page collecting all fork-specific options in one place.
wxWindow* PreferencesDialog::create_ultra_page()
{
    auto page = new wxWindow(m_scrolledWindow, wxID_ANY);
    page->SetBackgroundColour(*wxWHITE);
    wxBoxSizer* sizer_page = new wxBoxSizer(wxVERTICAL);

    auto title_project = create_item_title(_L("Project"), page, _L("Project"));
    auto item_autosave = create_item_checkbox(_L("Auto-Save project"), page,
        _L("Periodically save the project to its own file. Only projects that have already been saved once are auto-saved."), 50, "autosave_switch");
    auto item_autosave_interval = create_item_autosave_input(_L("every"), page, _L("The period of auto-save in seconds."), "autosave_interval");
    auto item_keep_printer = create_item_checkbox(_L("Keep my printer when opening project files"), page,
        _L("When opening a project made for another printer, switch back to the last printer you selected or sliced with, instead of the printer embedded in the file."), 50, "keep_printer_on_open");
    auto item_skip_mapping_warnings = create_item_checkbox(_L("Skip Settings Mapping Warnings"), page,
        _L("Don't show warnings about unrecognized or invalid settings replaced while loading project files; the defaults or automatic fixes are applied silently."), 50, "skip_settings_mapping_warnings");
    auto item_print_by_object_caution = create_item_checkbox(_L("Show print-by-object caution notices"), page,
        _L("Show the advisory notices suggesting auto-arrange, and the pre-slice caution, when Print sequence is set to \"By object\". Turn off here or via \"Do not show again\" on the notice itself."), 50, "show_print_by_object_caution");

    auto item_auto_drop = create_item_checkbox(_L("Drop imported models to the bed"), page,
        _L("When off, imported models keep the Z position stored in the file instead of being dropped onto the build plate."), 50, "auto_drop_on_import");
    auto item_bottom_z = create_item_checkbox(_L("Bottom-referenced Z position"), page,
        _L("Show and edit the Move panel's Z as the height of the object's bottom above the bed (world coordinates), instead of the object center."), 50, "bottom_referenced_z");
    auto item_hide_other_plates = create_item_checkbox(_L("Hide other plates while moving"), page,
        _L("While the Move tool is open, show only the plate you are working on. The other plates and their objects come back when the Move tool closes, and a plate you drag onto reappears when you release."), 50, "hide_other_plates_on_move");

    auto title_presets = create_item_title(_L("Presets"), page, _L("Presets"));
    auto item_prefer_last_print = create_item_checkbox(_L("Prefer Last Used Print Profile"), page,
        _L("When a project's print profile is not available, pick the profile you last used at the same layer height instead of the first compatible one."), 50, "prefer_last_print_profile");
    auto item_auto_shadow = create_item_checkbox(_L("Seamless System Filament Edits"), page,
        _L("Saving a modified system preset silently saves and selects \"<name> - Custom\" instead of asking for a new name every time."), 50, "auto_shadow_system_presets");
    auto item_sync_bambu_presets = create_item_checkbox(_L("Import Bambu Studio user presets"), page,
        _L("At startup, mirror your custom Bambu Studio print & filament presets into EdgeSlicer so they are usable here too. One-way (Bambu Studio stays the source); your own presets and deletions are preserved. Takes effect after restart."), 50, "sync_bambu_user_presets");
    auto item_sync_now = create_item_button(_L("Import from Bambu Studio now"), _L("Sync now"), page,
        _L("Import from Bambu Studio now"),
        _L("Re-run the one-way import of your Bambu Studio custom print/filament/machine presets and refresh the lists immediately."),
        []() { wxGetApp().sync_bambu_user_presets(true); });
    // Recovery affordance: an earlier build could delete mirrored presets it had copied (a Bambu
    // preset the loader could not parse was removed from disk and flagged deleted in the manifest,
    // so it was never pulled again). Clearing the flags makes the next sync restore them from
    // Bambu Studio, which still has every one of them.
    auto item_repull = create_item_button(_L("Re-import presets removed earlier"), _L("Re-pull all"), page,
        _L("Re-import presets removed earlier"),
        _L("Forget which mirrored presets were removed from this slicer, so the next import pulls them "
           "back from Bambu Studio. Your own presets are untouched."),
        []() {
            int cleared = repull_mirrored_presets();
            wxGetApp().sync_bambu_user_presets(true);
            MessageDialog(nullptr,
                wxString::Format(_L("Re-imported %d preset(s) that had been removed from this slicer."), cleared),
                _L("Import from Bambu Studio"), wxICON_INFORMATION | wxOK).ShowModal();
        });

    auto title_bambu = create_item_title(_L("Bambu Network"), page, _L("Bambu Network"));
    auto item_bambu_plugin = create_item_checkbox(_L("Enable Bambu network plugin"), page,
        _L("Connect to Bambu Lab printers (cloud login or LAN). The plug-in is downloaded from Bambu on first use; takes effect after restarting."), 50, "installed_networking");
    auto item_bambu_stealth = create_item_checkbox(_L("Stealth mode (no Bambu cloud telemetry)"), page,
        _L("Stops data transmission to Bambu's cloud services. Safe to enable for LAN-only use; disable it to sign in to a Bambu account."), 50, "stealth_mode");
    // Ultra (plug-in guards): "legacy" only picks WHICH Bambu CDN build to download. With our own
    // plug-in installed nothing is ever downloaded, so the switch has no effect - hide it.
    auto item_bambu_legacy = create_item_checkbox(_L("Use legacy network plugin"), page,
        _L("Use the older plug-in for printers on old Bambu firmware. Takes effect after restarting."), 50, "legacy_networking");

    auto title_spoolman = create_item_title(_L("Spool Manager"), page, _L("Spool Manager"));
    auto item_spoolman_enabled = create_item_checkbox(_L("Enable Spoolman integration"), page,
        _L("Connect to a self-hosted Spoolman server for spool inventory and filament-usage tracking."), 50, "spoolman_enabled");
    auto item_spoolman_url = create_item_text_input(_L("Server URL"), page,
        _L("Spoolman server address, e.g. http://10.0.0.5:7912"), "spoolman_url");
    auto item_spoolman_deduct = create_item_checkbox(_L("Deduct filament usage when sending a print"), page,
        _L("After a job is sent to the printer, subtract the sliced filament weight from the spools bound to each slot in the Spool Manager."), 50, "spoolman_deduct");

    // Ultra: hidden service mode. The hub passes SNORCA_HIDDEN explicitly for the slicers it starts,
    // so this only decides slicers started by hand (double-click, file association, command line).
    auto title_phone = create_item_title(_L("Phone access"), page, _L("Phone access"));
    auto item_start_hidden = create_item_checkbox(_L("Start hidden (serve the phone without a window)"), page,
        _L("Slicers you start by hand run without a window and serve the phone in the background. Bring a window back from the hub's tray icon (Slicer windows > Show), the hub page, or the phone's \"Show on PC\". Slicers started from the phone are always hidden, and a hidden slicer shows itself when it needs a person at the PC."), 50, "start_hidden");
    // Ultra (phone Snapmaker connect): a Snapmaker printer only answers a client holding the certificate it
    // issued at pairing, which the Device tab keeps in the web page alone. With this on, a connect made on
    // this PC leaves a copy under <datadir>/hub/snapmaker_keys.json so the phone can connect the printer
    // later. Off by default: it is a private key at rest.
    auto item_sm_keys = create_item_checkbox(_L("Remember Snapmaker printer certificates so the phone can connect them"), page,
        _L("After you connect a Snapmaker printer on the Device tab, keep its pairing certificate on this PC (under the data folder's hub\snapmaker_keys.json) so the phone's Devices tab can connect that printer without you at the PC. The certificate never leaves this PC. Off by default because it is a private key stored on disk."), 50, "snapmaker_remember_keys");

    // Ultra (G-code archive): every file this PC uploads is kept, with the printer it went to, so
    // the phone can send it again later without the project being open.
    auto title_archive = create_item_title(_L("G-Code Archive"), page, _L("G-Code Archive"));
    auto item_archive = create_item_checkbox(_L("Store G-Code Files"), page,
        _L("Keep a copy of every G-code file this PC sends to a printer - from here or from the phone - together with which printer it went to, so it can be sent again later without opening the project."), 50, "ultra_gcode_archive");
    auto item_archive_dir = create_item_gcode_archive_dir(page,
        _L("Where the copies are kept. The default is a gcode_archive folder inside the data folder; the folder is created the first time a file is stored."));
    auto item_archive_max = create_item_gcode_archive_max(page,
        _L("How many stored files to keep. When a new file takes the count past this, the oldest ones are deleted with their details and previews."));

    sizer_page->Add(title_project, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_autosave, 0, wxTOP, FromDIP(3));
    item_autosave->Add(item_autosave_interval, 0, wxLEFT, 0);
    sizer_page->Add(item_keep_printer, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_skip_mapping_warnings, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_print_by_object_caution, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_auto_drop, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_bottom_z, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_hide_other_plates, 0, wxTOP, FromDIP(3));
    sizer_page->Add(title_presets, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_prefer_last_print, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_auto_shadow, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_sync_bambu_presets, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_sync_now, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_repull, 0, wxTOP, FromDIP(3));
    sizer_page->Add(title_bambu, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_bambu_plugin, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_bambu_stealth, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_bambu_legacy, 0, wxTOP, FromDIP(3));
    // Ultra (plug-in guards): hidden after the Add so the sizer keeps no space for it. The switch
    // only picks WHICH Bambu CDN build to download; with UltraNet installed nothing is downloaded.
    if (wxGetApp().is_ultranet_plugin_installed()) {
        sizer_page->Hide(item_bambu_legacy, true);
        BOOST_LOG_TRIVIAL(info) << "[UltraNet] UltraNet present, Bambu CDN download disabled (Preferences legacy-plugin switch hidden)";
    }
    sizer_page->Add(title_spoolman, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_spoolman_enabled, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_spoolman_url, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_spoolman_deduct, 0, wxTOP, FromDIP(3));
    sizer_page->Add(title_phone, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_start_hidden, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_sm_keys, 0, wxTOP, FromDIP(3));
    sizer_page->Add(title_archive, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_archive, 0, wxTOP, FromDIP(3));
    sizer_page->Add(item_archive_dir, 0, wxTOP | wxEXPAND, FromDIP(3));
    sizer_page->Add(item_archive_max, 0, wxTOP, FromDIP(3));

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
    return page;
}

// NOTE: this page is still never added to create()'s `pages` tab list (create_gui_page() itself
// is never called), so it remains unreachable from the UI. The one working control it built,
// the gizmo panel opacity slider, has been moved to create_general_page() (see
// create_item_gizmo_panel_opacity() usage there). "show_home_page" is left here unexposed: it is
// only ever set to a default in AppConfig::set_defaults() and is not read anywhere else, so a
// checkbox for it would not do anything yet.
void PreferencesDialog::create_gui_page()
{
    auto page = new wxWindow(this, wxID_ANY);
    wxBoxSizer *sizer_page = new wxBoxSizer(wxVERTICAL);

    auto title_index_and_tip = create_item_title(_L("Home page and daily tips"), page, _L("Home page and daily tips"));
    auto item_home_page      = create_item_checkbox(_L("Show home page on startup"), page, _L("Show home page on startup"), 50, "show_home_page");
    //auto item_daily_tip      = create_item_checkbox(_L("Show daily tip on startup"), page, _L("Show daily tip on startup"), 50, "show_daily_tips");

    auto title_tool_panels = create_item_title(_L("3D view tool panels"), page, _L("3D view tool panels"));
    auto item_panel_opacity = create_item_gizmo_panel_opacity(page,
        _L("Background opacity of the tool panels on the 3D view. Lower values let you see the model behind a docked panel. Applies immediately."));

    sizer_page->Add(title_index_and_tip, 0, wxTOP, 26);
    sizer_page->Add(item_home_page, 0, wxTOP, 6);
    //sizer_page->Add(item_daily_tip, 0, wxTOP, 6);
    sizer_page->Add(title_tool_panels, 0, wxTOP | wxEXPAND, FromDIP(20));
    sizer_page->Add(item_panel_opacity, 0, wxTOP, 6);

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
}

void PreferencesDialog::create_sync_page()
{
    auto page = new wxWindow(this, wxID_ANY);
    wxBoxSizer *sizer_page = new wxBoxSizer(wxVERTICAL);

     auto title_sync_settingy   = create_item_title(_L("Sync settings"), page, _L("Sync settings"));
    auto item_user_sync        = create_item_checkbox(_L("User sync"), page, _L("User sync"), 50, "user_sync_switch");
    auto item_preset_sync      = create_item_checkbox(_L("Preset sync"), page, _L("Preset sync"), 50, "preset_sync_switch");
    auto item_preferences_sync = create_item_checkbox(_L("Preferences sync"), page, _L("Preferences sync"), 50, "preferences_sync_switch");

    sizer_page->Add(title_sync_settingy, 0, wxTOP, 26);
    sizer_page->Add(item_user_sync, 0, wxTOP, 6);
    sizer_page->Add(item_preset_sync, 0, wxTOP, 6);
    sizer_page->Add(item_preferences_sync, 0, wxTOP, 6);

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
}

void PreferencesDialog::create_shortcuts_page()
{
    auto page = new wxWindow(this, wxID_ANY);
    wxBoxSizer *sizer_page = new wxBoxSizer(wxVERTICAL);

    auto title_view_control = create_item_title(_L("View control settings"), page, _L("View control settings"));
    std::vector<wxString> keyboard_supported;
    Split(app_config->get("keyboard_supported"), "/", keyboard_supported);

    std::vector<wxString> mouse_supported;
    Split(app_config->get("mouse_supported"), "/", mouse_supported);

    auto item_rotate_view = create_item_multiple_combobox(_L("Rotate of view"), page, _L("Rotate of view"), 10, "rotate_view", keyboard_supported,
                                                               mouse_supported);
    auto item_move_view   = create_item_multiple_combobox(_L("Move of view"), page, _L("Move of view"), 10, "move_view", keyboard_supported, mouse_supported);
    auto item_zoom_view   = create_item_multiple_combobox(_L("Zoom of view"), page, _L("Zoom of view"), 10, "rotate_view", keyboard_supported, mouse_supported);

    auto title_other = create_item_title(_L("Other"), page, _L("Other"));
    auto item_other  = create_item_checkbox(_L("Mouse wheel reverses when zooming"), page, _L("Mouse wheel reverses when zooming"), 50, "mouse_wheel");

    sizer_page->Add(title_view_control, 0, wxTOP, 26);
    sizer_page->Add(item_rotate_view, 0, wxTOP, 8);
    sizer_page->Add(item_move_view, 0, wxTOP, 8);
    sizer_page->Add(item_zoom_view, 0, wxTOP, 8);
    // sizer_page->Add(item_precise_control, 0, wxTOP, 0);
    sizer_page->Add(title_other, 0, wxTOP, 20);
    sizer_page->Add(item_other, 0, wxTOP, 5);

    page->SetSizer(sizer_page);
    page->Layout();
    sizer_page->Fit(page);
}

wxWindow* PreferencesDialog::create_debug_page()
{
    auto page = new wxWindow(m_scrolledWindow, wxID_ANY);
    page->SetBackgroundColour(*wxWHITE);

    m_internal_developer_mode_def = app_config->get("internal_developer_mode");
    m_websocket_debug_def = app_config->get("websocket_debug");
    m_backup_interval_def = app_config->get("backup_interval");
    m_iot_environment_def = app_config->get("iot_environment");

    wxBoxSizer *bSizer = new wxBoxSizer(wxVERTICAL);


    auto enable_ssl_for_mqtt = create_item_checkbox(_L("Enable SSL(MQTT)"), page, _L("Enable SSL(MQTT)"), 50, "enable_ssl_for_mqtt");
    auto enable_ssl_for_ftp = create_item_checkbox(_L("Enable SSL(FTP)"), page, _L("Enable SSL(MQTT)"), 50, "enable_ssl_for_ftp");
    auto item_internal_developer = create_item_checkbox(_L("Internal developer mode"), page, _L("Internal developer mode"), 50, "internal_developer_mode");

    auto title_log_level = create_item_title(_L("Log Level"), page, _L("Log Level"));
    auto log_level_list  = std::vector<wxString>{_L("fatal"), _L("error"), _L("warning"), _L("info"), _L("debug"), _L("trace")};
    auto loglevel_combox = create_item_loglevel_combobox(_L("Log Level"), page, _L("Log Level"), log_level_list);

    auto title_host = create_item_title(_L("Host Setting"), page, _L("Host Setting"));
    auto radio1     = create_item_radiobox(_L("DEV host: api-dev.bambu-lab.com/v1"), page, wxEmptyString, 50, 1, "dev_host");
    auto radio2     = create_item_radiobox(_L("QA  host: api-qa.bambu-lab.com/v1"), page, wxEmptyString, 50, 1, "qa_host");
    auto radio3     = create_item_radiobox(_L("PRE host: api-pre.bambu-lab.com/v1"), page, wxEmptyString, 50, 1, "pre_host");
    auto radio4     = create_item_radiobox(_L("Product host"), page, wxEmptyString, 50, 1, "product_host");

    if (m_iot_environment_def == ENV_DEV_HOST) {
        on_select_radio("dev_host");
    } else if (m_iot_environment_def == ENV_QAT_HOST) {
        on_select_radio("qa_host");
    } else if (m_iot_environment_def == ENV_PRE_HOST) {
        on_select_radio("pre_host");
    } else if (m_iot_environment_def == ENV_PRODUCT_HOST) {
        on_select_radio("product_host");
    }


    StateColor btn_bg_white(std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Disabled), std::pair<wxColour, int>(AMS_CONTROL_DISABLE_COLOUR, StateColor::Pressed),
        std::pair<wxColour, int>(AMS_CONTROL_DEF_BLOCK_BK_COLOUR, StateColor::Hovered),
        std::pair<wxColour, int>(AMS_CONTROL_WHITE_COLOUR, StateColor::Normal));
    StateColor btn_bd_white(std::pair<wxColour, int>(AMS_CONTROL_WHITE_COLOUR, StateColor::Disabled), std::pair<wxColour, int>(wxColour(38, 46, 48), StateColor::Enabled));

    Button* debug_button = new Button(page, _L("debug save button"));
    debug_button->SetStyle(ButtonStyle::Regular, ButtonType::Window);

    debug_button->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &e) {
        // success message box
        MessageDialog dialog(this, _L("save debug settings"), _L("DEBUG settings have been saved successfully!"), wxNO_DEFAULT | wxYES_NO | wxICON_INFORMATION);
        dialog.SetSize(400,-1);
        switch (dialog.ShowModal()) {
        case wxID_NO: {
            //if (m_developer_mode_def != app_config->get("developer_mode")) {
            //    app_config->set_bool("developer_mode", m_developer_mode_def == "true" ? true : false);
            //    m_developer_mode_ckeckbox->SetValue(m_developer_mode_def == "true" ? true : false);
            //}
            //if (m_internal_developer_mode_def != app_config->get("internal_developer_mode")) {
            //    app_config->set_bool("internal_developer_mode", m_internal_developer_mode_def == "true" ? true : false);
            //    m_internal_developer_mode_ckeckbox->SetValue(m_internal_developer_mode_def == "true" ? true : false);
            //}

            if (m_backup_interval_def != m_backup_interval_time) { m_backup_interval_textinput->GetTextCtrl()->SetValue(m_backup_interval_def); }

            if (m_iot_environment_def == ENV_DEV_HOST) {
                on_select_radio("dev_host");
            } else if (m_iot_environment_def == ENV_QAT_HOST) {
                on_select_radio("qa_host");
            } else if (m_iot_environment_def == ENV_PRE_HOST) {
                on_select_radio("pre_host");
            } else if (m_iot_environment_def == ENV_PRODUCT_HOST) {
                on_select_radio("product_host");
            }

            break;
        }

        case wxID_YES: {
            // bbs  domain changed
            auto param = get_select_radio(1);

            std::map<wxString, wxString> iot_environment_map;
            iot_environment_map["dev_host"] = ENV_DEV_HOST;
            iot_environment_map["qa_host"]  = ENV_QAT_HOST;
            iot_environment_map["pre_host"] = ENV_PRE_HOST;
            iot_environment_map["product_host"] = ENV_PRODUCT_HOST;

            //if (iot_environment_map[param] != m_iot_environment_def) {
            if (true) {
                NetworkAgent* agent = wxGetApp().getAgent();
                if (param == "dev_host") {
                    app_config->set("iot_environment", ENV_DEV_HOST);
                }
                else if (param == "qa_host") {
                    app_config->set("iot_environment", ENV_QAT_HOST);
                }
                else if (param == "pre_host") {
                    app_config->set("iot_environment", ENV_PRE_HOST);
                }
                else if (param == "product_host") {
                    app_config->set("iot_environment", ENV_PRODUCT_HOST);
                }



                AppConfig* config = GUI::wxGetApp().app_config;
                std::string country_code = config->get_country_code();
                if (agent) {
                    wxGetApp().request_user_logout();
                    agent->set_country_code(country_code);
                }
                ConfirmBeforeSendDialog confirm_dlg(this, wxID_ANY, _L("Warning"), ConfirmBeforeSendDialog::ButtonStyle::ONLY_CONFIRM);
                confirm_dlg.update_text(_L("Cloud environment switched, please login again!"));
                confirm_dlg.on_show();
            }

            // bbs  backup
            //app_config->set("backup_interval", std::string(m_backup_interval_time.mb_str()));
            app_config->save();
            Slic3r::set_backup_interval(boost::lexical_cast<long>(app_config->get("backup_interval")));

            this->Close();
            break;
        }
        }
    });


    bSizer->Add(enable_ssl_for_mqtt, 0, wxTOP, FromDIP(3));
    bSizer->Add(enable_ssl_for_ftp, 0, wxTOP, FromDIP(3));
    bSizer->Add(item_internal_developer, 0, wxTOP, FromDIP(3));
    bSizer->Add(title_log_level, 0, wxTOP| wxEXPAND, FromDIP(20));
    bSizer->Add(loglevel_combox, 0, wxTOP, FromDIP(3));
    bSizer->Add(title_host, 0, wxTOP| wxEXPAND, FromDIP(20));
    bSizer->Add(radio1, 0, wxEXPAND | wxTOP, FromDIP(3));
    bSizer->Add(radio2, 0, wxEXPAND | wxTOP, FromDIP(3));
    bSizer->Add(radio3, 0, wxEXPAND | wxTOP, FromDIP(3));
    bSizer->Add(radio4, 0, wxEXPAND | wxTOP, FromDIP(3));
    bSizer->Add(debug_button, 0, wxALIGN_CENTER_HORIZONTAL | wxTOP, FromDIP(15));

    page->SetSizer(bSizer);
    page->Layout();
    bSizer->Fit(page);
    return page;
}

void PreferencesDialog::on_select_radio(std::string param)
{
    RadioSelectorList::compatibility_iterator it = m_radio_group.GetFirst();
    auto                     groupid = 0;

    while (it) {
        RadioSelector *rs = it->GetData();
        if (rs->m_param_name == param) groupid = rs->m_groupid;
        it = it->GetNext();
    }

    it = m_radio_group.GetFirst();
    while (it) {
        RadioSelector *rs = it->GetData();
        if (rs->m_groupid == groupid && rs->m_param_name == param) rs->m_radiobox->SetValue(true);
        if (rs->m_groupid == groupid && rs->m_param_name != param) rs->m_radiobox->SetValue(false);
        it = it->GetNext();
    }
}

wxString PreferencesDialog::get_select_radio(int groupid)
{
    RadioSelectorList::compatibility_iterator it = m_radio_group.GetFirst();
    while (it) {
        RadioSelector *rs = it->GetData();
        if (rs->m_groupid == groupid && rs->m_radiobox->GetValue()) { return rs->m_param_name; }
        it = it->GetNext();
    }

    return wxEmptyString;
}

void PreferencesDialog::OnSelectRadio(wxMouseEvent &event)
{
    RadioSelectorList::compatibility_iterator it = m_radio_group.GetFirst();
    auto                     groupid = 0;

    while (it) {
        RadioSelector *rs = it->GetData();
        if (rs->m_radiobox->GetId() == event.GetId()) groupid = rs->m_groupid;
        it = it->GetNext();
    }

    it = m_radio_group.GetFirst();
    while (it) {
        RadioSelector *rs = it->GetData();
        if (rs->m_groupid == groupid && rs->m_radiobox->GetId() == event.GetId()) rs->m_radiobox->SetValue(true);
        if (rs->m_groupid == groupid && rs->m_radiobox->GetId() != event.GetId()) rs->m_radiobox->SetValue(false);
        it = it->GetNext();
    }
}


}} // namespace Slic3r::GUI
