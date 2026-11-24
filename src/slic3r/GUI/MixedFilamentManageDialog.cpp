#include "MixedFilamentManageDialog.hpp"
#include "MixedFilamentManageWidgets.hpp"
#include "Widgets/Accordion.hpp"

#include "GUI_App.hpp"
#include "GUI.hpp"
#include "GUI_Utils.hpp"
#include "I18N.hpp"
#include "MFDTheme.hpp"
#include "MixedColorMatchHelpers.hpp"
#include "MixedFilamentBadge.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/MixedFilament/Recommendations.hpp"
#include "libslic3r/MixedFilament/Serialization.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/scrolwin.h>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <numeric>
#include <stdexcept>

namespace Slic3r::GUI {
namespace {

void track_manage(const char* action, nlohmann::json data = nlohmann::json::object())
{
    // Use the application's existing tracking/consent path. No material names,
    // project data or colors are included in these interaction events.
    try {
        data["action"] = action;
        if (auto* agent = wxGetApp().getAgent())
            agent->track_event("mixed_filament_manage", data.dump());
    } catch (...) {
        // Tracking must never interrupt editing.
    }
}

wxString recipe_tooltip(const MixedFilamentDefinition& definition)
{
    wxString   text;
    const auto row = mixed_filament_legacy_row_from_definition(definition);
    if (definition.behavior.gradient.enabled) {
        const auto ids = mixed_gradient_components(row, 0);
        for (unsigned int id : ids) {
            if (!text.empty())
                text += " > ";
            text += wxString::Format("F%u", id);
        }
    } else if (definition.recipe.kind == MixedFilamentRecipeKind::ManualPattern && definition.recipe.manual_pattern) {
        for (const auto& group : definition.recipe.manual_pattern->groups) {
            if (!text.empty())
                text += ", ";
            for (size_t i = 0; i < group.size(); ++i) {
                if (i)
                    text += " > ";
                text += wxString::Format("F%u", group[i].id);
            }
        }
    } else {
        for (const auto& component : definition.recipe.blend.components) {
            if (!text.empty())
                text += " + ";
            text += wxString::Format("F%u %d%%", component.filament.id, component.percent);
        }
    }
    return text;
}

class MixedFilamentManageDialog : public DPIDialog
{
public:
    MixedFilamentManageDialog(wxWindow* parent, MixedFilamentDisplayContext context, std::vector<std::string> types)
        : DPIDialog(parent, wxID_ANY, _L("Manage Mixed Filaments"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
        , m_context(std::move(context))
        , m_catalog(m_context, types, wxGetApp().preset_bundle->mixed_filaments.mixed_filament_definitions(m_context.num_physical))
    {
        build_ui();
        track_manage("open");
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { cancel("close"); });
    }

private:
    MixedFilamentDisplayContext           m_context;
    MixedFilamentRecommendations          m_catalog;
    wxScrolledWindow*                     m_scroll              = nullptr;
    wxStaticText*                         m_counts              = nullptr;
    wxStaticText*                         m_error               = nullptr;
    BatchCheckBox*                        m_select_all          = nullptr;
    Accordion*                            m_saved_section       = nullptr;
    Accordion*                            m_recommended_section = nullptr;
    wxPanel*                              m_footer              = nullptr;
    BatchSwatchGrid*                      m_saved_grid          = nullptr;
    std::vector<BatchSwatchGrid*>         m_grids;
    std::array<CollapsibleSubSection*, 2> m_groups{};
    std::array<BatchSwatchGrid*, 2>       m_recommendation_grids{};
    std::array<std::vector<size_t>, 2>    m_group_indices;
    Button*                               m_confirm    = nullptr;
    Button*                               m_cancel     = nullptr;
    Button*                               m_remove_all = nullptr;

    wxStaticText* label(wxWindow* parent, const wxString& text, bool heading = false)
    {
        auto* result = new wxStaticText(parent, wxID_ANY, text);
        result->SetFont(heading ? Label::Head_14 : Label::Body_13);
        MFDTheme::apply_text(result, MFDTheme::primary_text(), parent->GetBackgroundColour());
        return result;
    }
    BatchSwatchGrid* add_grid(
        wxWindow* parent, wxSizer* sizer, std::vector<ManageSwatch> swatches, std::function<void(size_t)> toggle = {}, int border = 0)
    {
        auto* grid = new BatchSwatchGrid(parent, std::move(swatches), std::move(toggle));
        sizer->Add(grid, 0, wxEXPAND | wxALL, border);
        m_grids.push_back(grid);
        return grid;
    }

    void build_ui()
    {
        // FullSpectrum main's dialog shell and accordion layout. The PRD changes
        // the grouping/selection rules, while the original widgets render them.
        SetBackgroundColour(MFDTheme::dialog_background());
        auto* root = new wxBoxSizer(wxVERTICAL);
        SetSizer(root);
        auto* divider = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, 1));
        divider->SetBackgroundColour(MFDTheme::divider());
        root->Add(divider, 0, wxEXPAND);
        m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxTAB_TRAVERSAL);
        m_scroll->SetBackgroundColour(StateColor::darkModeColorFor(wxColour("#F8F7F7")));
        m_scroll->SetScrollRate(0, FromDIP(20));
        auto* content = new wxBoxSizer(wxVERTICAL);
        m_scroll->SetSizer(content);

