#include "MixingGradientWeightsDialog.hpp"

namespace Slic3r {
namespace GUI {

MixedGradientWeightsDialog::MixedGradientWeightsDialog(wxWindow *parent,
                               const std::vector<unsigned int> &filament_ids,
                               const std::vector<wxColour> &palette,
                               const std::vector<int> &initial_weights)
        : wxDialog(parent, wxID_ANY, _L("Gradient Mix Weights"), wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    {
        m_colors.reserve(filament_ids.size());
        m_weights = normalize_color_match_weights(initial_weights, filament_ids.size());
        for (const unsigned int filament_id : filament_ids) {
            if (filament_id >= 1 && filament_id <= palette.size())
                m_colors.emplace_back(palette[filament_id - 1]);
            else
                m_colors.emplace_back(wxColour("#26A69A"));
        }
        if (m_colors.empty())
            m_colors.emplace_back(wxColour("#26A69A"));

        auto *root = new wxBoxSizer(wxVERTICAL);
        auto *hint = new wxStaticText(this, wxID_ANY, _L("Pick a point in the gradient map to control multi-filament mix."));
        root->Add(hint, 0, wxEXPAND | wxALL, FromDIP(10));

        m_color_map = new MixedFilamentColorMapPanel(this, filament_ids, palette, initial_weights,
                                                     wxSize(FromDIP(240), FromDIP(240)));
        root->Add(m_color_map, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        for (size_t i = 0; i < filament_ids.size(); ++i) {
            auto *row = new wxBoxSizer(wxHORIZONTAL);
            wxPanel *chip = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(18), FromDIP(18)), wxBORDER_SIMPLE);
            chip->SetBackgroundColour(m_colors[i]);
            row->Add(chip, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
            row->Add(new wxStaticText(this, wxID_ANY, wxString::Format("F%d", int(filament_ids[i]))),
                     0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
            auto *label = new wxStaticText(this, wxID_ANY, wxString::Format("%d%%", m_weights[i]));
            label->SetFont(Label::Body_12);
            row->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            root->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
            m_weight_labels.emplace_back(label);
        }

        root->Add(CreateSeparatedButtonSizer(wxOK | wxCANCEL), 0, wxEXPAND | wxALL, FromDIP(8));
        SetSizerAndFit(root);
        SetMinSize(wxSize(FromDIP(380), std::max(GetSize().GetHeight(), FromDIP(460))));
        update_weight_labels();

        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

        if (m_color_map) {
            m_color_map->Bind(wxEVT_SLIDER, [this](wxCommandEvent &) {
                m_weights = m_color_map ? m_color_map->normalized_weights() : m_weights;
                update_weight_labels();
            });
        }
    }

} // namespace GUI
} // namespace Slic3r
