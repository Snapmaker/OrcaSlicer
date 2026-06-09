#pragma once

#include "GUI_Utils.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "libslic3r/MixedFilament.hpp"

#include <wx/wx.h>
#include <wx/wrapsizer.h>

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <string>
#include <vector>

class Button;
class ComboBox;
class Label;
class StaticBox;

namespace Slic3r { namespace GUI {

class MixedFilamentBatchDialog : public DPIDialog
{
public:
    MixedFilamentBatchDialog(wxWindow* parent);
    ~MixedFilamentBatchDialog() override;

    const BatchMatchResult& GetResult() const { return m_result; }
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void build_ui();
    void on_method_changed(int method);
    void on_manual_selection_changed();
    void update_prompt_text();

    void start_batch_match();
    void cancel_batch_match();
    void launch_background_match();
    void handle_batch_match_result(const BatchMatchResult& result);

    void update_mapping_legend();

    // Same error/warning pattern as MixedFilamentDialog
    void display_warning(const wxString& msg);
    void set_error(const wxString& msg);

    void set_match_buttons_state(bool matching);
    void load_model_colors();

    enum MatchingMethod { RECOMMENDED = 0, MANUAL = 1 };

    MatchingMethod m_matching_method = RECOMMENDED;
    BatchMatchResult m_result;
    bool             m_match_completed = false;
    bool             m_match_running   = false;
    std::shared_ptr<std::atomic<bool>> m_destroyed        { std::make_shared<std::atomic<bool>>(false) };
    std::shared_ptr<std::atomic<bool>> m_cancel_requested { std::make_shared<std::atomic<bool>>(false) };
    std::thread      m_worker_thread;

    std::vector<ModelColorEntry> m_model_colors;
    std::vector<std::string>     m_physical_colors;

    bool  m_filament_enabled[4] = {true, true, false, false};
    int   m_filament_selections[4] = {0, 1, -1, -1};

    // Segmented method buttons (same as MixedFilamentDialog::build_ui §1)
    int                       m_method_selected = 0;
    std::vector<Button*>      m_method_buttons;

    // Manual filament card
    wxWindow*                 m_manual_card = nullptr;
    wxCheckBox*               m_enable_check[4] = {nullptr};
    ComboBox*                 m_filament_combo[4] = {nullptr};

    // Legend card
    wxScrolledWindow*         m_legend_scroller = nullptr;
    wxWrapSizer*              m_legend_sizer = nullptr;

    // Prompt
    Label*                    m_prompt_label = nullptr;

    // Error / warning (same icon+label pattern as MixedFilamentDialog)
    wxPanel*                  m_error_panel = nullptr;
    Label*                    m_error_text = nullptr;
    wxPanel*                  m_warning_panel = nullptr;
    Label*                    m_warning_text = nullptr;

    // Progress
    wxGauge*                  m_progress_bar = nullptr;
    Label*                    m_progress_label = nullptr;

    // Buttons
    Button*                   m_btn_start_match = nullptr;
    Button*                   m_btn_cancel_match = nullptr;
    Button*                   m_btn_rematch      = nullptr;
    Button*                   m_btn_confirm      = nullptr;

    // Scroll
    wxScrolledWindow*         m_scrolled_content = nullptr;
};

}} // namespace Slic3r::GUI