        m_saved_section   = new Accordion(m_scroll, _L("Saved Mixed Filaments"));
        auto* saved_body  = m_saved_section->get_body_panel();
        auto* saved_sizer = m_saved_section->get_body_sizer();
        if (m_catalog.saved.empty()) {
            auto* placeholder = label(saved_body, _L("No mixed filaments"));
            placeholder->SetForegroundColour(MFDTheme::muted_text());
            saved_sizer->Add(placeholder, 0, wxEXPAND | wxALL, FromDIP(8));
        } else {
            std::vector<ManageSwatch> swatches;
            for (size_t i = 0; i < m_catalog.saved.size(); ++i) {
                const auto&  saved      = m_catalog.saved[i];
                const auto&  definition = saved.definition;
                ManageSwatch tile;
                if (definition.recipe.kind == MixedFilamentRecipeKind::WeightedBlend) {
                    std::vector<unsigned int> ids;
                    std::vector<int>          weights;
                    for (const auto& component : definition.recipe.blend.components) {
                        ids.push_back(component.filament.id);
                        weights.push_back(component.percent);
                    }
                    // Use the same automatic predictor and requested ratios as
                    // recommendations, independent of print-layer quantization.
                    tile.color = parse_mixed_color(blend_mixed_components(ids, weights, m_context));
                } else {
                    const auto                sequence = mixed_filament_manual_pattern_preview_sequence(definition, m_context.num_physical,
                                                                                                        m_context.preview_settings.wall_loops);
                    std::vector<unsigned int> ids(m_context.num_physical);
                    std::iota(ids.begin(), ids.end(), 1u);
                    std::vector<int> weights(ids.size(), 0);
                    for (unsigned int id : sequence)
                        if (id > 0 && id <= weights.size())
                            ++weights[id - 1];
                    tile.color = parse_mixed_color(blend_mixed_components(ids, weights, m_context));
                }
                tile.label   = saved.filament_id ? wxString::Format("%u", saved.filament_id) : "";
                tile.tooltip = recipe_tooltip(definition) + "\n" + _L("Click to mark for removal.");
                if (definition.behavior.gradient.enabled) {
                    const auto row = mixed_filament_legacy_row_from_definition(definition);
                    for (int i = 0; i <= 32; ++i)
                        tile.gradient.emplace_back(mixed_gradient_display_color(row, m_context, double(i) / 32.));
                }
                swatches.push_back(std::move(tile));
            }
            m_saved_grid = add_grid(
                saved_body, saved_sizer, std::move(swatches),
                [this](size_t i) {
                    auto& saved   = m_catalog.saved[i];
                    saved.removed = !saved.removed;
                    track_manage("remove", {{"selected", saved.removed}, {"count", m_catalog.removals()}});
                    refresh_state();
                },
                FromDIP(8));
            m_remove_all = new Button(saved_body, _CTX("Remove all", "MixedFilaments"));
            m_remove_all->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
            m_remove_all->SetToolTip(_L("Mark all saved mixed filaments for removal. Changes are applied when you click Confirm."));
            m_remove_all->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
                for (auto& saved : m_catalog.saved)
                    saved.removed = true;
                track_manage("remove_all", {{"count", m_catalog.removals()}});
                refresh_state();
            });
            saved_sizer->Add(m_remove_all, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        }

        content->Add(m_saved_section, 0, wxEXPAND | wxALL, FromDIP(8));

