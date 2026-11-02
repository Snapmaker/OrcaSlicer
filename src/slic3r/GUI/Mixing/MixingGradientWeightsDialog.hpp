#pragma once

#include <vector>

#include <wx/dialog.h>
#include <wx/colour.h>

#include "../MixedColorMatchHelpers.hpp"
#include "../MixedFilamentColorMapPanel.hpp"
#include "../GUI_App.hpp"
#include "../I18N.hpp"

namespace Slic3r {
namespace GUI {

class MixedGradientWeightsDialog : public wxDialog
{
public:
    MixedGradientWeightsDialog(wxWindow *parent,
                               const std::vector<unsigned int> &filament_ids,
                               const std::vector<wxColour> &palette,
                               const std::vector<int> &initial_weights);

    std::vector<int> normalized_weights() const
    {
        return m_color_map ? m_color_map->normalized_weights() : m_weights;
    }

private:
    void update_weight_labels()
    {
        for (size_t i = 0; i < m_weight_labels.size() && i < m_weights.size(); ++i) {
            if (m_weight_labels[i])
                m_weight_labels[i]->SetLabel(wxString::Format("%d%%", m_weights[i]));
        }
        Layout();
    }

private:
    MixedFilamentColorMapPanel *m_color_map { nullptr };
    std::vector<wxColour>       m_colors;
    std::vector<int>            m_weights;
    std::vector<wxStaticText*>  m_weight_labels;
};

} // namespace GUI
} // namespace Slic3r

