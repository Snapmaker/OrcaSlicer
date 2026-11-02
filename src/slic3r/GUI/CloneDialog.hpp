#ifndef slic3r_GUI_SingleChoice_hpp_
#define slic3r_GUI_SingleChoice_hpp_

#include "GUI_Utils.hpp"
namespace Slic3r { namespace GUI { class Plater; } }   // H2 3.1：Plater 仅指针形态——前向声明替代传递包含
#include "Selection.hpp"

#include "Widgets/Button.hpp"
#include "Widgets/SpinInput.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ProgressBar.hpp"

namespace Slic3r { namespace GUI {

class CloneDialog : public DPIDialog
{
public:
    CloneDialog(wxWindow *parent = nullptr);
    ~CloneDialog();

private:
    SpinInput*   m_count_spin;
    int          m_count;
    CheckBox*    m_arrange_cb;
    Plater*      m_plater;
    ProgressBar* m_progress;
    AppConfig*   m_config;
    bool         m_cancel_process;

    void on_dpi_changed(const wxRect &suggested_rect) override {}

};
}} // namespace Slic3r::GUI

#endif