        m_recommended_section = new Accordion(m_scroll, _L("Mixing Recommendations"));
        auto* body            = m_recommended_section->get_body_panel();
        auto* recommendations = m_recommended_section->get_body_sizer();
        auto* explainer       = label(body, _L("Current physical filaments"));
        explainer->SetForegroundColour(MFDTheme::secondary_text());
        recommendations->Add(explainer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(8));
        std::vector<ManageSwatch> physical;
        for (size_t i = 0; i < m_context.num_physical; ++i) {
            ManageSwatch tile;
            tile.color    = parse_mixed_color(m_context.physical_colors[i]);
            tile.label    = wxString::Format("%u", unsigned(i + 1));
            tile.tooltip  = wxString::Format("F%u", unsigned(i + 1));
            tile.editable = false;
            physical.push_back(std::move(tile));
        }
        add_grid(body, recommendations, std::move(physical), {}, FromDIP(8));
        recommendations->AddSpacer(FromDIP(4));
        if (m_catalog.excluded_incompatible) {
            auto* warning = label(body,
                                  m_catalog.recommendations.empty() ?
                                      _L("The current physical filament types are incompatible. Use the same or compatible types to "
                                         "generate recommendations.") :
                                      _L("Some filament types are incompatible. Combinations that cannot be mixed have been excluded."));
            warning->Wrap(FromDIP(400));
            recommendations->Add(warning, 0, wxEXPAND | wxALL, FromDIP(8));
        }
        m_select_all = new BatchCheckBox(m_recommended_section->get_header_panel(), [this]() {
            m_catalog.select_all(m_catalog.selection_state() != 2);
            track_manage("select_all", {{"count", m_catalog.additions()}});
            refresh_state();
        });
        m_select_all->SetToolTip(_L("Select all recommendations"));
        m_select_all->SetName(_L("Select all recommendations"));
        m_recommended_section->add_header_control(m_select_all, true);
        for (size_t group = 0; group < 2; ++group) {
            auto* section     = new CollapsibleSubSection(body, group == 0 ? _L("Two-color Mixing") : _L("Three-color Mixing"));
            m_groups[group]   = section;
            auto* group_sizer = new wxBoxSizer(wxVERTICAL);
            section->get_body()->SetSizer(group_sizer);
            std::vector<ManageSwatch> swatches;
            for (size_t i = 0; i < m_catalog.recommendations.size(); ++i) {
                const auto& item = m_catalog.recommendations[i];
                if (item.definition.recipe.blend.components.size() != group + 2)
                    continue;
                ManageSwatch tile;
                tile.color    = parse_mixed_color(item.definition.presentation.display_color);
                tile.tooltip  = recipe_tooltip(item.definition);
                tile.editable = !item.registered();
                if (item.registered()) {
                    const auto id = m_catalog.saved[item.saved_indices.front()].filament_id;
                    tile.label    = id ? wxString::Format("%u", id) : "";
                    tile.tooltip += "\n" + _L("Registered. Remove from Saved Mixed Filaments.");
                }
                swatches.push_back(std::move(tile));
                m_group_indices[group].push_back(i);
            }
            if (swatches.empty()) {
                group_sizer->Add(label(section->get_body(), _L("No mixing recommendations")), 0, wxALL, FromDIP(8));
            } else {
                m_recommendation_grids[group] = add_grid(section->get_body(), group_sizer, std::move(swatches), [this, group](size_t index) {
                    auto& item             = m_catalog.recommendations[m_group_indices[group][index]];
                    item.selected          = !item.selected;
                    const auto& components = item.definition.recipe.blend.components;
                    const char* ratio      = components.size() == 2 ? (components[0].percent == 50 ? "1:1" : "1:2") :
                                                                      (components[0].percent == 33 ? "1:1:1" : "1:1:2");
                    track_manage("select", {{"selected", item.selected}, {"ratio", ratio}});
                    refresh_state();
                });
            }
            section->on_check_clicked = [this, group]() {
                m_catalog.select_all(m_catalog.selection_state(group + 2) != 2, group + 2);
                track_manage("select_all", {{"components", group + 2}, {"count", m_catalog.additions(group + 2)}});
                refresh_state();
            };
            recommendations->Add(section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        }
        content->Add(m_recommended_section, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(8));
        root->Add(m_scroll, 1, wxEXPAND);

        m_error = label(this, "");
        m_error->SetForegroundColour(MFDTheme::error_text());
        root->Add(m_error, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
        m_error->Hide();
        m_footer = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(-1, FromDIP(30)), wxBORDER_NONE);
        m_footer->SetBackgroundColour(GetBackgroundColour());
        auto* footer = new wxBoxSizer(wxHORIZONTAL);
        m_footer->SetSizer(footer);
        m_counts = label(m_footer, "");
        m_counts->SetFont(Label::Body_14);
        m_cancel = new Button(m_footer, _L("Cancel"), "", 0, 0, wxID_CANCEL);
        m_cancel->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
        m_cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { cancel("cancel"); });
        m_confirm = new Button(m_footer, _L("Confirm"), "", 0, 0, wxID_OK);
        m_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
        m_confirm->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { confirm(); });
        footer->Add(m_counts, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(16));
        footer->AddStretchSpacer();
        footer->Add(m_cancel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        footer->Add(m_confirm, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
        root->Add(m_footer, 0, wxEXPAND | wxTOP | wxBOTTOM, FromDIP(8));
        SetMinSize(FromDIP(wxSize(500, 500)));
        SetSize(FromDIP(wxSize(500, 600)));
        m_scroll->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            relayout();
            event.Skip();
        });
        refresh_state();
        relayout();
        wxGetApp().UpdateDlgDarkUI(this);
        CenterOnParent();
    }

    void refresh_state()
    {
        const bool had_error = m_error->IsShown();
        m_error->Hide();
        wxString counts;
        if (m_catalog.additions())
            counts = wxString::Format("+%u", unsigned(m_catalog.additions()));
        if (m_catalog.removals()) {
            if (!counts.empty())
                counts += " ";
            counts += wxString::Format("-%u", unsigned(m_catalog.removals()));
        }
        if (m_counts->GetLabel() != counts) {
            m_counts->SetLabel(counts);
            m_footer->Layout();
        }
        m_select_all->set_state(m_catalog.selection_state());
        bool any_available = false;
        for (size_t group = 0; group < 2; ++group) {
            m_groups[group]->set_check_state(m_catalog.selection_state(group + 2));
            bool available     = false;
            int  removed_count = 0;
            for (size_t i = 0; i < m_group_indices[group].size(); ++i) {
                const auto& item = m_catalog.recommendations[m_group_indices[group][i]];
                available |= !item.registered();
                const bool removed = item.registered() && std::all_of(item.saved_indices.begin(), item.saved_indices.end(),
                                                                      [this](size_t saved) { return m_catalog.saved[saved].removed; });
                removed_count += removed;
                m_recommendation_grids[group]->set_mark(i, removed ? -1 : item.selected ? 1 : 0);
            }
            m_groups[group]->set_check_enabled(available);
            m_groups[group]->set_info(int(m_catalog.additions(group + 2)), removed_count);
            any_available |= available;
        }
        if (m_select_all->IsEnabled() != any_available)
            m_select_all->Enable(any_available);
        for (size_t i = 0; i < m_catalog.saved.size(); ++i)
            m_saved_grid->set_mark(i, m_catalog.saved[i].removed ? -1 : 0);
        if (m_remove_all) {
            const bool can_remove = m_catalog.removals() < m_catalog.saved.size();
            if (m_remove_all->IsEnabled() != can_remove)
                m_remove_all->Enable(can_remove);
        }
        // Swatch marks never change geometry. Only removing an error banner
        // requires the expensive parent layout.
        if (had_error)
            Layout();
    }

    void relayout()
    {
        for (auto* group : m_groups)
            if (group)
                group->Layout();
        if (m_saved_section)
            m_saved_section->Layout();
        if (m_recommended_section)
            m_recommended_section->Layout();
        m_scroll->Layout();
        m_scroll->FitInside();
    }
    void error(const wxString& message)
    {
        m_error->SetLabel(message);
        m_error->Wrap(GetClientSize().x - FromDIP(24));
        m_error->Show();
        Layout();
    }
    void cancel(const char* method)
    {
        track_manage("rollback");
        track_manage("close", {{"method", method}});
        EndModal(wxID_CANCEL);
    }
    void confirm()
    {
        if (m_catalog.exceeds_limit()) {
            error(_L("The limit is 256 mixed filaments. Deselect some recommendations before confirming."));
            track_manage("limit", {{"count", m_catalog.saved.size() - m_catalog.removals() + m_catalog.additions()}});
            return;
        }
        auto* bundle = wxGetApp().preset_bundle;
        auto* plater = wxGetApp().plater();
        if (!bundle || !plater) {
            error(_L("Registration failed. Please try again."));
            return;
        }
        if (m_catalog.additions() || m_catalog.removals()) {
            try {
                auto       candidate   = m_catalog.prepare(bundle->mixed_filaments);
                const auto definitions = candidate.mixed_filament_definitions(m_context.num_physical);
                // Validate through the canonical 3MF codec before changing any project state.
                serialize_mixed_filaments_json(definitions, m_context.num_physical);
                DynamicPrintConfig config = bundle->project_config;
                config.set_key_value("mixed_filament_definitions", new ConfigOptionString(candidate.serialize_custom_entries()));
                std::vector<unsigned int> remap(bundle->mixed_filaments.total_filaments(m_context.num_physical) + 1, 0);
                for (size_t id = 1; id <= m_context.num_physical; ++id)
                    remap[id] = unsigned(id);
                for (const auto& saved : m_catalog.saved)
                    if (saved.filament_id && !saved.removed)
                        remap[saved.filament_id] =
                            candidate.filament_id_from_stable_id(saved.definition.identity.stable_id, m_context.num_physical).value_or(0);
                // All allocation/validation above is staged. Publish once, then use
                // the normal filament-change path to remap painting and assignments.
                bundle->set_filament_id_remap(remap);
                bundle->project_config.swap(config);
                std::swap(bundle->mixed_filaments, candidate);
            } catch (const std::exception& exception) {
                BOOST_LOG_TRIVIAL(error) << "Mixed filament registration failed: " << exception.what();
                error(_L("Registration failed. Please try again."));
                return;
            }
            plater->post_slice_state_change_update();
            plater->on_filaments_change(m_context.num_physical, true);
            plater->update_project_dirty_from_presets();
        }
        track_manage("confirm", {{"added", m_catalog.additions()}, {"removed", m_catalog.removals()}});
        track_manage("close", {{"method", "confirm"}});
        EndModal(wxID_OK);
    }
    void on_dpi_changed(const wxRect&) override
    {
        SetMinSize(FromDIP(wxSize(500, 500)));
        m_confirm->Rescale();
        m_cancel->Rescale();
        if (m_remove_all)
            m_remove_all->Rescale();
        for (auto* grid : m_grids)
            grid->rescale();
        m_select_all->rescale();
        for (auto* group : m_groups)
            group->rescale();
        relayout();
    }
};
} // namespace

void show_mixed_filament_manager(wxWindow* parent)
{
    track_manage("entry");
    try {
        auto* bundle = wxGetApp().preset_bundle;
        if (!bundle)
            throw std::runtime_error("Missing preset bundle");
        const auto* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        if (!colors || colors->values.size() < 2) {
            show_info(parent, _L("Please configure at least two filaments first."), _L("Manage Mixed Filaments"));
            return;
        }
        if (colors->values.size() > MixedFilamentRecommendations::MAX_PHYSICAL_FILAMENTS) {
            track_manage("entry_blocked", {{"physical_count", colors->values.size()}});
            show_info(parent, _L("Too many physical filaments. Reduce the list to 16 or fewer to use this feature."),
                      _L("Manage Mixed Filaments"));
            return;
        }
        auto context         = build_mixed_filament_display_context(colors->values);
        context.color_engine = MixedFilamentColorEngine::FullSpectrumKSPairResidual;
        std::vector<std::string> types(context.num_physical);
        if (bundle->filament_presets.size() != context.num_physical)
            throw std::runtime_error("Incomplete filament presets");
        for (size_t i = 0; i < types.size(); ++i) {
            const auto* preset = bundle->filaments.find_preset(bundle->filament_presets[i]);
            if (!preset)
                throw std::runtime_error("Missing filament preset");
            const auto* type = preset->config.option<ConfigOptionStrings>("filament_type");
            if (type && !type->values.empty())
                types[i] = type->values.front();
        }
        MixedFilamentManageDialog dialog(wxGetTopLevelParent(parent), std::move(context), std::move(types));
        dialog.ShowModal();
    } catch (const std::exception& exception) {
        BOOST_LOG_TRIVIAL(error) << "Mixed filament loading failed: " << exception.what();
        show_error(parent, _L("Mixed filament loading failed. Please try again."));
    }
}

} // namespace Slic3r::GUI
