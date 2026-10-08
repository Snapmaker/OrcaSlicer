#include "libslic3r/Config.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"
// cut_target_label() - how "Copy cut to..." names an object in its submenu.
#include "libslic3r/CutRecipe.hpp"
#include "libslic3r/QuadRemesh.hpp"
// Ultra: voxel_ops_available() gates the "Round all edges" entry below.
#include "libslic3r/MeshRepair.hpp"

#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "Plater.hpp"
#include "ObjectDataViewModel.hpp"

#include "OptionsGroup.hpp"
#include "GLCanvas3D.hpp"
#include "Selection.hpp"
#include "format.hpp"
//BBS: add partplate related logic
#include "PartPlate.hpp"
#include "Gizmos/GLGizmoEmboss.hpp"
#include "Gizmos/GLGizmoSVG.hpp"

#include <boost/algorithm/string.hpp>
#include "slic3r/GUI/Tab.hpp"
#include "libslic3r/SupportSet.hpp"
#include "SupportGroupsDialog.hpp"
#include <algorithm>
#include <iterator>
#include "slic3r/Utils/FixModelByWin10.hpp"
#include "ParamsPanel.hpp"
#include "MixedFilamentBadge.hpp"
#include "MsgDialog.hpp"
#include "wx/utils.h"

namespace Slic3r
{
namespace GUI
{

static PrinterTechnology printer_technology()
{
    return wxGetApp().preset_bundle->printers.get_selected_preset().printer_technology();
}

static int physical_filaments_count()
{
    return std::max(wxGetApp().filaments_cnt(), 0);
}

static int filaments_count()
{
    if (wxGetApp().preset_bundle == nullptr)
        return 0;
    const int physical = physical_filaments_count();
    const auto &mixed_mgr = wxGetApp().preset_bundle->mixed_filaments;
    return static_cast<int>(mixed_mgr.total_filaments(size_t(physical)));
}

static std::vector<unsigned int> ui_ordered_filament_ids()
{
    if (wxGetApp().plater() == nullptr)
        return {};
    return wxGetApp().plater()->sidebar().get_ui_ordered_filament_ids();
}

static wxString filament_menu_item_name(const int filament_id_1based, const int display_filament_id_1based)
{
    if (filament_id_1based <= 0)
        return _L("Default");

    if (wxGetApp().preset_bundle == nullptr)
        return wxString::Format(_L("Filament %d"), filament_id_1based);

    const int physical = physical_filaments_count();
    if (filament_id_1based <= physical) {
        const size_t preset_idx = size_t(filament_id_1based - 1);
        const auto  &filament_presets = wxGetApp().preset_bundle->filament_presets;
        if (preset_idx < filament_presets.size()) {
            auto preset = wxGetApp().preset_bundle->filaments.find_preset(filament_presets[preset_idx]);
            if (preset != nullptr)
                return from_u8(preset->label(false));
        }
        return wxString::Format(_L("Filament %d"), filament_id_1based);
    }

    return wxString::Format(_L("Mixed Filament %d"), display_filament_id_1based);
}

// Ultra (support groups): the third clause used to test category == "Support material", a
// string no option in this tree carries - every support key's category is "Support" - so the
// clause was a dead no-op and the real gate on what a part may carry was
// SettingsFactory::get_options(). It is now key-aware: a part may carry the curated
// part-level support keys and nothing else from the "Support" category.
//
// A caller with no key in hand (create_freq_settings_popupmenu, which offers whole bundles)
// therefore still blocks the entire category on a part, which is exactly what the plan's risk
// R2.3 asks for. docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2.2.
static bool is_improper_category(const std::string& category, const int filaments_cnt,
                                 const bool is_object_settings = true,
                                 const std::string& opt_key = std::string())
{
    return  category.empty() ||
        (filaments_cnt == 1 && (category == "Extruders" || category == "Wipe options")) ||
        (!is_object_settings && category == "Support" && !SettingsFactory::is_part_support_key(opt_key));
}


//-------------------------------------
//            SettingsFactory
//-------------------------------------

// pt_FFF
static SettingsFactory::Bundle FREQ_SETTINGS_BUNDLE_FFF =
{
    //BBS
    { L("Quality"), { "layer_height" } },
    { L("Shell"), { "wall_loops", "top_shell_layers", "bottom_shell_layers"} },
    { L("Infill")               , { "sparse_infill_density", "sparse_infill_pattern" } },
    // BBS
    { L("Support")     , { "enable_support", "support_type", "support_threshold_angle", "support_threshold_overlap",
                                    "support_base_pattern", "support_on_build_plate_only","support_critical_regions_only",
                                    "support_remove_small_overhang",
                                    "support_base_pattern_spacing", "support_expansion"}},
    //BBS
    { L("Flush options")         , { "flush_into_infill", "flush_into_objects", "flush_into_support"} }
};

// pt_SLA
static SettingsFactory::Bundle FREQ_SETTINGS_BUNDLE_SLA =
{
    // BBS: remove SLA freq settings
};

//BBS: add setting data for table
std::map<std::string, std::vector<SimpleSettingData>>  SettingsFactory::OBJECT_CATEGORY_SETTINGS=
{
    { L("Quality"), {{"layer_height", "",1},
                    //{"initial_layer_print_height", "",2},
                    {"seam_position", "",2}, {"seam_prefer_part_joints", "",2},
                    {"slice_closing_radius", "",3}, {"resolution", "",4},
                    {"xy_hole_compensation", "",5}, {"xy_contour_compensation", "",6}, {"elefant_foot_compensation", "",7},
                    {"make_overhang_printable_angle","", 8},{"make_overhang_printable_hole_size","",9}, {"wall_sequence","",10},
                    {"precise_z_height", "",10},
                    {"zaa_enabled", "",11}, {"zaa_minimize_perimeter_height", "",12},
                    {"zaa_dont_alternate_fill_direction", "",13}, {"zaa_min_z", "",14},
                    {"zaa_speed_scaling", "",15}

                    }},
    { L("Support"), {{"brim_type", "",1},{"brim_width", "",2},{"brim_object_gap", "",3},
                    {"enable_support", "",4},{"support_type", "",5},{"support_threshold_angle", "",6}, {"support_threshold_overlap", "",6}, {"support_on_build_plate_only", "",7},
                    {"support_filament", "",8},{"support_interface_filament", "",9},{"support_expansion", "",24},{"support_style", "",25},
                    {"tree_support_brim_width", "",26}, {"tree_support_branch_angle", "",10},{"tree_support_branch_angle_organic","",10}, {"tree_support_wall_count", "",11},{"tree_support_branch_diameter_angle", "",11},//tree support
                    {"support_top_z_distance", "",13},{"support_bottom_z_distance", "",12},{"support_base_pattern", "",14},{"support_base_pattern_spacing", "",15},
                    {"support_interface_top_layers", "",16},{"support_interface_bottom_layers", "",17},{"support_interface_spacing", "",18},{"support_bottom_interface_spacing", "",19},
                    {"support_object_xy_distance", "",20}, {"bridge_no_support", "",21},{"max_bridge_length", "",22},{"support_critical_regions_only", "",23},{"support_remove_small_overhang","",27},
                    {"support_object_first_layer_gap","",28},
                    {"stabilizer_supports","",29},{"stabilizer_ring_spacing","",30},{"stabilizer_points_per_ring","",31},
                    {"stabilizer_tip_diameter","",32},{"stabilizer_tip_gap","",32},{"stabilizer_pillar_diameter","",33},{"stabilizer_max_island_width","",34},
                    {"stabilizer_pillar_base_diameter","",35},{"stabilizer_bracing","",36},{"stabilizer_brace_max_unbraced","",37},{"stabilizer_brace_max_span","",38},{"stabilizer_column_shape","",39},{"stabilizer_column_width","",40},
                    {"stabilizer_column_length","",41},{"stabilizer_column_min_height","",42},{"stabilizer_wall_loops","",43},{"stabilizer_infill_density","",44},{"stabilizer_infill_pattern","",45}
                            }},
    { L("Speed"), {{"support_speed", "",12}, {"support_interface_speed", "",13}
                    }}
};

std::map<std::string, std::vector<SimpleSettingData>>  SettingsFactory::PART_CATEGORY_SETTINGS=
{
    { L("Quality"), {{"ironing_type", "",8},{"ironing_flow", "",9},{"ironing_spacing", "",10},{"ironing_inset", "", 11},{"bridge_flow", "",11},{"make_overhang_printable", "",11},{"bridge_density", "", 1},
                     {"zaa_enabled", "",12}, {"zaa_minimize_perimeter_height", "",13},
                     {"zaa_dont_alternate_fill_direction", "",14}, {"zaa_min_z", "",15},
                     {"zaa_speed_scaling", "",16}
                    }},
    { L("Strength"), {{"wall_loops", "",1},{"top_shell_layers", L("Top Solid Layers"),1},{"top_shell_thickness", L("Top Minimum Shell Thickness"),1},{"top_color_penetration_layers", "",1},{"top_surface_density", L("Top Surface Density"),1},
                    {"bottom_shell_layers", L("Bottom Solid Layers"),1}, {"bottom_shell_thickness", L("Bottom Minimum Shell Thickness"),1},{"bottom_color_penetration_layers", "",1},{"bottom_surface_density", L("Bottom Surface Density"),1},
                    {"sparse_infill_density", "",1},{"sparse_infill_pattern", "",1},{"sparse_infill_filament", "",1},{"lateral_lattice_angle_1", "",1},{"lateral_lattice_angle_2", "",1},{"infill_overhang_angle", "",1},{"infill_anchor", "",1},{"infill_anchor_max", "",1},{"top_surface_pattern", "",1},{"undertop_surface_pattern", "",1},{"bottom_surface_pattern", "",1}, {"internal_solid_infill_pattern", "",1},
                    {"align_infill_direction_to_model", "", 1},
                    {"hollow_interior", "", 1}, {"hollow_shell_thickness", "", 1},
                    {"extra_solid_infills", "", 1},
        {"infill_combination", "",1}, {"infill_combination_max_layer_height", "",1}, {"infill_wall_overlap", "",1},{"top_bottom_infill_wall_overlap", "",1}, {"solid_infill_direction", "",1}, {"infill_direction", "",1}, {"bridge_angle", "",1}, {"internal_bridge_angle", "",1}, {"minimum_sparse_infill_area", "",1}
                    }},
    { L("Speed"), {{"outer_wall_speed", "",1},{"inner_wall_speed", "",2},{"sparse_infill_speed", "",3},{"top_surface_speed", "",4}, {"internal_solid_infill_speed", "",5},
                    {"enable_overhang_speed", "",6}, {"overhang_1_4_speed", "",7}, {"overhang_2_4_speed", "",8}, {"overhang_3_4_speed", "",9}, {"overhang_4_4_speed", "",10},
                    {"bridge_speed", "",11}, {"gap_infill_speed", "",12}, {"internal_bridge_speed", "", 13}
                    }},
    // Ultra (support groups): the curated part-level support keys, so a part's settings node
    // renders them in the same table style as Quality/Strength/Speed. An OBJECT takes its
    // Support rows from OBJECT_CATEGORY_SETTINGS instead - get_visible_options() strips these
    // back out for objects, or the shared rows would appear twice.
    // docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2.2, 3.5.
    { L("Support"), {{"support_style", "",1},{"support_threshold_angle", "",2},{"support_top_z_distance", "",3},
                    {"support_interface_filament", "",4},
                    {"support_interface_top_layers", "",5},{"support_interface_bottom_layers", "",6},
                    {"support_interface_spacing", "",7},{"support_bottom_interface_spacing", "",8},
                    {"support_interface_pattern", "",9},{"support_interface_loop_pattern", "",10},
                    {"support_ironing", "",11},{"support_ironing_pattern", "",12},
                    {"support_ironing_flow", "",13},{"support_ironing_spacing", "",14}
                    }}
};

const std::vector<std::string>& SettingsFactory::part_support_keys()
{
    // One source of truth with PrintObject::support_groups() and the PrintApply predicate, so
    // the menu can never offer a key the resolver ignores.
    return Slic3r::part_support_keys();
}

bool SettingsFactory::is_part_support_key(const std::string& opt_key)
{
    const std::vector<std::string>& keys = SettingsFactory::part_support_keys();
    return std::find(keys.begin(), keys.end(), opt_key) != keys.end();
}

std::string SettingsFactory::part_support_group(const ModelVolume* volume)
{
    if (volume == nullptr || !volume->is_model_part())
        return std::string();
    const ConfigOption* opt = volume->config.option("support_group");
    if (opt == nullptr)
        return std::string();
    // The raw value, not serialize(): the label is shown to the user and put in a menu, and
    // ConfigOptionString::serialize() would C-escape it.
    const ConfigOptionString* str = dynamic_cast<const ConfigOptionString*>(opt);
    return str != nullptr ? str->value : opt->serialize();
}

std::vector<std::string> SettingsFactory::get_options(const bool is_part)
{
    if (printer_technology() == ptSLA) {
        SLAPrintObjectConfig full_sla_config;
        auto options = full_sla_config.keys();
        options.erase(find(options.begin(), options.end(), "layer_height"));
        return options;
    }

    PrintRegionConfig reg_config;
    auto options = reg_config.keys();
    if (!is_part) {
        PrintObjectConfig obj_config;
        std::vector<std::string> obj_options = obj_config.keys();
        options.insert(options.end(), obj_options.begin(), obj_options.end());
    } else {
        // Ultra (support groups): a part may also carry the curated part-level support keys.
        // They are PrintObjectConfig keys, so apply_to_print_region_config still drops them on
        // the way to a PrintRegionConfig - PrintObject::support_groups() reads them off the
        // volume config directly instead.
        // docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2.2, 3.5.
        const std::vector<std::string>& sup = SettingsFactory::part_support_keys();
        options.insert(options.end(), sup.begin(), sup.end());
    }
    return options;
}

std::vector<SimpleSettingData> SettingsFactory::get_visible_options(const std::string& category, const bool is_part)
{
    /*t_config_option_keys options = {
        //Quality
        "wall_infill_order", "ironing_type", "inner_wall_line_width", "outer_wall_line_width", "top_surface_line_width",
        //Shell
        "wall_loops", "top_shell_layers", "bottom_shell_layers", "top_shell_thickness", "bottom_shell_thickness",
        //Infill
        "sparse_infill_density", "sparse_infill_pattern", "top_surface_pattern", "undertop_surface_pattern", "bottom_surface_pattern", "infill_combination", "infill_direction", "infill_wall_overlap",
        //speed
        "inner_wall_speed", "outer_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed", "gap_infill_speed"
        };

    t_config_option_keys object_options = {
        //Quality
        "layer_height", "initial_layer_print_height", "adaptive_layer_height", "seam_position", "seam_prefer_part_joints", "xy_hole_compensation", "xy_contour_compensation", "elefant_foot_compensation", "support_line_width",
        //Support
        "enable_support", "support_type", "support_threshold_angle", "support_threshold_overlap", "support_on_build_plate_only", "support_critical_regions_only", "enforce_support_layers",
        //tree support
        "tree_support_wall_count",
        //support
        "support_top_z_distance", "support_base_pattern", "support_base_pattern_spacing", "support_interface_top_layers", "support_interface_bottom_layers", "support_interface_spacing", "support_bottom_interface_spacing", "support_object_xy_distance",
        "support_object_first_layer_gap",
        //adhesion
        "brim_type", "brim_width", "brim_object_gap", "raft_layers"
        };*/
    std::vector<SimpleSettingData> options;
    std::map<std::string, std::vector<SimpleSettingData>>::iterator it;

    it = PART_CATEGORY_SETTINGS.find(category);
    if (it != PART_CATEGORY_SETTINGS.end())
    {
        options = PART_CATEGORY_SETTINGS[category];
    }

    if (!is_part) {
        // Ultra (support groups): PART_CATEGORY_SETTINGS carries the curated support keys so a
        // PART's settings table can show them. For an OBJECT the authoritative list is
        // OBJECT_CATEGORY_SETTINGS, which already holds most of them - drop ours, or the object
        // table would show the shared rows twice.
        options.erase(std::remove_if(options.begin(), options.end(), [](const SimpleSettingData& s) {
            return SettingsFactory::is_part_support_key(s.name); }), options.end());
        it = OBJECT_CATEGORY_SETTINGS.find(category);
        if (it != OBJECT_CATEGORY_SETTINGS.end())
            options.insert(options.end(), OBJECT_CATEGORY_SETTINGS[category].begin(), OBJECT_CATEGORY_SETTINGS[category].end());
    }

    auto sort_func = [](SimpleSettingData& setting1, SimpleSettingData& setting2) {
        return (setting1.priority < setting2.priority);
    };
    std::sort(options.begin(), options.end(), sort_func);
    return options;
}

std::map<std::string, std::vector<SimpleSettingData>> SettingsFactory::get_all_visible_options(const bool is_part)
{
    std::map<std::string, std::vector<SimpleSettingData>> option_maps;
    std::map<std::string, std::vector<SimpleSettingData>>::iterator it1, it2;

    option_maps = PART_CATEGORY_SETTINGS;
    if (!is_part) {
        // Ultra (support groups): see get_visible_options(). Dropping the part-level support
        // rows leaves the "Support" entry empty; erasing it too keeps the merge below on the
        // exact path it took before this feature existed, object row order included.
        for (auto it = option_maps.begin(); it != option_maps.end(); ) {
            std::vector<SimpleSettingData>& opts = it->second;
            opts.erase(std::remove_if(opts.begin(), opts.end(), [](const SimpleSettingData& s) {
                return SettingsFactory::is_part_support_key(s.name); }), opts.end());
            it = opts.empty() ? option_maps.erase(it) : std::next(it);
        }
        for (it1 = OBJECT_CATEGORY_SETTINGS.begin(); it1 != OBJECT_CATEGORY_SETTINGS.end(); it1++)
        {
            std::string category = it1->first;
            it2 = PART_CATEGORY_SETTINGS.find(category);
            if (it2 != PART_CATEGORY_SETTINGS.end())
            {
                std::vector<SimpleSettingData>& options = option_maps[category];
                options.insert(options.end(), it1->second.begin(), it1->second.end());

                auto sort_func = [](SimpleSettingData& setting1, SimpleSettingData& setting2) {
                    return (setting1.priority < setting2.priority);
                };
                std::sort(options.begin(), options.end(), sort_func);
            }
            else {
                option_maps.insert(*it1);
            }
        }
    }

    return option_maps;
}


SettingsFactory::Bundle SettingsFactory::get_bundle(const DynamicPrintConfig* config, bool is_object_settings, bool is_layer_settings/* = false*/)
{
    auto opt_keys = config->keys();
    if (opt_keys.empty())
        return Bundle();

    // update options list according to print technology
    auto full_current_opts = get_options(!is_object_settings);
    if (is_layer_settings)
        full_current_opts.push_back("layer_height");
    for (int i = opt_keys.size() - 1; i >= 0; --i)
        if (find(full_current_opts.begin(), full_current_opts.end(), opt_keys[i]) == full_current_opts.end())
            opt_keys.erase(opt_keys.begin() + i);

    if (opt_keys.empty())
        return Bundle();

    const int filaments_cnt = wxGetApp().filaments_cnt();

    Bundle bundle;
    for (auto& opt_key : opt_keys)
    {
        auto category = config->def()->get(opt_key)->category;
        if (is_improper_category(category, filaments_cnt, is_object_settings, opt_key))
            continue;

        std::vector< std::string > new_category;

        auto& cat_opt = bundle.find(category) == bundle.end() ? new_category : bundle.at(category);
        cat_opt.push_back(opt_key);
        if (cat_opt.size() == 1)
            bundle[category] = cat_opt;
    }

    return bundle;
}

// Fill CategoryItem
std::map<std::string, std::string> SettingsFactory::CATEGORY_ICON =
{
//    settings category name      related bitmap name
    // ptFFF
    { L("Quality")              , "blank2"      },
    { L("Shell")                , "blank_14"    },
    { L("Infill")               , "blank_14"    },
    { L("Ironing")              , "blank_14"    },
    { L("Fuzzy Skin")           , "menu_fuzzy_skin"  },
    { L("Support")              , "support"     },
    { L("Speed")                , "blank_14"    },
    { L("Extruders")            , "blank_14"    },
    { L("Extrusion Width")      , "blank_14"    },
    { L("Wipe options")         , "blank_14"    },
    { L("Bed adhesion")         , "blank_14"    },
//  { L("Speed > Acceleration") , "time"        },
    { L("Advanced")             , "blank_14"    },
    // BBS: remove SLA categories
};

wxBitmap SettingsFactory::get_category_bitmap(const std::string& category_name, bool menu_bmp)
{
    if (CATEGORY_ICON.find(category_name) == CATEGORY_ICON.end())
        return wxNullBitmap;
    return create_scaled_bitmap(CATEGORY_ICON.at(category_name));
}


//-------------------------------------
//            MenuFactory
//-------------------------------------

// Note: id accords to type of the sub-object (adding volume), so sequence of the menu items is important
static const constexpr std::array<std::pair<const char *, const char *>, 11> ADD_VOLUME_MENU_ITEMS = {{
    //       menu_item Name              menu_item bitmap name
        {L("Add part"),              "menu_add_part" },           // ~ModelVolumeType::MODEL_PART
        {L("Add negative part"),     "menu_add_negative" },       // ~ModelVolumeType::NEGATIVE_VOLUME
        {L("Add modifier"),          "menu_add_modifier"},         // ~ModelVolumeType::PARAMETER_MODIFIER
        {L("Add support blocker"),   "menu_support_blocker"},     // ~ModelVolumeType::SUPPORT_BLOCKER
        {L("Add support enforcer"),  "menu_support_enforcer"},     // ~ModelVolumeType::SUPPORT_ENFORCER
        // Precise Seam modifiers (all 6 subtypes - tree icons; add menu uses a nested submenu)
        {L("Add precise seam"),      "menu_precise_seam_center"},     // ~ModelVolumeType::PRECISE_SEAM_CENTER
        {L("Add precise seam"),      "menu_precise_seam_left"},       // ~ModelVolumeType::PRECISE_SEAM_LEFT
        {L("Add precise seam"),      "menu_precise_seam_right"},      // ~ModelVolumeType::PRECISE_SEAM_RIGHT
        {L("Add precise seam"),      "menu_precise_seam_enforced"},   // ~ModelVolumeType::PRECISE_SEAM_ENFORCED
        {L("Add precise seam"),      "menu_precise_seam_blocked"},    // ~ModelVolumeType::PRECISE_SEAM_BLOCKED
        {L("Add precise seam"),      "menu_precise_seam_neutral"},    // ~ModelVolumeType::PRECISE_SEAM_NEUTRAL
}};

// "Seam ..." prefix disambiguates from other i18n contexts (extruder Left/Right, "Center on bed", etc.)
static const std::array<std::pair<const char*, ModelVolumeType>, 6> PRECISE_SEAM_MENU_TYPES = {{
    {L("Seam Center"),   ModelVolumeType::PRECISE_SEAM_CENTER},
    {L("Seam Left"),     ModelVolumeType::PRECISE_SEAM_LEFT},
    {L("Seam Right"),    ModelVolumeType::PRECISE_SEAM_RIGHT},
    {L("Seam Enforced"), ModelVolumeType::PRECISE_SEAM_ENFORCED},
    {L("Seam Blocked"),  ModelVolumeType::PRECISE_SEAM_BLOCKED},
    {L("Seam Neutral"),  ModelVolumeType::PRECISE_SEAM_NEUTRAL},
}};

// Note: id accords to type of the sub-object (adding volume), so sequence of the menu items is important
static const constexpr std::array<std::pair<const char *, const char *>, 3> TEXT_VOLUME_ICONS {{
//       menu_item Name              menu_item bitmap name
        {L("Add text"),             "add_text_part"},        // ~ModelVolumeType::MODEL_PART
        {L("Add negative text"),    "add_text_negative" },   // ~ModelVolumeType::NEGATIVE_VOLUME
        {L("Add text modifier"),    "add_text_modifier"},    // ~ModelVolumeType::PARAMETER_MODIFIER
}};
// Note: id accords to type of the sub-object (adding volume), so sequence of the menu items is important
static const constexpr std::array<std::pair<const char *, const char *>, 3> SVG_VOLUME_ICONS{{
    {L("Add SVG part"),     "svg_part"},     // ~ModelVolumeType::MODEL_PART
    {L("Add negative SVG"), "svg_negative"}, // ~ModelVolumeType::NEGATIVE_VOLUME
    {L("Add SVG modifier"), "svg_modifier"}, // ~ModelVolumeType::PARAMETER_MODIFIER
}};

static Plater* plater()
{
    return wxGetApp().plater();
}

static ObjectList* obj_list()
{
    return wxGetApp().obj_list();
}

static ObjectDataViewModel* list_model()
{
    return wxGetApp().obj_list()->GetModel();
}

static const Selection& get_selection()
{
    return plater()->get_current_canvas3D(true)->get_selection();
}

//				  category ->		vector 			 ( option	;  label )
typedef std::map< std::string, std::vector< std::pair<std::string, std::string> > > FullSettingsHierarchy;
static void get_full_settings_hierarchy(FullSettingsHierarchy& settings_menu, const bool is_part)
{
    auto options = SettingsFactory::get_options(is_part);

    const int filaments_cnt = filaments_count();

    DynamicPrintConfig config;
    for (auto& option : options)
    {
        auto const opt = config.def()->get(option);
        auto category = opt->category;
        if (is_improper_category(category, filaments_cnt, !is_part, option))
            continue;

        const std::string& label = !opt->full_label.empty() ? opt->full_label : opt->label;
        std::pair<std::string, std::string> option_label(option, label);
        std::vector< std::pair<std::string, std::string> > new_category;
        auto& cat_opt_label = settings_menu.find(category) == settings_menu.end() ? new_category : settings_menu.at(category);
        cat_opt_label.push_back(option_label);
        if (cat_opt_label.size() == 1)
            settings_menu[category] = cat_opt_label;
    }
}

static wxMenu* create_settings_popupmenu(wxMenu* parent_menu, const bool is_object_settings, wxDataViewItem item/*, ModelConfig& config*/)
{
    wxMenu* menu = new wxMenu;

    FullSettingsHierarchy categories;
    get_full_settings_hierarchy(categories, !is_object_settings);

    auto get_selected_options_for_category = [categories, item](const wxString& category_name) {
        wxArrayString names;
        wxArrayInt selections;

        std::vector< std::pair<std::string, bool> > category_options;
        for (auto& cat : categories) {
            if (_(cat.first) == category_name) {
                ModelConfig& config = obj_list()->get_item_config(item);
                auto opt_keys = config.keys();

                int sel = 0;
                for (const std::pair<std::string, std::string>& pair : cat.second) {
                    names.Add(_(pair.second));
                    if (find(opt_keys.begin(), opt_keys.end(), pair.first) != opt_keys.end())
                        selections.Add(sel);
                    sel++;
                    category_options.push_back(std::make_pair(pair.first, false));
                }
                break;
            }
        }

        if (!category_options.empty() &&
            wxGetSelectedChoices(selections, _L("Select settings"), category_name, names) != -1) {
            for (auto sel : selections)
                category_options[sel].second = true;
        }
        return category_options;
    };

    for (auto cat : categories) {
        append_menu_item(menu, wxID_ANY, _(cat.first), "",
                         [menu, item, get_selected_options_for_category](wxCommandEvent& event) {
                            std::vector< std::pair<std::string, bool> > category_options = get_selected_options_for_category(menu->GetLabel(event.GetId()));
                            obj_list()->add_category_to_settings_from_selection(category_options, item);
                         }, SettingsFactory::get_category_bitmap(cat.first), parent_menu,
                         []() { return true; }, plater());
    }

    return menu;
}

// NOTE (Ultra, support groups - the plan's risk R2.3): this whole path is dead - its only call
// site is commented out below. If anyone re-enables it, it MUST filter the bundle through
// SettingsFactory::get_options(is_part) first: add_category_to_settings_from_frequent()
// (GUI_ObjectList.cpp) writes whatever it is handed onto the item's config with no legality
// check, so a "Support" bundle offered on a part would put object-only support keys on a volume,
// where nothing reads them. is_improper_category() below blocks the category here because this
// caller has no option key to offer it, which is the safe answer for a part.
static void create_freq_settings_popupmenu(wxMenu* menu, const bool is_object_settings, wxDataViewItem item)
{
    // Add default settings bundles
    const SettingsFactory::Bundle& bundle = printer_technology() == ptFFF ? FREQ_SETTINGS_BUNDLE_FFF : FREQ_SETTINGS_BUNDLE_SLA;

    const int filaments_cnt = filaments_count();

    for (auto& category : bundle) {
        if (is_improper_category(category.first, filaments_cnt, is_object_settings))
            continue;

        append_menu_item(menu, wxID_ANY, _(category.first), "",
            [menu, item, is_object_settings, bundle](wxCommandEvent& event) {
                    wxString category_name = menu->GetLabel(event.GetId());
                    std::vector<std::string> options;
                    for (auto& category : bundle)
                        if (category_name == _(category.first)) {
                            options = category.second;
                            break;
                        }
                    if (options.empty())
                        return;
                    // Because of we couldn't edited layer_height for ItVolume from settings list,
                    // correct options according to the selected item type : remove "layer_height" option
                    if (!is_object_settings && category_name == _("Quality")) {
                        const auto layer_height_it = std::find(options.begin(), options.end(), "layer_height");
                        if (layer_height_it != options.end())
                            options.erase(layer_height_it);
                    }

                    obj_list()->add_category_to_settings_from_frequent(options, item);
                },
            SettingsFactory::get_category_bitmap(category.first), menu,
            []() { return true; }, plater());
    }
}

std::vector<wxBitmap> MenuFactory::get_volume_bitmaps()
{
    std::vector<wxBitmap> volume_bmps;
    volume_bmps.reserve(ADD_VOLUME_MENU_ITEMS.size());
    for (const auto& item : ADD_VOLUME_MENU_ITEMS) {
        volume_bmps.push_back(create_scaled_bitmap(item.second));
    }
    return volume_bmps;
}

std::vector<wxBitmap> MenuFactory::get_text_volume_bitmaps()
{
    std::vector<wxBitmap> volume_bmps;
    volume_bmps.reserve(TEXT_VOLUME_ICONS.size());
    for (const auto& item : TEXT_VOLUME_ICONS)
        volume_bmps.push_back(create_scaled_bitmap(item.second));
    return volume_bmps;
}

std::vector<wxBitmap> MenuFactory::get_svg_volume_bitmaps()
{
    std::vector<wxBitmap> volume_bmps;
    volume_bmps.reserve(SVG_VOLUME_ICONS.size());
    for (const auto &item : SVG_VOLUME_ICONS)
        volume_bmps.push_back(create_scaled_bitmap(item.second));
    return volume_bmps;
}

void MenuFactory::append_menu_item_set_visible(wxMenu* menu)
{
    bool has_one_shown = false;
    const Selection& selection = plater()->canvas3D()->get_selection();
    for (unsigned int i : selection.get_volume_idxs()) {
        has_one_shown |= selection.get_volume(i)->visible;
    }

    append_menu_item(menu, wxID_ANY, has_one_shown ?_L("Hide") : _L("Show"), "",
        [has_one_shown](wxCommandEvent&) { plater()->set_selected_visible(!has_one_shown); }, "", nullptr,
        []() { return true; }, m_parent);
}

void MenuFactory::append_menu_item_delete(wxMenu* menu)
{
#ifdef __WINDOWS__
    append_menu_item(menu, wxID_ANY, _L("Delete") + "\t" + _L("Del"), _L("Delete the selected object"),
        [](wxCommandEvent&) { plater()->remove_selected(); }, "menu_delete", nullptr,
        []() { return plater()->can_delete(); }, m_parent);
#else
    append_menu_item(menu, wxID_ANY, _L("Delete") + "\t" + _L("Backspace"), _L("Delete the selected object"),
        [](wxCommandEvent&) { plater()->remove_selected(); }, "", nullptr,
        []() { return plater()->can_delete(); }, m_parent);
#endif
}

wxMenu* MenuFactory::append_submenu_add_generic(wxMenu* menu, ModelVolumeType type) {
    auto sub_menu = new wxMenu;

    if (type != ModelVolumeType::INVALID) {
        append_menu_item(sub_menu, wxID_ANY, _L("Load..."), "",
            [type](wxCommandEvent&) { obj_list()->load_subobject(type); }, "menu_load", menu);
        sub_menu->AppendSeparator();
    }

    append_menu_item(sub_menu, wxID_ANY, _L("Cube"), "",
        [type](wxCommandEvent&) { obj_list()->load_generic_subobject(L("Cube") ,type); },"menu_obj_cube", menu);

    append_menu_item(sub_menu, wxID_ANY, _L("Cylinder"), "",
        [type](wxCommandEvent&) { obj_list()->load_generic_subobject(L("Cylinder"), type); },"menu_obj_cylinder", menu);

    append_menu_item(sub_menu, wxID_ANY, _L("Sphere"), "",
        [type](wxCommandEvent&) { obj_list()->load_generic_subobject(L("Sphere"), type); },"menu_obj_sphere", menu);

    append_menu_item(sub_menu, wxID_ANY, _L("Cone"), "",
        [type](wxCommandEvent&) { obj_list()->load_generic_subobject(L("Cone"), type); },"menu_obj_cone", menu);

    append_menu_item(sub_menu, wxID_ANY, _L("Disc"), "",
        [type](wxCommandEvent&) { obj_list()->load_generic_subobject(L("Disc"), type); },"menu_obj_disc", menu);

    append_menu_item(sub_menu, wxID_ANY, _L("Torus"), "",
        [type](wxCommandEvent&) { obj_list()->load_generic_subobject(L("Torus"), type); },"menu_obj_torus", menu);


    append_menu_item_add_text(sub_menu, type);
    append_menu_item_add_svg(sub_menu, type);
    append_menu_item_add_image(sub_menu, type);
    append_menu_item_add_code(sub_menu, type);
    append_menu_item_add_shape(sub_menu, type);

    return sub_menu;
}

// Orca: add submenu for adding handy models
wxMenu* MenuFactory::append_submenu_add_handy_model(wxMenu* menu, ModelVolumeType type) {
    auto sub_menu = new wxMenu;

    for (auto &item : {L("Orca Cube"), L("Orca Tolerance Test"), L("3DBenchy"), L("Autodesk FDM Test"),
                       L("Voron Cube"), L("Stanford Bunny"), L("Orca String Hell") }) {
        append_menu_item(
            sub_menu, wxID_ANY, _(item), "",
            [type, item](wxCommandEvent&) {
                std::vector<boost::filesystem::path> input_files;
                bool                                 is_stringhell = false;
                std::string                          file_name     = item;
                if (file_name == L("Orca Cube"))
                    file_name = "OrcaCube_v2.3mf";
                else if (file_name == L("Orca Tolerance Test"))
                    file_name = "OrcaToleranceTest.stl";
                else if (file_name == L("3DBenchy"))
                    file_name = "3DBenchy.3mf";
                else if (file_name == L("Autodesk FDM Test"))
                    file_name = "ksr_fdmtest_v4.3mf";
                else if (file_name == L("Voron Cube"))
                    file_name = "Voron_Design_Cube_v7.3mf";
                else if (file_name == L("Stanford Bunny"))
                    file_name = "Stanford_Bunny.3mf";
                else if (file_name == L("Orca String Hell")) {
                    file_name     = "Orca_stringhell.3mf";
                    is_stringhell = true;
                } else
                    return;
                input_files.push_back((boost::filesystem::path(Slic3r::resources_dir()) / "handy_models" / file_name));
                Plater* currentPlater = plater();
                if (currentPlater == nullptr)
                {
                    return;
                }

                const std::vector<size_t> loadedObjectIndexes = currentPlater->load_files(input_files, LoadStrategy::LoadModel);
                Model& model = currentPlater->model();
                ModelObjectPtrs loadedObjects;
                loadedObjects.reserve(loadedObjectIndexes.size());
                for (const size_t objectIndex : loadedObjectIndexes)
                {
                    if (objectIndex < model.objects.size())
                    {
                        loadedObjects.push_back(model.objects[objectIndex]);
                    }
                }
                model.InitializeAssemblyPositions(loadedObjects);

                // Suggest to change settings for stringhell
                // This serves as mini tutorial for new users
                if (is_stringhell) {
                    wxGetApp().CallAfter([=] {
                        DynamicPrintConfig* m_config = &wxGetApp().preset_bundle->prints.get_edited_preset().config;

                        bool is_only_one_wall_top  = m_config->opt_bool("only_one_wall_top");
                        auto min_width_top_surface = m_config->option<ConfigOptionFloatOrPercent>("min_width_top_surface")->value;
                        if (is_only_one_wall_top && min_width_top_surface > 0) {
                            wxString msg_text = _L("This model features text embossment on the top surface. For optimal results, it is "
                                                   "advisable to set the 'One Wall Threshold (min_width_top_surface)' "
                                                   "to 0 for the 'Only One Wall on Top Surfaces' to work best.\n"
                                                   "Yes - Change these settings automatically\n"
                                                   "No  - Do not change these settings for me");

                            MessageDialog dialog(wxGetApp().plater(), msg_text, "Suggestion", wxICON_WARNING | wxYES | wxNO);
                            if (dialog.ShowModal() == wxID_YES) {
                                m_config->set_key_value("min_width_top_surface", new ConfigOptionFloatOrPercent(0, false));
                                wxGetApp().get_tab(Preset::TYPE_PRINT)->update_dirty();
                                wxGetApp().get_tab(Preset::TYPE_PRINT)->reload_config();
                            }
                            wxGetApp().plater()->update();
                        }
                    });
                }
            },
            "", menu);
    }


    return sub_menu;
}
wxMenu* MenuFactory::append_submenu_add_custom_model(wxMenu* menu, std::unique_ptr<CustomModelsMenu>& holder)
{
    holder = std::make_unique<CustomModelsMenu>();
    return holder->create(menu, m_parent);
}

// Right-click on an object: put it (with its parts, settings, paint and filament) into the
// Custom Models library.
void MenuFactory::append_menu_item_save_custom_model(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Save to Custom Models") + dots,
        _L("Save the selected object, with its parts, modifiers, settings overrides, painting and filament, to the Custom Models folder"),
        [](wxCommandEvent&) { wxGetApp().CallAfter([]() { save_selection_as_custom_model(); }); }, "", menu,
        []() { return can_save_selection_as_custom_model(); }, m_parent);
}

static void append_menu_itemm_add_(const wxString& name, GLGizmosManager::EType gizmo_type, wxMenu *menu, ModelVolumeType type, bool is_submenu_item) {
    auto add_ = [type, gizmo_type](const wxCommandEvent & /*unnamed*/) {
        const GLCanvas3D *canvas = plater()->canvas3D();
        const GLGizmosManager &mng = canvas->get_gizmos_manager();
        GLGizmoBase *gizmo_base = mng.get_gizmo(gizmo_type);

        ModelVolumeType volume_type = type;
        // no selected object means create new object
        if (volume_type == ModelVolumeType::INVALID)
            volume_type = ModelVolumeType::MODEL_PART;

        auto screen_position = canvas->get_popup_menu_position();
        if (gizmo_type == GLGizmosManager::Emboss) {
            auto emboss = dynamic_cast<GLGizmoEmboss *>(gizmo_base);
            assert(emboss != nullptr);
            if (emboss == nullptr) return;
            if (screen_position.has_value()) {
                emboss->create_volume(volume_type, *screen_position);
            } else {
                emboss->create_volume(volume_type);
            }
        } else if (gizmo_type == GLGizmosManager::Svg) {
            auto svg = dynamic_cast<GLGizmoSVG *>(gizmo_base);
            assert(svg != nullptr);
            if (svg == nullptr) return;
            if (screen_position.has_value()) {
                svg->create_volume(volume_type, *screen_position);
            } else {
                svg->create_volume(volume_type);
            }
        }
    };

    if (type == ModelVolumeType::MODEL_PART || type == ModelVolumeType::NEGATIVE_VOLUME || type == ModelVolumeType::PARAMETER_MODIFIER ||
        type == ModelVolumeType::INVALID // cannot use gizmo without selected object
    ) {
        wxString item_name = wxString(is_submenu_item ? "" : _(ADD_VOLUME_MENU_ITEMS[int(type)].first) + ": ") + name;
        menu->AppendSeparator();
        auto def_icon_name = (gizmo_type == GLGizmosManager::Emboss) ? "menu_obj_text" : "menu_obj_svg";
        const std::string icon_name = is_submenu_item ? def_icon_name : ADD_VOLUME_MENU_ITEMS[int(type)].second;
        append_menu_item(menu, wxID_ANY, item_name, "", add_, icon_name, menu);
    }
}

void MenuFactory::append_menu_item_add_text(wxMenu* menu, ModelVolumeType type, bool is_submenu_item/* = true*/){
    append_menu_itemm_add_(_L("Text"), GLGizmosManager::Emboss, menu, type, is_submenu_item);
}

void MenuFactory::append_menu_item_add_svg(wxMenu *menu, ModelVolumeType type, bool is_submenu_item /* = true*/){
    append_menu_itemm_add_(_L("SVG"), GLGizmosManager::Svg, menu, type, is_submenu_item);
}

void MenuFactory::append_menu_item_add_code(wxMenu *menu, ModelVolumeType type)
{
    if (type != ModelVolumeType::MODEL_PART && type != ModelVolumeType::NEGATIVE_VOLUME &&
        type != ModelVolumeType::PARAMETER_MODIFIER && type != ModelVolumeType::INVALID)
        return;
    auto add_code = [type](const wxCommandEvent & /*unnamed*/) {
        const GLCanvas3D *canvas = plater()->canvas3D();
        auto *svg = dynamic_cast<GLGizmoSVG *>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg));
        assert(svg != nullptr);
        if (svg == nullptr)
            return;
        // INVALID .. no selected object, code is created as new object
        svg->create_code(type, canvas->get_popup_menu_position());
    };
    append_menu_item(menu, wxID_ANY, _L("QR code / Barcode") + dots, _L("Emboss QR code or barcode, parts can use different filaments or depths"),
                     add_code, "menu_obj_svg", menu);
}

void MenuFactory::append_menu_item_add_shape(wxMenu *menu, ModelVolumeType type)
{
    if (type != ModelVolumeType::MODEL_PART && type != ModelVolumeType::NEGATIVE_VOLUME &&
        type != ModelVolumeType::PARAMETER_MODIFIER && type != ModelVolumeType::INVALID)
        return;
    auto add_shape = [type](const wxCommandEvent & /*unnamed*/) {
        const GLCanvas3D *canvas = plater()->canvas3D();
        auto *svg = dynamic_cast<GLGizmoSVG *>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg));
        assert(svg != nullptr);
        if (svg == nullptr)
            return;
        // INVALID .. no selected object, shape is created as new object
        svg->create_shape(type, canvas->get_popup_menu_position());
    };
    append_menu_item(menu, wxID_ANY, _L("Shape") + dots, _L("Emboss a circle, square, star or other simple shape"), add_shape,
                     "menu_obj_svg", menu);
}

void MenuFactory::append_menu_item_add_image(wxMenu *menu, ModelVolumeType type)
{
    if (type != ModelVolumeType::MODEL_PART && type != ModelVolumeType::NEGATIVE_VOLUME &&
        type != ModelVolumeType::PARAMETER_MODIFIER && type != ModelVolumeType::INVALID)
        return;
    auto add_image = [type](const wxCommandEvent & /*unnamed*/) {
        const GLCanvas3D *canvas = plater()->canvas3D();
        auto *svg = dynamic_cast<GLGizmoSVG *>(canvas->get_gizmos_manager().get_gizmo(GLGizmosManager::Svg));
        assert(svg != nullptr);
        if (svg == nullptr)
            return;
        // INVALID .. no selected object, image is created as new object
        svg->create_image(type, canvas->get_popup_menu_position());
    };
    append_menu_item(menu, wxID_ANY, _L("Image") + dots,
                     _L("Trace a PNG or JPG image into shapes, one part per colour, embossed like an SVG"), add_image,
                     "menu_obj_svg", menu);
}

void MenuFactory::append_menu_items_add_volume(wxMenu* menu)
{
    // Update "add" items(delete old & create new)  settings popupmenu
    for (auto& item : ADD_VOLUME_MENU_ITEMS) {
        const wxString item_name = _(item.first);
        int item_id = menu->FindItem(item_name);
        if (item_id != wxNOT_FOUND)
            menu->Destroy(item_id);

        item_id = menu->FindItem(item_name + ": " + _L("Text"));
        if (item_id != wxNOT_FOUND)
            menu->Destroy(item_id);
    }

    for (size_t type = 0; type < ADD_VOLUME_MENU_ITEMS.size(); type++)
    {
        // Skip Precise Seam subtypes except CENTER — they only supply tree icons.
        // CENTER is expanded into a nested "Add precise seam" submenu of the six types.
        if (type >= size_t(int(ModelVolumeType::PRECISE_SEAM_LEFT)) &&
            type <= size_t(int(ModelVolumeType::PRECISE_SEAM_NEUTRAL)))
            continue;

        auto& item = ADD_VOLUME_MENU_ITEMS[type];

        if (type == size_t(int(ModelVolumeType::PRECISE_SEAM_CENTER))) {
            wxMenu* ps_root = new wxMenu;
            for (const auto& ps_type : PRECISE_SEAM_MENU_TYPES) {
                wxMenu* sub_menu = append_submenu_add_generic(ps_root, ps_type.second);
                append_submenu(ps_root, sub_menu, wxID_ANY, _(ps_type.first), "",
                    ADD_VOLUME_MENU_ITEMS[int(ps_type.second)].second,
                    []() { return obj_list()->is_instance_or_object_selected(); }, m_parent);
            }
            append_submenu(menu, ps_root, wxID_ANY, _(item.first), "", "menu_precise_seam_add",
                []() { return obj_list()->is_instance_or_object_selected(); }, m_parent);
            continue;
        }

        wxMenu* sub_menu = append_submenu_add_generic(menu, ModelVolumeType(type));
        append_submenu(menu, sub_menu, wxID_ANY, _(item.first), "", item.second,
            []() { return obj_list()->is_instance_or_object_selected(); }, m_parent);
    }

    append_menu_item_layers_editing(menu);
}

wxMenuItem* MenuFactory::append_menu_item_layers_editing(wxMenu* menu)
{
    return append_menu_item(menu, wxID_ANY, _L("Height range Modifier"), "",
        [](wxCommandEvent&) { obj_list()->layers_editing(); wxGetApp().params_panel()->switch_to_object(); }, "height_range_modifier", menu,
        []() { return obj_list()->is_instance_or_object_selected(); }, m_parent);
}

wxMenuItem* MenuFactory::append_menu_item_settings(wxMenu* menu_)
{
    MenuWithSeparators* menu = dynamic_cast<MenuWithSeparators*>(menu_);

    const wxString menu_name = _L("Add settings");
    // Delete old items from settings popupmenu
    auto settings_id = menu->FindItem(menu_name);
    if (settings_id != wxNOT_FOUND)
        menu->Destroy(settings_id);

    for (auto& it : FREQ_SETTINGS_BUNDLE_FFF)
    {
        settings_id = menu->FindItem(_(it.first));
        if (settings_id != wxNOT_FOUND)
            menu->Destroy(settings_id);
    }
    for (auto& it : FREQ_SETTINGS_BUNDLE_SLA)
    {
        settings_id = menu->FindItem(_(it.first));
        if (settings_id != wxNOT_FOUND)
            menu->Destroy(settings_id);
    }

    menu->DestroySeparators(); // delete old separators

    // If there are selected more then one instance but not all of them
    // don't add settings menu items
    const Selection& selection = get_selection();
    if ((selection.is_multiple_full_instance() && !selection.is_single_full_object()) ||
        selection.is_multiple_volume() || selection.is_mixed()) // more than one volume(part) is selected on the scene
        return nullptr;

    const auto sel_vol = obj_list()->get_selected_model_volume();
    if (sel_vol && sel_vol->type() >= ModelVolumeType::SUPPORT_ENFORCER)
        return nullptr;

    // detect itemm for adding of the setting
    ObjectList* object_list = obj_list();
    ObjectDataViewModel* obj_model = list_model();

    const wxDataViewItem sel_item = // when all instances in object are selected
                                    object_list->GetSelectedItemsCount() > 1 && selection.is_single_full_object() ?
                                    obj_model->GetItemById(selection.get_object_idx()) :
                                    object_list->GetSelection();
    if (!sel_item)
        return nullptr;

    // If we try to add settings for object/part from 3Dscene,
    // for the second try there is selected ItemSettings in ObjectList.
    // So, check if selected item isn't SettingsItem. And get a SettingsItem's parent item, if yes
    wxDataViewItem item = obj_model->GetItemType(sel_item) & itSettings ? obj_model->GetParent(sel_item) : sel_item;
    const ItemType item_type = obj_model->GetItemType(item);
    const bool is_object_settings = !(item_type& itVolume || item_type & itLayer);

    // Add frequently settings
    // BBS remvoe freq setting popupmenu
    // create_freq_settings_popupmenu(menu, is_object_settings, item);

    //menu->SetSecondSeparator();

    // Add full settings list
    auto  menu_item = new wxMenuItem(menu, wxID_ANY, menu_name);
    menu_item->SetBitmap(create_scaled_bitmap("cog"));
    menu_item->SetSubMenu(create_settings_popupmenu(menu, is_object_settings, item));

    return menu->Append(menu_item);
}

wxMenuItem* MenuFactory::append_menu_item_change_type(wxMenu* menu)
{
    return append_menu_item(menu, wxID_ANY, _L("Change type"), "",
        [](wxCommandEvent&) { obj_list()->change_part_type(); }, "", menu,
        []() {
            wxDataViewItem item = obj_list()->GetSelection();
            return item.IsOk() || obj_list()->GetModel()->GetItemType(item) == itVolume;
        }, m_parent);
}

void MenuFactory::append_menu_item_precise_seam_submenu(wxMenu* menu)
{
    wxString submenu_name = _L("Precise Seam Type");

    // Remove existing submenu if present (menu is rebuilt on every right-click)
    const int menu_item_id = menu->FindItem(submenu_name);
    if (menu_item_id != wxNOT_FOUND)
        menu->Destroy(menu_item_id);

    // --- Precondition: ALL selected volumes must be Precise Seam ---
    // Mixed selections (PS + non-PS) are ambiguous for subtype switching: applying a subtype
    // would implicitly convert the non-PS volumes to PS, which is not what the user expects
    // from a subtype picker. For mixed selections the user should first use
    // "Change type" → "Precise Seam" to unify them, then come back to this submenu.
    wxDataViewItemArray sels;
    obj_list()->GetSelections(sels);
    if (sels.IsEmpty())
        return;

    std::vector<ModelVolumeType> selected_ps_types;
    selected_ps_types.reserve(sels.size());

    auto* model = obj_list()->GetModel();
    for (const auto& sel_item : sels) {
        wxDataViewItem vol_item  = sel_item;
        const ItemType type_mask = model->GetItemType(sel_item);
        if (!(type_mask & itVolume)) {
            if ((type_mask & itSettings) && (model->GetItemType(model->GetParent(sel_item)) & itVolume))
                vol_item = model->GetParent(sel_item);
            else
                return;
        }
        const ModelVolumeType vol_type = model->GetVolumeType(vol_item);
        if (!is_precise_seam(vol_type))
            return;
        selected_ps_types.push_back(vol_type);
    }

    wxMenu* ps_menu = new wxMenu();

    for (const auto& ps_type : PRECISE_SEAM_MENU_TYPES) {
        wxString label = _(ps_type.first);

        wxMenuItem* item = append_menu_check_item(ps_menu, wxID_ANY, label, "",
            [ps_type](wxCommandEvent&) {
                obj_list()->set_volume_type(ps_type.second, /*preserve_ps_subtype=*/false);
            },
            ps_menu);

        const bool is_present = std::find(selected_ps_types.begin(), selected_ps_types.end(),
                                          ps_type.second) != selected_ps_types.end();
        if (is_present && item)
            item->Check(true);
    }

    append_submenu(menu, ps_menu, wxID_ANY,
                   submenu_name,
                   _L("Choose precise seam subtype"),
                   "menu_precise_seam_type",
                   []() { return true; },
                   m_parent);
}

wxMenuItem* MenuFactory::append_menu_item_instance_to_object(wxMenu* menu)
{
    wxMenuItem* menu_item = append_menu_item(menu, wxID_ANY, _L("Set as an individual object"), "",
        [](wxCommandEvent&) { obj_list()->split_instances(); }, "", menu);

    /* New behavior logic:
     * 1. Split Object to several separated object, if ALL instances are selected
     * 2. Separate selected instances from the initial object to the separated object,
     *    if some (not all) instances are selected
     */
    m_parent->Bind(wxEVT_UPDATE_UI, [](wxUpdateUIEvent& evt)
        {
            const Selection& selection = plater()->canvas3D()->get_selection();
            evt.SetText(selection.is_single_full_object() ?
                _L("Set as individual objects") : _L("Set as an individual object"));

            evt.Enable(plater()->can_set_instance_to_object());
        }, menu_item->GetId());

    return menu_item;
}

void MenuFactory::append_menu_item_fill_bed(wxMenu *menu)
{
    append_menu_item(
        menu, wxID_ANY, _L("Fill bed with copies"), _L("Fill the remaining area of bed with copies of the selected object"),
        [](wxCommandEvent &) { plater()->fill_bed_with_instances(); }, "", nullptr, []() { return plater()->can_increase_instances(); }, m_parent);
}

wxMenuItem* MenuFactory::append_menu_item_printable(wxMenu* menu)
{
    // BBS: to be checked
    wxMenuItem* menu_item_printable = append_menu_check_item(menu, wxID_ANY, _L("Printable"), "",
        [](wxCommandEvent&) { obj_list()->toggle_printable_state(); }, menu);

    m_parent->Bind(wxEVT_UPDATE_UI, [](wxUpdateUIEvent& evt) {
        ObjectList* list = obj_list();
        wxDataViewItemArray sels;
        list->GetSelections(sels);
        wxDataViewItem frst_item = sels[0];
        ItemType type = list->GetModel()->GetItemType(frst_item);
        bool check;
        if (type != itInstance && type != itObject)
            check = false;
        else {
            int obj_idx = list->GetModel()->GetObjectIdByItem(frst_item);
            int inst_idx = type == itObject ? 0 : list->GetModel()->GetInstanceIdByItem(frst_item);
            check = list->object(obj_idx)->instances[inst_idx]->printable;
        }

        evt.Check(check);
        plater()->set_current_canvas_as_dirty();

        }, menu_item_printable->GetId());

    return menu_item_printable;
}

void MenuFactory::append_menu_item_rename(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Rename"), "",
        [](wxCommandEvent&) { obj_list()->rename_item(); }, "", menu);

    menu->AppendSeparator();
}

wxMenuItem* MenuFactory::append_menu_item_fix_through_netfabb(wxMenu* menu)
{
    // Ultra: local OpenVDB voxel repair - always available, works on any platform,
    // and handles meshes the Windows repair service rejects. The original Windows
    // "Fix model" stays below as the conservative alternative.
    append_menu_item(menu, wxID_ANY, _L("Repair/Remesh"), _L("Rebuild the selected parts as watertight meshes (detail below ~0.1mm may be lost)"),
        [](wxCommandEvent&) { obj_list()->repair_by_remesh(); }, "", menu,
        []() {
            std::vector<int> obj_idxs, vol_idxs;
            obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
            return !obj_idxs.empty() || !vol_idxs.empty();
        }, plater());

    // Ultra: "Round all edges" - the Edit gizmo's interim whole-mesh fillet, next to
    // Repair/Remesh because it is the same voxel round trip with a rounding filter in
    // the middle. Hidden without OpenVDB, for the same reason the quad remesher is
    // hidden without QuadriFlow.
    if (voxel_ops_available()) {
        append_menu_item(menu, wxID_ANY, _L("Round all edges..."), _L("Fillet every edge of the selected parts by a radius (the part is rebuilt, so painted data is cleared)"),
            [](wxCommandEvent&) { obj_list()->round_all_edges(); }, "", menu,
            []() {
                std::vector<int> obj_idxs, vol_idxs;
                obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
                return !obj_idxs.empty() || !vol_idxs.empty();
            }, plater());
    }

    // Ultra: Phase 2 - the quad remesher, next to Repair/Remesh as the spec asks.
    // Hidden entirely when the build has no QuadriFlow: an entry that can only ever
    // report "unavailable" is worse than no entry.
    if (quad_remesh_available()) {
        append_menu_item(menu, wxID_ANY, _L("Quad remesh..."), _L("Rebuild the selected parts as an even grid of quads at a target face count (needs a closed mesh)"),
            [](wxCommandEvent&) { obj_list()->quad_remesh(); }, "", menu,
            []() {
                std::vector<int> obj_idxs, vol_idxs;
                obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
                return !obj_idxs.empty() || !vol_idxs.empty();
            }, plater());
    }

    if (!is_windows10())
        return nullptr;

    wxMenuItem* menu_item = append_menu_item(menu, wxID_ANY, _L("Fix model"), "",
        [](wxCommandEvent&) { obj_list()->fix_through_netfabb(); }, "", menu,
        []() {return plater()->can_fix_through_netfabb(); }, plater());

    return menu_item;
}

void MenuFactory::append_menu_item_export_stl(wxMenu* menu, bool is_mulity_menu)
{
    append_menu_item(menu, wxID_ANY, _L("Export as one STL") + dots, "",
        [](wxCommandEvent&) { plater()->export_stl(false, true); }, "", nullptr,
        [is_mulity_menu]() {
            const Selection& selection = plater()->canvas3D()->get_selection();
            if (is_mulity_menu)
                return selection.is_multiple_full_instance() || selection.is_multiple_full_object();
            else
                return selection.is_single_full_instance() || selection.is_single_full_object();
        }, m_parent);
    if (!is_mulity_menu)
        return;
    append_menu_item(menu, wxID_ANY, _L("Export as STLs") + dots, "",
        [](wxCommandEvent&) { plater()->export_stl(false, true, true); }, "", nullptr,
        []() {
            const Selection& selection = plater()->canvas3D()->get_selection();
            return selection.is_multiple_full_instance() || selection.is_multiple_full_object();
        }, m_parent);
}

// Export the selected object(s)/instance(s) as a STEP file of solids.
void MenuFactory::append_menu_item_export_step(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Export as STEP") + dots,
        _L("Export the selection as a STEP file of solids; parts imported from STEP keep their exact geometry"),
        [](wxCommandEvent&) { plater()->export_step(true); }, "", nullptr,
        []() {
            const Selection& selection = plater()->canvas3D()->get_selection();
            return selection.is_single_full_instance() || selection.is_single_full_object() ||
                   selection.is_multiple_full_instance() || selection.is_multiple_full_object();
        }, m_parent);
}

// Ultra: export the ONE selected part. The object menu's "Export as one STL" only ever
// handled whole objects/instances, so a single selected ModelVolume had no export at all.
// Enabled for exactly one volume - modifiers, negative volumes and support blockers or
// enforcers included, they are all meshes the user may want out.
void MenuFactory::append_menu_item_export_stl_part(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Export part as STL") + dots,
        _L("Export only the selected part as an STL file, positioned as it sits on the plate"),
        [](wxCommandEvent&) { plater()->export_stl_part(); }, "", nullptr,
        []() {
            const Selection& selection = plater()->canvas3D()->get_selection();
            return selection.is_single_volume_or_modifier();
        }, m_parent);
}

void MenuFactory::append_menu_item_reload_from_disk(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Reload from disk"), _L("Reload the selected parts from disk"),
        [](wxCommandEvent&) { plater()->reload_from_disk(); }, "", menu,
        []() { return plater()->can_reload_from_disk(); }, m_parent);
}

void MenuFactory::append_menu_item_replace_with_stl(wxMenu *menu)
{
    append_menu_item(menu, wxID_ANY, _L("Replace with STL"), _L("Replace the selected part with new STL"),
        [](wxCommandEvent &) { plater()->replace_with_stl(); }, "", menu,
        []() { return plater()->can_replace_with_stl(); }, m_parent);
}

// Exact B-rep fillet / chamfer / shell of the selected part (GLGizmoCadFillet).
void MenuFactory::append_menu_item_cad_fillet(wxMenu *menu)
{
    append_menu_item(menu, wxID_ANY, _L("Fillet / chamfer (CAD)") + dots,
        _L("Exact fillet, chamfer or shell of the part's CAD body: STEP parts keep their exact shape, meshes are converted first"),
        [](wxCommandEvent &) { obj_list()->open_cad_fillet(); }, "", menu,
        []() { return ObjectList::can_open_cad_fillet(); }, m_parent);
}

void MenuFactory::append_menu_item_edit_in_blender(wxMenu *menu)
{
    append_menu_item(menu, wxID_ANY, _L("Edit in Blender"), _L("Open the selected part in Blender. Saving in Blender updates the part here"),
        [](wxCommandEvent &) { plater()->edit_in_blender(); }, "", menu,
        []() { return plater()->can_edit_in_blender(); }, m_parent);
}

void MenuFactory::append_menu_item_edit_in_freecad(wxMenu *menu)
{
    append_menu_item(menu, wxID_ANY, _L("Edit in FreeCAD"), _L("Open the selected part in FreeCAD. Saving in FreeCAD updates the part here"),
        [](wxCommandEvent &) { plater()->edit_in_freecad(); }, "", menu,
        []() { return plater()->can_edit_in_freecad(); }, m_parent);
}

void MenuFactory::append_menu_item_change_extruder(wxMenu* menu)
{
    // BBS
    const std::vector<wxString> names = { _L("Change filament"), _L("Set filament for selected items") };
    // Delete old menu item
    for (const wxString& name : names) {
        const int item_id = menu->FindItem(name);
        if (item_id != wxNOT_FOUND)
            menu->Destroy(item_id);
    }

    const std::vector<unsigned int> ordered_filament_ids = ui_ordered_filament_ids();
    if (ordered_filament_ids.size() <= 1)
        return;

    wxDataViewItemArray sels;
    obj_list()->GetSelections(sels);
    if (sels.IsEmpty())
        return;

    std::vector<wxBitmap*> icons = get_extruder_color_icons(true);
    wxMenu* extruder_selection_menu = new wxMenu();
    const wxString& name = sels.Count() == 1 ? names[0] : names[1];

    int initial_extruder = -1; // negative value for multiple object/part selection
    if (sels.Count() == 1) {
        const ModelConfig& config = obj_list()->get_item_config(sels[0]);
        // BBS: set default extruder to 1
        initial_extruder = config.has("extruder") ? config.extruder() : 0;
    }

    for (size_t display_idx = 0; display_idx <= ordered_filament_ids.size(); ++display_idx)
    {
        const int actual_filament_id = display_idx == 0 ? 0 : int(ordered_filament_ids[display_idx - 1]);
        const bool is_active_extruder = actual_filament_id == initial_extruder;
        const int icon_idx = actual_filament_id == 0 ? 0 : actual_filament_id - 1;

        wxString item_name = filament_menu_item_name(actual_filament_id, int(display_idx));

        if (is_active_extruder) {
            item_name << " (" + _L("current") + ")";
        }

        if (icon_idx >= 0 && icon_idx < icons.size()) {
            append_menu_item(
                extruder_selection_menu, wxID_ANY, item_name, "",
                [actual_filament_id](wxCommandEvent &) { obj_list()->set_extruder_for_selected_items(actual_filament_id); }, *icons[icon_idx], menu,
                [is_active_extruder]() { return !is_active_extruder; }, m_parent);
        } else {
            append_menu_item(
                extruder_selection_menu, wxID_ANY, item_name, "",
                [actual_filament_id](wxCommandEvent &) { obj_list()->set_extruder_for_selected_items(actual_filament_id); }, "", menu,
                [is_active_extruder]() { return !is_active_extruder; }, m_parent);
        }
    }

    menu->AppendSubMenu(extruder_selection_menu, name);
}

void MenuFactory::append_menu_item_scale_selection_to_fit_print_volume(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Scale to build volume"), _L("Scale an object to fit the build volume"),
        [](wxCommandEvent&) { plater()->scale_selection_to_fit_print_volume(); }, "", menu,
        []() { return plater()->can_scale_to_print_volume(); }, m_parent);
}

void MenuFactory::append_menu_items_flush_options(wxMenu* menu)
{
    const wxString name = _L("Flush Options");
    // Delete old menu item
    const int item_id = menu->FindItem(name);
    if (item_id != wxNOT_FOUND)
        menu->Destroy(item_id);

    bool show_flush_option_menu = false;
    ObjectList* object_list = obj_list();
    const Selection& selection = get_selection();
    if (selection.get_object_idx() < 0)
        return;
    if (wxGetApp().plater()->get_partplate_list().get_curr_plate()->contains(selection.get_bounding_box())) {
        auto plate_extruders = wxGetApp().plater()->get_partplate_list().get_curr_plate()->get_extruders();
        for (auto extruder : plate_extruders) {
            if (extruder != plate_extruders[0])
                show_flush_option_menu = true;
        }
    }
    if (!show_flush_option_menu)
        return;

    DynamicPrintConfig& global_config = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    ModelConfig& select_object_config = object_list->object(selection.get_object_idx())->config;

    wxMenu* flush_options_menu = new wxMenu();
    auto can_flush = [&global_config]() {
        auto option = global_config.option("enable_prime_tower");
        return option ? option->getBool() : false;
    };
    append_menu_check_item(flush_options_menu, wxID_ANY, _L("Flush into objects' infill"), "",
        [&select_object_config, &global_config](wxCommandEvent&) {
            const ConfigOption* option = select_object_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][0]);
            if (!option) {
                option = global_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][0]);
            }
            select_object_config.set_key_value(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][0], new ConfigOptionBool(!option->getBool()));
            wxGetApp().obj_settings()->UpdateAndShow(true);
        }, menu, can_flush,
        [&select_object_config, &global_config]() {
            const ConfigOption* option = select_object_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][0]);
            if (!option) {
                option = global_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][0]);
            }
            return option->getBool();
        }, m_parent);

    append_menu_check_item(flush_options_menu, wxID_ANY, _L("Flush into this object"), "",
        [&select_object_config, &global_config](wxCommandEvent&) {
            const ConfigOption* option = select_object_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][1]);
            if (!option) {
                option = global_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][1]);
            }
            select_object_config.set_key_value(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][1], new ConfigOptionBool(!option->getBool()));
            wxGetApp().obj_settings()->UpdateAndShow(true);
        }, menu, can_flush,
        [&select_object_config, &global_config]() {
            const ConfigOption* option = select_object_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][1]);
            if (!option) {
                option = global_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][1]);
            }
            return option->getBool();
        }, m_parent);

    append_menu_check_item(flush_options_menu, wxID_ANY, _L("Flush into objects' support"), "",
        [&select_object_config, &global_config](wxCommandEvent&) {
            const ConfigOption* option = select_object_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][2]);
            if (!option) {
                option = global_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][2]);
            }
            select_object_config.set_key_value(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][2], new ConfigOptionBool(!option->getBool()));
            wxGetApp().obj_settings()->UpdateAndShow(true);
        }, menu, can_flush,
        [&select_object_config, &global_config]() {
            const ConfigOption* option = select_object_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][2]);
            if (!option) {
                option = global_config.option(FREQ_SETTINGS_BUNDLE_FFF["Flush options"][2]);
            }
            return option->getBool();
        }, m_parent);

    size_t i = 0;
    for (auto node = menu->GetMenuItems().GetFirst(); node; node = node->GetNext())
    {
        i++;
        wxMenuItem* item = node->GetData();
        if (item->GetItemLabelText() == _L("Edit in Parameter Table"))
            break;
    }
    menu->Insert(i, wxID_ANY, _L("Flush Options"), flush_options_menu);
}

void MenuFactory::append_menu_items_convert_unit(wxMenu* menu)
{
    std::vector<int> obj_idxs, vol_idxs;
    obj_list()->get_selection_indexes(obj_idxs, vol_idxs);
    if (obj_idxs.empty() && vol_idxs.empty())
        return;

    auto volume_respects_conversion = [](ModelVolume* volume, ConversionType conver_type)
    {
        return  (conver_type == ConversionType::CONV_FROM_INCH && volume->source.is_converted_from_inches) ||
            (conver_type == ConversionType::CONV_TO_INCH && !volume->source.is_converted_from_inches) ||
            (conver_type == ConversionType::CONV_FROM_METER && volume->source.is_converted_from_meters) ||
            (conver_type == ConversionType::CONV_TO_METER && !volume->source.is_converted_from_meters);
    };

    auto can_append = [obj_idxs, vol_idxs, volume_respects_conversion](ConversionType conver_type)
    {
        ModelObjectPtrs objects;
        for (int obj_idx : obj_idxs) {
            ModelObject* object = obj_list()->object(obj_idx);
            if (vol_idxs.empty()) {
                for (ModelVolume* volume : object->volumes)
                    if (volume_respects_conversion(volume, conver_type))
                        return false;
            }
            else {
                for (int vol_idx : vol_idxs)
                    if (volume_respects_conversion(object->volumes[vol_idx], conver_type))
                        return false;
            }
        }
        return true;
    };

    std::vector<std::pair<ConversionType, wxString>> items = {
        {ConversionType::CONV_FROM_INCH , _L("Convert from inches") },
        {ConversionType::CONV_TO_INCH   , _L("Restore to inches") },
        {ConversionType::CONV_FROM_METER, _L("Convert from meters") },
        {ConversionType::CONV_TO_METER  , _L("Restore to meters") } };

    for (auto item : items) {
        int menu_id = menu->FindItem(item.second);
        if (can_append(item.first)) {
            // Add menu item if it doesn't exist
            if (menu_id == wxNOT_FOUND)
                append_menu_item(menu, wxID_ANY, item.second, item.second,
                    [item](wxCommandEvent&) { plater()->convert_unit(item.first); }, "", menu,
                    []() { return true; }, m_parent);
        }
        else if (menu_id != wxNOT_FOUND) {
            // Delete menu item
            menu->Destroy(menu_id);
        }
    }
}

void MenuFactory::append_menu_item_merge_to_multipart_object(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Assemble"), _L("Assemble the selected objects to an object with multiple parts"),
        [](wxCommandEvent&) { obj_list()->merge(true); }, "", menu,
        []() { return obj_list()->can_merge_to_multipart_object(); }, m_parent);
}

// Ultra: per-object/per-part Normal/Ghost/Hidden view modes for the Prepare view.
void MenuFactory::append_menu_items_visibility(wxMenu* menu)
{
    // Every selected row resolves to a ModelVolume id (part rows) or ModelObject id.
    auto selected_object_ids = []() -> std::vector<size_t> {
        std::vector<size_t> ids;
        wxDataViewItemArray sels;
        obj_list()->GetSelections(sels);
        const auto& objects = plater()->model().objects;
        for (const wxDataViewItem& item : sels) {
            const ItemType type = list_model()->GetItemType(item);
            const int obj_idx = list_model()->GetObjectIdByItem(item);
            if (obj_idx < 0 || obj_idx >= (int) objects.size())
                continue;
            if (type & itVolume) {
                const int vol_idx = list_model()->GetVolumeIdByItem(item);
                if (vol_idx >= 0 && vol_idx < (int) objects[obj_idx]->volumes.size())
                    ids.push_back(objects[obj_idx]->volumes[vol_idx]->id().id);
            } else if (type & (itObject | itInstance))
                ids.push_back(objects[obj_idx]->id().id);
        }
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        return ids;
    };
    auto set_mode = [selected_object_ids](GLCanvas3D::ObjectViewMode mode) {
        const std::vector<size_t> ids = selected_object_ids();
        for (size_t id : ids)
            plater()->canvas3D()->set_object_view_mode(id, mode);
        if (!ids.empty())
            obj_list()->update_visibility_icons();
    };
    auto selected_object_id = [selected_object_ids]() -> std::pair<bool, size_t> {
        const std::vector<size_t> ids = selected_object_ids();
        return { !ids.empty(), ids.empty() ? 0 : ids.front() };
    };
    wxMenu* vis = new wxMenu();
    append_menu_item(vis, wxID_ANY, _L("Normal"), _L("Show this object normally"),
        [set_mode](wxCommandEvent&) { set_mode(GLCanvas3D::ObjectViewMode::Normal); }, "", nullptr,
        [selected_object_id]() { return selected_object_id().first; }, m_parent);
    append_menu_item(vis, wxID_ANY, _L("Ghost (X-ray)"), _L("Render this object see-through; clicks pass through to objects behind it"),
        [set_mode](wxCommandEvent&) { set_mode(GLCanvas3D::ObjectViewMode::Ghost); }, "", nullptr,
        [selected_object_id]() { return selected_object_id().first; }, m_parent);
    append_menu_item(vis, wxID_ANY, _L("Hidden"), _L("Hide this object in the 3D view (it still slices and prints)"),
        [set_mode](wxCommandEvent&) { set_mode(GLCanvas3D::ObjectViewMode::Hidden); }, "", nullptr,
        [selected_object_id]() { return selected_object_id().first; }, m_parent);
    vis->AppendSeparator();
    append_menu_item(vis, wxID_ANY, _L("Show all objects"), _L("Reset every object back to normal visibility"),
        [](wxCommandEvent&) { plater()->canvas3D()->clear_object_view_modes(); obj_list()->update_visibility_icons(); }, "", nullptr,
        []() { return plater()->canvas3D()->has_object_view_modes(); }, m_parent);
    append_submenu(menu, vis, wxID_ANY, _L("Visibility"), _L("Per-object view mode in the Prepare view"), "",
        []() { return true; }, m_parent);
}

// ---------------------------------------------------------------------------------------------
// Ultra (support groups): assign the selected parts to a named support group.
//
// A group's VALUES live on the parts themselves - the curated part-level support keys are
// written onto ModelVolume::config - so a saved 3MF is self-contained and reopens anywhere,
// including stock Orca, which drops the unknown support_group key silently. support_group is
// only the label the badge and the Support-groups dialog read back:
// PrintObject::support_groups() groups by RESOLVED config and never by name, so a part whose
// values equal the object's collapses into the default group and K stays 1.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2.2, 3.4, 3.5.
// ---------------------------------------------------------------------------------------------

// The MODEL_PART volumes behind the current object-list selection, in list order, deduplicated.
// Re-derived inside every menu handler rather than captured, so a menu built for one selection
// can never write to a stale volume.
std::vector<ModelVolume*> selected_part_volumes()
{
    std::vector<ModelVolume*> volumes;
    wxDataViewItemArray sels;
    obj_list()->GetSelections(sels);
    const auto& objects = plater()->model().objects;
    for (const wxDataViewItem& item : sels) {
        if (!(list_model()->GetItemType(item) & itVolume))
            continue;
        const int obj_idx = list_model()->GetObjectIdByItem(item);
        const int vol_idx = list_model()->GetVolumeIdByItem(item);
        if (obj_idx < 0 || obj_idx >= (int) objects.size())
            continue;
        if (vol_idx < 0 || vol_idx >= (int) objects[obj_idx]->volumes.size())
            continue;
        ModelVolume* volume = objects[obj_idx]->volumes[vol_idx];
        // A modifier, a support enforcer/blocker or a negative volume never carries a group -
        // PrintObject::support_groups() only ever looks at is_model_part() volumes.
        if (volume->is_model_part() && std::find(volumes.begin(), volumes.end(), volume) == volumes.end())
            volumes.push_back(volume);
    }
    return volumes;
}

// The distinct group labels an object's parts already use, in first-appearance order.
std::vector<std::string> object_support_group_names(const ModelObject* object)
{
    std::vector<std::string> names;
    if (object == nullptr)
        return names;
    for (const ModelVolume* volume : object->volumes) {
        const std::string name = SettingsFactory::part_support_group(volume);
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }
    return names;
}

// "Group", "Group 2", "Group 3", ... - the first label this object does not use yet.
std::string unique_support_group_name(const ModelObject* object, const std::string& base)
{
    const std::vector<std::string> used = object_support_group_names(object);
    auto taken = [&used](const std::string& n) {
        return std::find(used.begin(), used.end(), n) != used.end();
    };
    if (!taken(base))
        return base;
    for (int i = 2; i < 1000; ++ i) {
        const std::string candidate = base + " " + std::to_string(i);
        if (!taken(candidate))
            return candidate;
    }
    return base;
}

// The curated support values an existing group carries, read off its first member, so a part
// joining the group ends up with the same RESOLVED config and therefore really is in that group.
DynamicPrintConfig support_group_values(const ModelObject* object, const std::string& group)
{
    DynamicPrintConfig values;
    if (object == nullptr)
        return values;
    for (const ModelVolume* volume : object->volumes) {
        if (SettingsFactory::part_support_group(volume) != group)
            continue;
        const DynamicPrintConfig& cfg = volume->config.get();
        for (const std::string& key : SettingsFactory::part_support_keys())
            if (const ConfigOption* opt = cfg.option(key); opt != nullptr)
                values.set_key_value(key, opt->clone());
        break;
    }
    return values;
}

// The object's own effective support values - the edited process preset with the object's
// overrides on top. A brand new group starts from these, so it shows up in the part panel
// immediately and still resolves into the default group until the user changes something.
DynamicPrintConfig object_support_values(const ModelObject* object)
{
    DynamicPrintConfig values;
    if (wxGetApp().preset_bundle == nullptr)
        return values;
    DynamicPrintConfig full = wxGetApp().preset_bundle->prints.get_edited_preset().config;
    if (object != nullptr)
        full.apply(object->config.get(), true);
    for (const std::string& key : SettingsFactory::part_support_keys())
        if (const ConfigOption* opt = full.option(key); opt != nullptr)
            values.set_key_value(key, opt->clone());
    return values;
}

// A support set reduced to the part-level keys. The set carries its interface filament as a
// TYPE; resolve_interface_filament() turns it into a slot here, which is what makes a set
// portable between printers (the plan's 3.3).
static DynamicPrintConfig support_set_values(const SupportSet& set, std::string* warning)
{
    DynamicPrintConfig filaments;
    if (PresetBundle* bundle = wxGetApp().preset_bundle; bundle != nullptr) {
        const DynamicPrintConfig full = bundle->full_config();
        for (const char* key : { "filament_type", "filament_soluble" })
            if (const ConfigOption* opt = full.option(key); opt != nullptr)
                filaments.set_key_value(key, opt->clone());
    }
    DynamicPrintConfig applied;
    support_set_apply_to(set, applied, filaments, warning);

    DynamicPrintConfig values;
    for (const std::string& key : SettingsFactory::part_support_keys())
        if (const ConfigOption* opt = applied.option(key); opt != nullptr)
            values.set_key_value(key, opt->clone());
    return values;
}

// By name, for callers that only have the group label: a group "references" the support set of
// the same name - that IS the whole reference. There is no second config key holding a set id;
// the plan's 3.1 allows exactly one new key, so the label carries the association and a group
// whose set was renamed or deleted simply stops showing one.
DynamicPrintConfig support_set_values_by_name(const std::string& set_name, std::string* warning)
{
    SupportSetStore& store = SupportSetStore::instance();
    const SupportSet* set = store.find(set_name);
    if (set == nullptr) {
        store.reload();
        set = store.find(set_name);
    }
    return set != nullptr ? support_set_values(*set, warning) : DynamicPrintConfig();
}

// Write a group assignment onto every selected part, under ONE undo snapshot, so Ctrl+Z takes
// the badge, the part panel and PrintObject::support_groups() back together
// (ModelConfig::set_key_value bumps the timestamp the undo stack keys on - the plan's R2.4).
void assign_support_group(const std::vector<ModelVolume*>& volumes,
                          const std::string&               group_name,
                          const DynamicPrintConfig*        values,
                          const std::string&               snapshot)
{
    if (volumes.empty() || plater() == nullptr)
        return;
    plater()->take_snapshot(snapshot);

    const std::vector<std::string>& keys = SettingsFactory::part_support_keys();
    for (ModelVolume* volume : volumes) {
        // Always start from a clean slate: a stale override left behind would keep the part in a
        // group nobody asked for, because the resolver keys on values and not on the label.
        volume->config.erase("support_group");
        for (const std::string& key : keys)
            volume->config.erase(key);
        if (values == nullptr)
            continue;
        if (!group_name.empty())
            volume->config.set_key_value("support_group", new ConfigOptionString(group_name));
        for (const std::string& key : keys)
            if (const ConfigOption* opt = values->option(key); opt != nullptr)
                volume->config.set_key_value(key, opt->clone());
    }

    const auto& objects = plater()->model().objects;
    std::vector<int> changed_objects;
    for (ModelVolume* volume : volumes) {
        // Adds or removes the settings node under the part and refreshes the part panel.
        obj_list()->object_config_options_changed({ volume->get_object(), volume });
        for (int i = 0; i < (int) objects.size(); ++ i)
            if (objects[i] == volume->get_object()) {
                if (std::find(changed_objects.begin(), changed_objects.end(), i) == changed_objects.end())
                    changed_objects.push_back(i);
                break;
            }
    }
    obj_list()->update_support_group_badges();
    support_groups_dialog_refresh();
    // What actually triggers the re-slice; PrintApply's model_support_group_data_changed() then
    // invalidates posSupportMaterial (or posSlice when the soluble rule flips).
    for (int obj_idx : changed_objects)
        obj_list()->changed_object(obj_idx);
}

void MenuFactory::append_menu_items_support_group(wxMenu* menu)
{
    const wxString title = _L("Support group");
    // The submenu's contents depend on the selection and the part menu is long-lived, so it is
    // rebuilt on every open - the idiom of append_menu_item_change_filament().
    if (const int item_id = menu->FindItem(title); item_id != wxNOT_FOUND)
        menu->Destroy(item_id);

    const std::vector<ModelVolume*> volumes = selected_part_volumes();
    if (volumes.empty())
        return;
    const ModelObject* object = volumes.front()->get_object();

    wxMenu* sub = new wxMenu();

    append_menu_item(sub, wxID_ANY, _L("None (object settings)"),
        _L("Take these parts out of their support group: they follow the object's own support settings again"),
        [](wxCommandEvent&) {
            assign_support_group(selected_part_volumes(), std::string(), nullptr, "Support group cleared");
        }, "", nullptr, []() { return true; }, m_parent);

    const std::vector<std::string> names = object_support_group_names(object);
    if (!names.empty())
        sub->AppendSeparator();
    for (const std::string& name : names)
        append_menu_item(sub, wxID_ANY, from_u8(name),
            format_wxstr(_L("Put the selected parts in the support group \"%1%\""), from_u8(name)),
            [name](wxCommandEvent&) {
                const std::vector<ModelVolume*> sel = selected_part_volumes();
                if (sel.empty())
                    return;
                const DynamicPrintConfig values = support_group_values(sel.front()->get_object(), name);
                assign_support_group(sel, name, &values, "Support group assigned");
            }, "", nullptr, []() { return true; }, m_parent);

    sub->AppendSeparator();

    // Building the menu is an explicit user action, so a directory scan is allowed here - unlike
    // inside a settings-change scope, which a phone request can reach.
    SupportSetStore& store = SupportSetStore::instance();
    store.reload();
    if (!store.list().empty()) {
        wxMenu* from_set = new wxMenu();
        for (const SupportSet& set : store.list()) {
            const std::string set_name = set.name;
            append_menu_item(from_set, wxID_ANY, from_u8(set_name), "",
                [set_name](wxCommandEvent&) {
                    const std::vector<ModelVolume*> sel = selected_part_volumes();
                    const SupportSet* set = SupportSetStore::instance().find(set_name);
                    if (sel.empty() || set == nullptr)
                        return;
                    std::string warning;
                    const DynamicPrintConfig values = support_set_values(*set, &warning);
                    const std::string group = unique_support_group_name(sel.front()->get_object(), set_name);
                    assign_support_group(sel, group, &values, "Support group assigned");
                    // MessageDialog, never a raw wxDialog: its ShowModal override auto-answers on
                    // a hub-managed instance rather than blocking a GUI nobody is watching.
                    if (!warning.empty())
                        MessageDialog(plater(), from_u8(warning), _L("Support group"),
                                      wxICON_INFORMATION | wxOK).ShowModal();
                }, "", nullptr, []() { return true; }, m_parent);
        }
        append_submenu(sub, from_set, wxID_ANY, _L("New group from support set"),
            _L("Create a group named after the set and write its resolved values onto the selected parts"),
            "", []() { return true; }, m_parent);
    }

    append_menu_item(sub, wxID_ANY, _L("New group"),
        _L("Create a support group seeded with the object's current support settings"),
        [](wxCommandEvent&) {
            const std::vector<ModelVolume*> sel = selected_part_volumes();
            if (sel.empty())
                return;
            const ModelObject* mo = sel.front()->get_object();
            const DynamicPrintConfig values = object_support_values(mo);
            // Not translated: the label is written into the 3MF and read back on any machine.
            assign_support_group(sel, unique_support_group_name(mo, "Group"), &values,
                                 "Support group assigned");
        }, "", nullptr, []() { return true; }, m_parent);

    sub->AppendSeparator();
    append_menu_item(sub, wxID_ANY, _L("Support groups..."),
        _L("Open the Support groups window for this object"),
        [](wxCommandEvent&) {
            const std::vector<ModelVolume*> sel = selected_part_volumes();
            if (!sel.empty())
                open_support_groups_dialog(plater(), sel.front()->get_object());
        }, "", nullptr, []() { return true; }, m_parent);

    append_submenu(menu, sub, wxID_ANY, title,
        _L("Assign these parts to a named support group"), "",
        []() { return true; }, m_parent);
}

// Ported from BambuStudio's "Sub merge": extract the selected parts into a new assembly.
void MenuFactory::append_menu_item_assemble_separately(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Assemble Separately"), _L("Move the selected parts into a new assembly, keeping their position in space"),
        [](wxCommandEvent&) { obj_list()->assemble_separately(); }, "", nullptr,
        []() {
            if (plater()->get_current_canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasView3D)
                return false;
            return obj_list()->can_assemble_separately();
        }, m_parent);
}

void MenuFactory::append_menu_item_merge_to_single_object(wxMenu* menu)
{
    menu->AppendSeparator();
    append_menu_item(menu, wxID_ANY, _L("Assemble"), _L("Assemble the selected objects to an object with single part"),
        [](wxCommandEvent&) { obj_list()->merge(false); }, "", menu,
        []() { return obj_list()->can_merge_to_single_object(); }, m_parent);
}

void MenuFactory::append_menu_item_merge_parts_to_single_part(wxMenu* menu)
{
    menu->AppendSeparator();
    append_menu_item(menu, wxID_ANY, _L("Mesh boolean"), _L("Mesh boolean operations including union and subtraction"),
        [](wxCommandEvent&) { obj_list()->boolean/*merge_volumes*/(); }, "", menu,
        []() { return obj_list()->can_mesh_boolean(); }, m_parent);
}

// BBS: merge the selected parts into one part, keeping the other parts of the object (port of OrcaSlicer#15413)
void MenuFactory::append_menu_item_merge_some_parts_to_single_part(wxMenu* menu)
{
    menu->AppendSeparator();
    append_menu_item(menu, wxID_ANY, _L("Merge into Single Part"), _L("Merge the selected parts into one part"),
        [](wxCommandEvent&) { obj_list()->merge_volumes(); }, "", menu,
        []() {
            if (plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasView3D)
                return false;
            return !obj_list()->has_selected_cut_object();
        },
        m_parent);
}

void MenuFactory::append_menu_items_mirror(wxMenu* menu)
{
    wxMenu* mirror_menu = new wxMenu();
    if (!mirror_menu)
        return;

    append_menu_item(mirror_menu, wxID_ANY, _L("Along X axis"), _L("Mirror along the X axis"),
        [](wxCommandEvent&) { plater()->mirror(X); }, "menu_mirror_x", menu);
    append_menu_item(mirror_menu, wxID_ANY, _L("Along Y axis"), _L("Mirror along the Y axis"),
        [](wxCommandEvent&) { plater()->mirror(Y); }, "menu_mirror_y", menu);
    append_menu_item(mirror_menu, wxID_ANY, _L("Along Z axis"), _L("Mirror along the Z axis"),
        [](wxCommandEvent&) { plater()->mirror(Z); }, "menu_mirror_z", menu);

    append_submenu(menu, mirror_menu, wxID_ANY, _L("Mirror"), _L("Mirror object"), "",
        []() { return plater()->can_mirror(); }, m_parent);
}

void MenuFactory::append_menu_item_edit_text(wxMenu *menu)
{
    wxString name        = _L("Edit text");

    auto can_edit_text = []() {
        if (plater() == nullptr)
            return false;
        const Selection& selection = plater()->get_selection();
        if (selection.volumes_count() != 1)
            return false;
        const GLVolume* gl_volume = selection.get_first_volume();
        if (gl_volume == nullptr)
            return false;
        const ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
        if (volume == nullptr)
            return false;
        return volume->is_text();
    };

    if (menu != &m_text_part_menu) {
        const int menu_item_id = menu->FindItem(name);
        if (menu_item_id != wxNOT_FOUND)
            menu->Destroy(menu_item_id);
        if (!can_edit_text())
            return;
    }

    wxString description = _L("Ability to change text, font, size, ...");
    std::string icon = "cog";
    auto open_emboss = [](const wxCommandEvent &) {
        GLGizmosManager &mng = plater()->get_view3D_canvas3D()->get_gizmos_manager();
        if (mng.get_current_type() == GLGizmosManager::Emboss)
            mng.open_gizmo(GLGizmosManager::Emboss); // close() and reopen - move to be visible
        mng.open_gizmo(GLGizmosManager::Emboss);
    };
    append_menu_item(menu, wxID_ANY, name, description, open_emboss, icon, nullptr, can_edit_text, m_parent);
}

void MenuFactory::append_menu_item_edit_svg(wxMenu *menu)
{
    wxString name = _L("Edit SVG");
    auto can_edit_svg = []() {
        if (plater() == nullptr)
            return false;
        const Selection& selection = plater()->get_selection();
        if (selection.volumes_count() != 1)
            return false;
        const GLVolume* gl_volume = selection.get_first_volume();
        if (gl_volume == nullptr)
            return false;
        const ModelVolume *volume = get_model_volume(*gl_volume, selection.get_model()->objects);
        if (volume == nullptr)
            return false;
        return volume->is_svg();
    };

    if (menu != &m_svg_part_menu) {
        const int menu_item_id = menu->FindItem(name);
        if (menu_item_id != wxNOT_FOUND)
            menu->Destroy(menu_item_id);
        if (!can_edit_svg())
            return;
    }

    wxString description = _L("Change SVG source file, projection, size, ...");
    std::string icon = "cog";
    auto open_svg = [](const wxCommandEvent &) {
        GLGizmosManager &mng = plater()->get_view3D_canvas3D()->get_gizmos_manager();
        if (mng.get_current_type() == GLGizmosManager::Svg)
            mng.open_gizmo(GLGizmosManager::Svg); // close() and reopen - move to be visible
        mng.open_gizmo(GLGizmosManager::Svg);
    };
    append_menu_item(menu, wxID_ANY, name, description, open_svg, icon, nullptr, can_edit_svg, m_parent);
}

// RE-EDITABLE CUTS. Reopen the Cut gizmo on the cut that produced the selected object,
// with its original uncut shape and the surface and settings it was cut with.
//
// Dynamic, like "Invalidate cut info" beside it: the item only exists while the
// selection actually carries a recipe, so it is destroyed and re-appended on every
// menu open rather than being greyed out forever on objects that were never cut.
void MenuFactory::append_menu_item_edit_cut(wxMenu *menu)
{
    const wxString menu_name = _L("Edit cut...");

    auto menu_item_id = menu->FindItem(menu_name);
    if (menu_item_id != wxNOT_FOUND)
        menu->Destroy(menu_item_id);

    if (obj_list()->has_selected_editable_cut())
        append_menu_item(menu, wxID_ANY, menu_name, _L("Edit the cut that produced this object"),
            [](wxCommandEvent &) { obj_list()->edit_cut(); },
            "", menu, []() { return true; }, m_parent);
}

// "COPY CUT TO...": take the selected object's cut and set it up on ANOTHER
// object, so a cut authored on one half of a symmetric part lands exactly - not
// hand-matched - on the other. Together with the cut panel's Mirror on X / Y / Z
// buttons this is the owner's ask: "currently it's hard to duplicate a cut
// exactly on both halves of an object."
//
// A SUBMENU of the objects, rather than a clipboard pair ("Copy cut" then
// "Paste cut") or a picker on the target. One gesture from where the intent
// forms - the user has just finished cutting half A and is looking at A when
// they want the same cut on B - and no stale clipboard that can paste onto the
// wrong object three actions later.
//
// THE SOURCE OBJECT IS LISTED TOO, and that is a deliberate departure from the
// design doc's "every OTHER object". The doc assumed a cut always produces two
// separate objects, so "the other half" was always another entry; it is not.
// A cut made with "Cut to parts" leaves both halves as PARTS OF ONE OBJECT, and
// the owner's case is exactly that - notches cut off an assembly, wanting the
// same cut on the other side of the SAME assembly. Filtering the source out
// removed the only entry that case ever wanted. It is marked rather than hidden,
// because re-cutting the object a cut came from is a different thing from cutting
// a neighbour and the menu should not pretend otherwise.
//
// THE LABELS NAME THE PARTS. An object made by the assemble action is called
// "Assembly", and a plate full of them gave a submenu of a dozen identical
// "Assembly" entries - the list the owner was shown, in which nothing could be
// told apart. An object's parts are what actually identify it, so multi-part
// objects carry them in the label. The target is still the OBJECT: the Cut gizmo
// works on a whole instance (GLGizmoCut3D::on_is_activable requires
// is_single_full_instance), so a cut applies to every part of the object it opens
// on, and a per-part target would be a promise the gizmo cannot keep.
//
// Dynamic, like "Edit cut..." above it and "Invalidate cut info" beside it: the
// object list changes under this menu, so it is destroyed and rebuilt on every
// open rather than greyed out forever.
void MenuFactory::append_menu_item_copy_cut(wxMenu *menu)
{
    const wxString menu_name = _L("Copy cut to...");

    auto menu_item_id = menu->FindItem(menu_name);
    if (menu_item_id != wxNOT_FOUND)
        menu->Destroy(menu_item_id);

    if (!obj_list()->has_selected_copyable_cut())
        return;

    const int src_idx = obj_list()->selected_cut_recipe_source();
    if (src_idx < 0)
        return;

    const ModelObjectPtrs &objects = obj_list()->objects() ? *obj_list()->objects() : ModelObjectPtrs();

    wxMenu *sub = new wxMenu();
    int     targets = 0;
    for (int i = 0; i < int(objects.size()); ++ i) {
        // Index AND name: two imports of the same STL carry the same name, and the
        // user has to be able to tell which row of the object list they are aiming
        // at. The index is 1-based, matching what the list shows.
        wxString label = format_wxstr("%1%. %2%", i + 1, from_u8(cut_target_label(objects[i])));
        if (i == src_idx)
            // The source of the cut, marked. Its OTHER parts are the owner's main
            // case ("the same notch on the other side"), so it must be reachable -
            // but re-cutting the object the cut came from should be a conscious
            // click, not one indistinguishable from cutting a neighbour.
            label += _L(" (this object)");
        append_menu_item(sub, wxID_ANY, label,
            // The frame caveat, said out loud rather than left as a surprise: the
            // cut is placed in the TARGET's own model space, which is exactly right
            // for two halves of one cut (they share that space) and a starting
            // point for two unrelated objects posed differently on the plate.
            _L("Open the Cut gizmo on this object with the selected object's cut - plane, surface and connectors - "
               "applied in this object's own model space"),
            [i](wxCommandEvent &) { obj_list()->copy_cut_to(i); },
            "", nullptr, []() { return true; }, m_parent);
        ++ targets;
    }

    if (targets == 0) {
        // has_selected_copyable_cut() already guarantees an object exists, so this
        // is belt and braces - but an empty submenu is a dead end, and deleting the
        // menu we just built is cheaper than showing one.
        delete sub;
        return;
    }

    append_submenu(menu, sub, wxID_ANY, menu_name,
                   // "an object", not "another object": the source is listed too, because
                   // a "Cut to parts" cut leaves both halves inside ONE object and the
                   // other side of that same object is the target the user usually wants.
                   _L("Set this cut up on an object, ready to cut it the same way"), "",
                   []() { return true; }, m_parent);
}
void MenuFactory::append_menu_item_invalidate_cut_info(wxMenu *menu)
{
    const wxString menu_name = _L("Invalidate cut info");

    auto menu_item_id = menu->FindItem(menu_name);
    if (menu_item_id != wxNOT_FOUND)
        // Delete old menu item if selected object isn't cut
        menu->Destroy(menu_item_id);

    if (obj_list()->has_selected_cut_object())
        append_menu_item(menu, wxID_ANY, menu_name, "", [](wxCommandEvent &) { obj_list()->invalidate_cut_info_for_selection(); },
            "", menu, []() { return true; }, m_parent);
}

MenuFactory::MenuFactory()
{
    for (int i = 0; i < mtCount; i++) {
        items_increase[i] = nullptr;
        items_decrease[i] = nullptr;
        items_set_number_of_copies[i] = nullptr;
    }
}

void MenuFactory::create_default_menu()
{
    wxMenu* sub_menu_primitives = append_submenu_add_generic(&m_default_menu, ModelVolumeType::INVALID);
    wxMenu* sub_menu_handy = append_submenu_add_handy_model(&m_default_menu, ModelVolumeType::INVALID);
    wxMenu* sub_menu_custom = append_submenu_add_custom_model(&m_default_menu, m_custom_models_default);
#ifdef __WINDOWS__
    append_submenu(&m_default_menu, sub_menu_primitives, wxID_ANY, _L("Add Primitive"), "", "menu_add_part",
        []() {return true; }, m_parent);
    append_submenu(&m_default_menu, sub_menu_handy, wxID_ANY, _L("Add Handy models"), "", "menu_add_part",
        []() {return true; }, m_parent);
    append_submenu(&m_default_menu, sub_menu_custom, wxID_ANY, _L("Add Custom Models"), "", "menu_add_part",
        []() {return true; }, m_parent);
    append_menu_item(&m_default_menu, wxID_ANY, _L("Add Models"), "", // ORCA: Add Models
        [](wxCommandEvent&) { plater()->add_file(); }, "menu_add_part", &m_default_menu,
        []() {return wxGetApp().plater()->can_add_model(); }, m_parent);
#else
    append_submenu(&m_default_menu, sub_menu_primitives, wxID_ANY, _L("Add Primitive"), "", "",
        []() {return true; }, m_parent);
    append_submenu(&m_default_menu, sub_menu_handy, wxID_ANY, _L("Add Handy models"), "", "",
        []() {return true; }, m_parent);
    append_submenu(&m_default_menu, sub_menu_custom, wxID_ANY, _L("Add Custom Models"), "", "",
        []() {return true; }, m_parent);
    append_menu_item(&m_default_menu, wxID_ANY, _L("Add Models"), "", // ORCA: Add Models
        [](wxCommandEvent&) { plater()->add_file(); }, "", &m_default_menu,
        []() {return wxGetApp().plater()->can_add_model(); }, m_parent);
#endif

    m_default_menu.AppendSeparator();

    append_menu_check_item(&m_default_menu, wxID_ANY, _L("Show Labels"), "",
        [](wxCommandEvent&) { plater()->show_view3D_labels(!plater()->are_view3D_labels_shown()); plater()->get_current_canvas3D()->post_event(SimpleEvent(wxEVT_PAINT)); }, &m_default_menu,
        []() { return plater()->is_view3D_shown(); }, [this]() { return plater()->are_view3D_labels_shown(); }, m_parent);
}

void MenuFactory::create_common_object_menu(wxMenu* menu)
{
    append_menu_item_rename(menu);
    // BBS
    //append_menu_items_instance_manipulation(menu);
    // Delete menu was moved to be after +/- instace to make it more difficult to be selected by mistake.
    append_menu_item_delete(menu);
    //append_menu_item_instance_to_object(menu);
    menu->AppendSeparator();

    // BBS
    append_menu_item_reload_from_disk(menu);
    append_menu_item_export_stl(menu);
    append_menu_item_export_step(menu);
    append_menu_item_cad_fillet(menu);
    // "Scale to print volume" makes a sense just for whole object
    append_menu_item_scale_selection_to_fit_print_volume(menu);

    append_menu_item_fix_through_netfabb(menu);
    append_menu_items_mirror(menu);
}

void MenuFactory::create_object_menu()
{
    create_common_object_menu(&m_object_menu);
    wxMenu* split_menu = new wxMenu();
    if (!split_menu)
        return;

    append_menu_item(split_menu, wxID_ANY, _L("To objects"), _L("Split the selected object into multiple objects"),
        [](wxCommandEvent&) { plater()->split_object(); }, "menu_split_objects", &m_object_menu,
        []() { return plater()->can_split(true); }, m_parent);
    append_menu_item(split_menu, wxID_ANY, _L("To parts"), _L("Split the selected object into multiple parts"),
        [](wxCommandEvent&) { plater()->split_volume(); }, "menu_split_parts", &m_object_menu,
        []() { return plater()->can_split(false); }, m_parent);
    append_menu_item(split_menu, wxID_ANY, _L("By painted colour"), _L("Convert the part's colour painting into separate parts, one per filament"),
        [](wxCommandEvent&) { plater()->split_by_color(); }, "menu_split_parts", &m_object_menu,
        []() { return plater()->can_split_by_color(); }, m_parent);

    append_submenu(&m_object_menu, split_menu, wxID_ANY, _L("Split"), _L("Split the selected object"), "",
        []() { return plater()->can_split(true) || plater()->can_split(false) || plater()->can_split_by_color(); }, m_parent);
    m_object_menu.AppendSeparator();

    // BBS: remove Layers Editing
    m_object_menu.AppendSeparator();

    // "Add (volumes)" popupmenu will be added later in append_menu_items_add_volume()
}

void MenuFactory::create_extra_object_menu()
{
    // Object Clone
    append_menu_item_clone(&m_object_menu);
    // Straight to the fill dialog, without going through the Clone dialog's Fill button.
    append_menu_item_fill_bed(&m_object_menu);
    // Ultra: per-object visibility (Normal / Ghost / Hidden)
    append_menu_items_visibility(&m_object_menu);
    // Ultra (support groups): the per-object Support groups window. Non-modal, so it stays open
    // while the user picks the parts a new group is made from.
    append_menu_item(&m_object_menu, wxID_ANY, _L("Support groups..."),
        _L("Name and edit this object's support groups"),
        [](wxCommandEvent&) {
            const int obj_idx = obj_list()->get_selected_obj_idx();
            if (obj_idx >= 0)
                open_support_groups_dialog(plater(), obj_list()->object(obj_idx));
        }, "", &m_object_menu,
        []() { return obj_list()->get_selected_obj_idx() >= 0; }, m_parent);
    // Object Repair
    append_menu_item_fix_through_netfabb(&m_object_menu);
    // Object Simplify
    append_menu_item_simplify(&m_object_menu);
    // Ultra (slice baking, phase 1): turn the SLICED outer wall - fuzzy skin and all - into
    // a mesh that can be re-sliced. Enabled only once the object has perimeters to bake.
    // docs/superpowers/specs/2026-09-12-slice-bake-research.md
    append_menu_item(&m_object_menu, wxID_ANY, _L("Bake slice to mesh..."),
        _L("Rebuild this object as the outer wall the slicer will actually print, so it can be re-sliced at another layer height"),
        [](wxCommandEvent&) { obj_list()->bake_slice_to_mesh(); }, "", &m_object_menu,
        []() { return ObjectList::can_bake_slice_to_mesh(); }, m_parent);
    // Side stabilizers as real geometry, so they print in other slicers too. Enabled once the object is
    // sliced with its side stabilizers on. tests/research_stabilizer_bake.md
    append_menu_item(&m_object_menu, wxID_ANY, _L("Bake stabilizers..."),
        _L("Turn this object's side stabilizers into real geometry, as a separate object or a part, so they print in any slicer"),
        [](wxCommandEvent&) { obj_list()->bake_stabilizers(); }, "", &m_object_menu,
        []() { return ObjectList::can_bake_stabilizers(); }, m_parent);
    // Image Fill (Phase 2): on the object menu too - a single-part object never opens the part menu.
    append_menu_item_image_fill(&m_object_menu);
    // merge to single part
    append_menu_item_merge_parts_to_single_part(&m_object_menu);
    // Object Center
    append_menu_item_center(&m_object_menu);
    // Object Drop
    append_menu_item_drop(&m_object_menu);
    // The BBL object menu is built here, not by create_common_object_menu, so the upstream
    // "Scale to build volume" entry never reached it.
    append_menu_item_scale_selection_to_fit_print_volume(&m_object_menu);
    // Object Split
    wxMenu* split_menu = new wxMenu();
    if (!split_menu)
        return;
    append_menu_item(split_menu, wxID_ANY, _L("To objects"), _L("Split the selected object into multiple objects"),
        [](wxCommandEvent&) { plater()->split_object(); }, "menu_split_objects", &m_object_menu,
        []() { return plater()->can_split(true); }, m_parent);
    append_menu_item(split_menu, wxID_ANY, _L("To parts"), _L("Split the selected object into multiple parts"),
        [](wxCommandEvent&) { plater()->split_volume(); }, "menu_split_parts", &m_object_menu,
        []() { return plater()->can_split(false); }, m_parent);
    append_menu_item(split_menu, wxID_ANY, _L("By painted colour"), _L("Convert the part's colour painting into separate parts, one per filament"),
        [](wxCommandEvent&) { plater()->split_by_color(); }, "menu_split_parts", &m_object_menu,
        []() { return plater()->can_split_by_color(); }, m_parent);

    append_submenu(&m_object_menu, split_menu, wxID_ANY, _L("Split"), _L("Split the selected object"), "",
        []() { return plater()->can_split(true) || plater()->can_split_by_color(); }, m_parent);

    // Mirror
    append_menu_items_mirror(&m_object_menu);
    // Delete
    append_menu_item_delete(&m_object_menu);
    m_object_menu.AppendSeparator();
    // Modifier Part
    // BBS
    append_menu_items_add_volume(&m_object_menu);
    m_object_menu.AppendSeparator();
    // Set filament insert menu item here
    // Set Printable
    wxMenuItem* menu_item_printable = append_menu_item_printable(&m_object_menu);
    append_menu_item_per_object_process(&m_object_menu);
    // Enter per object parameters
    append_menu_item_per_object_settings(&m_object_menu);
    m_object_menu.AppendSeparator();
    append_menu_item_reload_from_disk(&m_object_menu);
    append_menu_item_replace_with_stl(&m_object_menu);
    append_menu_item_edit_in_blender(&m_object_menu);
    append_menu_item_edit_in_freecad(&m_object_menu);
    append_menu_item_cad_fillet(&m_object_menu);
    append_menu_item_export_stl(&m_object_menu);
    append_menu_item_export_step(&m_object_menu);
    append_menu_item_save_custom_model(&m_object_menu);
}

void MenuFactory::create_bbl_assemble_object_menu()
{
    // Delete
    append_menu_item_delete(&m_assemble_object_menu);
    // Object Repair
    append_menu_item_fix_through_netfabb(&m_assemble_object_menu);
    // Object Simplify
    append_menu_item_simplify(&m_assemble_object_menu);
    m_assemble_object_menu.AppendSeparator();
}

void MenuFactory::create_sla_object_menu()
{
    create_common_object_menu(&m_sla_object_menu);
    append_menu_item(&m_sla_object_menu, wxID_ANY, _L("Split"), _L("Split the selected object into multiple objects"),
        [](wxCommandEvent&) { plater()->split_object(); }, "", nullptr,
        []() { return plater()->can_split(true); }, m_parent);

    m_sla_object_menu.AppendSeparator();

    // Add the automatic rotation sub-menu
    append_menu_item(&m_sla_object_menu, wxID_ANY, _L("Auto orientation"), _L("Auto orient the object to improve print quality."),
        [](wxCommandEvent&) { plater()->optimize_rotation(); });
}

void MenuFactory::create_part_menu()
{
    wxMenu* menu = &m_part_menu;
    append_menu_item_rename(menu);
    append_menu_item_delete(menu);
    append_menu_item_reload_from_disk(menu);
    append_menu_item_export_stl(menu);
    append_menu_item_cad_fillet(menu);
    append_menu_item_export_stl_part(menu);
    append_menu_item_fix_through_netfabb(menu);
    append_menu_items_mirror(menu);
    // Ultra: per-part visibility inside assemblies
    append_menu_items_visibility(menu);
    // "Separate": extract this one part into its own object, keeping its position
    // (single-part variant of Assemble Separately; same implementation).
    append_menu_item(menu, wxID_ANY, _L("Separate"), _L("Move this part into a new object, keeping its position in space"),
        [](wxCommandEvent&) { obj_list()->assemble_separately(); }, "", nullptr,
        []() { return obj_list()->can_assemble_separately(); }, m_parent);
    append_menu_item_merge_parts_to_single_part(menu);

    append_menu_item(menu, wxID_ANY, _L("Split"), _L("Split the selected object into multiple parts"),
        [](wxCommandEvent&) { plater()->split_volume(); }, "split_parts", nullptr,
        []() { return plater()->can_split(false); }, m_parent);
    append_menu_item(menu, wxID_ANY, _L("Split by painted colour"), _L("Convert the part's colour painting into separate parts, one per filament"),
        [](wxCommandEvent&) { plater()->split_by_color(); }, "split_parts", nullptr,
        []() { return plater()->can_split_by_color(); }, m_parent);
    append_menu_item_image_fill(menu);

    menu->AppendSeparator();
    append_menu_item_change_type(menu);
    append_menu_items_mirror(&m_part_menu);
    append_menu_item(&m_part_menu, wxID_ANY, _L("Split"), _L("Split the selected object into multiple parts"),
        [](wxCommandEvent&) { plater()->split_volume(); }, "split_parts", nullptr,
        []() { return plater()->can_split(false); }, m_parent);
    m_part_menu.AppendSeparator();
    append_menu_item_per_object_settings(&m_part_menu);
}

void MenuFactory::create_text_part_menu()
{
    wxMenu* menu = &m_text_part_menu;

    append_menu_item_edit_text(menu);
    append_menu_item_delete(menu);
    append_menu_item_fix_through_netfabb(menu);
    append_menu_item_simplify(menu);
    append_menu_items_mirror(menu);
    append_menu_item_export_stl_part(menu);
    menu->AppendSeparator();
    append_menu_item_per_object_settings(menu);
    append_menu_item_change_type(menu);
}

void MenuFactory::create_svg_part_menu()
{
    wxMenu* menu = &m_svg_part_menu;

    append_menu_item_edit_svg(menu);
    append_menu_item_delete(menu);
    append_menu_item_fix_through_netfabb(menu);
    append_menu_item_simplify(menu);
    append_menu_items_mirror(menu);
    append_menu_item_export_stl_part(menu);
    menu->AppendSeparator();
    append_menu_item_per_object_settings(menu);
    append_menu_item_change_type(menu);
}

void MenuFactory::create_bbl_part_menu()
{
    wxMenu* menu = &m_part_menu;

    append_menu_item_delete(menu);
    append_menu_item_edit_text(menu);
    append_menu_item_fix_through_netfabb(menu);
    append_menu_item_simplify(menu);
    append_menu_item_center(menu);
    append_menu_item_drop(menu);
    append_menu_items_mirror(menu);
    // Ultra: per-part visibility inside assemblies
    append_menu_items_visibility(menu);
    // "Separate": pull this one part out of the assembly into its own object, keeping its
    // position (single-part variant of Assemble Separately; same implementation). This is the
    // part menu the object list actually shows - create_part_menu() is not used.
    append_menu_item(menu, wxID_ANY, _L("Separate"), _L("Move this part into a new object, keeping its position in space"),
        [](wxCommandEvent&) { obj_list()->assemble_separately(); }, "", nullptr,
        []() { return obj_list()->can_assemble_separately(); }, m_parent);
    wxMenu* split_menu = new wxMenu();
    if (!split_menu)
        return;

    append_menu_item(split_menu, wxID_ANY, _L("To objects"), _L("Split the selected object into multiple objects"),
        [](wxCommandEvent&) { plater()->split_object(); }, "menu_split_objects", menu,
        []() { return plater()->can_split(true); }, m_parent);
    append_menu_item(split_menu, wxID_ANY, _L("To parts"), _L("Split the selected object into multiple parts"),
        [](wxCommandEvent&) { plater()->split_volume(); }, "menu_split_parts", menu,
        []() { return plater()->can_split(false); }, m_parent);
    append_menu_item(split_menu, wxID_ANY, _L("By painted colour"), _L("Convert the part's colour painting into separate parts, one per filament"),
        [](wxCommandEvent&) { plater()->split_by_color(); }, "menu_split_parts", menu,
        []() { return plater()->can_split_by_color(); }, m_parent);

    append_submenu(menu, split_menu, wxID_ANY, _L("Split"), _L("Split the selected object"), "",
        []() { return plater()->can_split(true) || plater()->can_split_by_color(); }, m_parent);
    menu->AppendSeparator();
    append_menu_item_per_object_settings(menu);
    append_menu_item_change_type(menu);
    append_menu_item_reload_from_disk(menu);
    append_menu_item_replace_with_stl(menu);
    append_menu_item_edit_in_blender(menu);
    append_menu_item_edit_in_freecad(menu);
    append_menu_item_cad_fillet(menu);
    append_menu_item_export_stl_part(menu);
}

void MenuFactory::create_bbl_assemble_part_menu()
{
    wxMenu* menu = &m_assemble_part_menu;

    append_menu_item_delete(menu);
    append_menu_item_simplify(menu);
    menu->AppendSeparator();
}

void MenuFactory::create_filament_action_menu(bool init, int active_filament_menu_id)
{
    wxMenu* menu = &m_filament_action_menu;

    if (init) {
        append_menu_item(
            menu, wxID_ANY, _L("Edit"), "", [](wxCommandEvent&) { plater()->sidebar().edit_filament(); }, "", nullptr,
            []() { return true; }, m_parent);
    }

    if (init) {
        append_menu_item(
            menu, wxID_ANY, _L("Delete"), _L("Delete this filament"), [](wxCommandEvent&) { plater()->sidebar().delete_filament(-2); }, "",
            nullptr, []() { return plater()->sidebar().combos_filament().size() > 1; }, m_parent);
    }

    if (wxGetApp().preset_bundle == nullptr)
        return;

    const int item_id = menu->FindItem(_L("Merge with"));
    if (item_id != wxNOT_FOUND)
        menu->Destroy(item_id);

    wxMenu *                      sub_menu         = new wxMenu();
    std::vector<wxBitmap *>       icons            = get_extruder_color_icons(true);
    const std::vector<std::string> &filament_presets = wxGetApp().preset_bundle->filament_presets;
    int                            filaments_cnt    = std::max(wxGetApp().filaments_cnt(), 0);
    filaments_cnt = std::min(filaments_cnt, static_cast<int>(icons.size()));
    filaments_cnt = std::min(filaments_cnt, static_cast<int>(filament_presets.size()));

    // Add physical filaments as merge targets
    for (int i = 0; i < filaments_cnt; i++) {
        if (i == active_filament_menu_id)
            continue;

        auto     preset    = wxGetApp().preset_bundle->filaments.find_preset(filament_presets[i]);
        wxString item_name = preset ? from_u8(preset->label(false)) : wxString::Format(_L("Filament %d"), i + 1);

        append_menu_item(
            sub_menu, wxID_ANY, item_name, "", [i](wxCommandEvent&) { plater()->sidebar().change_filament(-2, i); }, *icons[i], menu,
            []() { return true; }, m_parent);
    }

    // Add mixed filaments as merge targets
    auto& mixed_mgr = wxGetApp().preset_bundle->mixed_filaments;
    const auto& mfs = mixed_mgr.mixed_filaments();
    const size_t num_physical = filament_presets.size();
    
    // Get icon dimensions
    const double em = Slic3r::GUI::wxGetApp().em_unit();
    const int icon_width = lround(2 * em);
    const int icon_height = lround(2 * em);
    
    // Determine the source physical filament ID (if active_filament_menu_id is a physical filament)
    unsigned int source_physical_1based = 0;
    if (active_filament_menu_id >= 0 && active_filament_menu_id < (int)num_physical) {
        source_physical_1based = (unsigned int)(active_filament_menu_id + 1);
    }
    
    // Get indices of mixed filaments that depend on the source physical filament
    std::vector<size_t> dependent_mixed_indices;
    if (source_physical_1based > 0) {
        dependent_mixed_indices = mixed_mgr.mixed_filaments_using_physical(source_physical_1based);
    }
    
    size_t visible_idx = 0;
    size_t running_idx = 0;  // counts all non-deleted entries for virtual ID calculation
    for (size_t j = 0; j < mfs.size(); ++j) {
        if (mfs[j].deleted || !mfs[j].enabled) continue;

        // Calculate virtual ID for this mixed filament based on all non-deleted entries
        size_t mixed_virtual_id = num_physical + running_idx;
        running_idx++;

        // Skip active mixed filament (consistent with physical filaments)
        if (mixed_virtual_id == (size_t)active_filament_menu_id) {
            continue;
        }

        // Skip mixed filaments that depend on the source physical filament
        // This prevents merging a physical filament into a mixed filament that uses it as a component
        if (std::find(dependent_mixed_indices.begin(), dependent_mixed_indices.end(), j) != dependent_mixed_indices.end()) {
            continue;
        }
        
        const int virtual_id = static_cast<int>(mixed_virtual_id) + 1;
        wxString item_name = wxString::Format(_L("Mixed Filament %d"), virtual_id);
        
        // Create a colored bitmap for the mixed filament — gradient filaments get a gradient icon
        MixedFilamentDisplayContext menu_ctx;
        {
            auto* co2 = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
            menu_ctx.physical_colors = co2 ? co2->values : std::vector<std::string>{};
            menu_ctx.num_physical = num_physical;
        }
        wxBitmap* mixed_bmp = create_mixed_filament_menu_bitmap(
            mfs[j], menu_ctx, icon_width, icon_height,
            wxString::Format("%d", virtual_id));

        size_t captured_target = mixed_virtual_id;
        append_menu_item(
            sub_menu, wxID_ANY, item_name, "", [captured_target](wxCommandEvent&) {
                plater()->sidebar().change_filament(-2, captured_target);
            }, *mixed_bmp, menu,
            []() { return true; }, m_parent);
        
        visible_idx++;
    }

    // Show merge menu if there are any targets available
    size_t total_targets = filaments_cnt + visible_idx;
    append_submenu(
        menu, sub_menu, wxID_ANY, _L("Merge with"), "", "", [total_targets, active_filament_menu_id]() { 
            return total_targets > 1 || (total_targets == 1 && active_filament_menu_id < 0); 
        }, m_parent);
}

//BBS: add part plate related logic
void MenuFactory::create_plate_menu()
{
    wxMenu* menu = &m_plate_menu;
    // select objects on current plate
    append_menu_item(menu, wxID_ANY, _L("Select All"), _L("select all objects on current plate"),
        [](wxCommandEvent&) {
            plater()->select_curr_plate_all();
        }, "", nullptr, []() {
            PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
            assert(plate);
            return !plate->get_objects().empty();
        }, m_parent);

    // delete objects on current plate
    append_menu_item(menu, wxID_ANY, _L("Delete All"), _L("delete all objects on current plate"),
        [](wxCommandEvent&) {
            plater()->remove_curr_plate_all();
        }, "", nullptr, []() {
            PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
            assert(plate);
            return !plate->get_objects().empty();
        }, m_parent);

    // arrange objects on current plate
    append_menu_item(menu, wxID_ANY, _L("Arrange"), _L("arrange current plate"),
        [](wxCommandEvent&) {
            PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
            assert(plate);
            plater()->set_prepare_state(Job::PREPARE_STATE_MENU);
            plater()->arrange();
        }, "", nullptr,
        []() {
            return !plater()->get_partplate_list().get_selected_plate()->get_objects().empty();
        },
        m_parent);

    // reload all objects on current plate
    append_menu_item(
        menu, wxID_ANY, _L("Reload All"), _L("reload all from disk"),
        [](wxCommandEvent&) {
            PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
            assert(plate);
            plater()->set_prepare_state(Job::PREPARE_STATE_MENU);
            plater()->reload_all_from_disk();
        },
        "", nullptr, []() { return !plater()->get_partplate_list().get_selected_plate()->get_objects().empty(); }, m_parent);

    // orient objects on current plate
    append_menu_item(menu, wxID_ANY, _L("Auto Rotate"), _L("auto rotate current plate"),
        [](wxCommandEvent&) {
            PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
            assert(plate);
            //BBS TODO call auto rotate for current plate
            plater()->set_prepare_state(Job::PREPARE_STATE_MENU);
            plater()->orient();
        }, "", nullptr,
        []() {
            return !plater()->get_partplate_list().get_selected_plate()->get_objects().empty();
        }, m_parent);

    // delete current plate
#ifdef __WINDOWS__
    append_menu_item(menu, wxID_ANY, _L("Delete Plate"), _L("Remove the selected plate"),
        [](wxCommandEvent&) { plater()->delete_plate(); }, "menu_delete", nullptr,
        []() { return plater()->can_delete_plate(); }, m_parent);
#else
    append_menu_item(menu, wxID_ANY, _L("Delete Plate"), _L("Remove the selected plate"),
        [](wxCommandEvent&) { plater()->delete_plate(); }, "", nullptr,
        []() { return plater()->can_delete_plate(); }, m_parent);
#endif


    // add shapes
    menu->AppendSeparator();
    wxMenu* sub_menu_primitives = append_submenu_add_generic(menu, ModelVolumeType::INVALID);
    wxMenu* sub_menu_handy = append_submenu_add_handy_model(menu, ModelVolumeType::INVALID);
    wxMenu* sub_menu_custom = append_submenu_add_custom_model(menu, m_custom_models_plate);

#ifdef __WINDOWS__
    append_submenu(menu, sub_menu_primitives, wxID_ANY, _L("Add Primitive"), "", "menu_add_part",
        []() {return true; }, m_parent);
    append_submenu(menu, sub_menu_handy, wxID_ANY, _L("Add Handy models"), "", "menu_add_part",
        []() {return true; }, m_parent);
    append_submenu(menu, sub_menu_custom, wxID_ANY, _L("Add Custom Models"), "", "menu_add_part",
        []() {return true; }, m_parent);
    append_menu_item(menu, wxID_ANY, _L("Add Models"), "", // ORCA: Add Models
        [](wxCommandEvent&) { plater()->add_file(); }, "menu_add_part", menu,
        []() {return wxGetApp().plater()->can_add_model(); }, m_parent);
#else
    append_submenu(menu, sub_menu_primitives, wxID_ANY, _L("Add Primitive"), "", "",
        []() {return true; }, m_parent);
    append_submenu(menu, sub_menu_handy, wxID_ANY, _L("Add Handy models"), "", "",
        []() {return true; }, m_parent);
    append_submenu(menu, sub_menu_custom, wxID_ANY, _L("Add Custom Models"), "", "",
        []() {return true; }, m_parent);
    append_menu_item(menu, wxID_ANY, _L("Add Models"), "", // ORCA: Add Models
        [](wxCommandEvent&) { plater()->add_file(); }, "", menu,
        []() {return wxGetApp().plater()->can_add_model(); }, m_parent);
#endif


    return;
}

void MenuFactory::init(wxWindow* parent)
{
    m_parent = parent;

    create_default_menu();
    //BBS
    //create_object_menu();
    create_sla_object_menu();
    //create_part_menu();
    create_text_part_menu();
    create_svg_part_menu();
    create_extra_object_menu();
    create_bbl_part_menu();
    create_bbl_assemble_object_menu();
    create_bbl_assemble_part_menu();

    //BBS: add part plate related logic
    create_plate_menu();

    create_filament_action_menu(true, -1);

    // create "Instance to Object" menu item
    append_menu_item_instance_to_object(&m_instance_menu);
}

void MenuFactory::update()
{
    update_default_menu();
    update_object_menu();
}

wxMenu* MenuFactory::default_menu()
{
    // Called right before the menu is shown: pick up files added to the Custom Models folder.
    if (m_custom_models_default)
        m_custom_models_default->refresh();
    return &m_default_menu;
}

wxMenu* MenuFactory::object_menu()
{
    append_menu_items_convert_unit(&m_object_menu);
    append_menu_items_flush_options(&m_object_menu);
    append_menu_item_edit_cut(&m_object_menu);
    append_menu_item_copy_cut(&m_object_menu);
    append_menu_item_invalidate_cut_info(&m_object_menu);
    append_menu_item_edit_text(&m_object_menu);
    append_menu_item_edit_svg(&m_object_menu);
    append_menu_item_change_filament(&m_object_menu);
    return &m_object_menu;
}

wxMenu* MenuFactory::sla_object_menu()
{
    append_menu_items_convert_unit(&m_sla_object_menu);
    append_menu_item_settings(&m_sla_object_menu);
    //update_menu_items_instance_manipulation(mtObjectSLA);
    append_menu_item_edit_text(&m_sla_object_menu);
    append_menu_item_edit_svg(&m_object_menu);

    return &m_sla_object_menu;
}

wxMenu* MenuFactory::part_menu()
{
    append_menu_items_convert_unit(&m_part_menu);
    append_menu_item_change_filament(&m_part_menu);
    // Ultra (support groups): rebuilt per open, like Change Filament above it.
    append_menu_items_support_group(&m_part_menu);
    append_menu_item_per_object_settings(&m_part_menu);
    append_menu_item_precise_seam_submenu(&m_part_menu);
    return &m_part_menu;
}

wxMenu* MenuFactory::text_part_menu()
{
    append_menu_item_change_filament(&m_text_part_menu);
    append_menu_item_per_object_settings(&m_text_part_menu);

    return &m_text_part_menu;
}

wxMenu *MenuFactory::svg_part_menu()
{
    append_menu_item_change_filament(&m_svg_part_menu);
    append_menu_item_per_object_settings(&m_svg_part_menu);

    return &m_svg_part_menu;
}

wxMenu* MenuFactory::instance_menu()
{
    return &m_instance_menu;
}

wxMenu* MenuFactory::layer_menu()
{
    MenuWithSeparators* menu = new MenuWithSeparators();
    append_menu_item_settings(menu);

    return menu;
}

wxMenu* MenuFactory::multi_selection_menu()
{
    //BBS
    wxDataViewItemArray sels;
    obj_list()->GetSelections(sels);
    bool multi_volume = true;

    for (const wxDataViewItem& item : sels) {
        multi_volume = list_model()->GetItemType(item) & itVolume;
        if (!(list_model()->GetItemType(item) & (itVolume | itObject | itInstance)))
            // show this menu only for Objects(Instances mixed with Objects)/Volumes selection
            return nullptr;
    }

    wxMenu* menu = new MenuWithSeparators();
    if (!multi_volume) {
        int index = 0;
        if (obj_list()->can_merge_to_multipart_object()) {
            append_menu_item_merge_to_multipart_object(menu);
            // Ultra: auto-assemble by best-fit faces/holes (largest = fixed target), then merge to one part.
            append_menu_item(menu, wxID_ANY, _L("Auto-Fit Assembly"),
                _L("Auto-assemble the selected objects: the largest stays fixed as the target, the rest mate into it by best-fit faces/holes, then merge into one multi-part object"),
                [](wxCommandEvent&) { plater()->ultra_auto_assemble(); }, "", menu,
                []() { return true; }, m_parent);
            index++;
        }
        append_menu_item_center(menu);
        append_menu_item_drop(menu);
        append_menu_item_fix_through_netfabb(menu);
        //append_menu_item_simplify(menu);
        append_menu_item_delete(menu);
        menu->AppendSeparator();

        append_menu_item_set_printable(menu);
        append_menu_item_per_object_process(menu);
        // Ultra: visibility for multi-object selections
        append_menu_items_visibility(menu);
        menu->AppendSeparator();
        append_menu_items_convert_unit(menu);
        //BBS
        append_menu_item_change_filament(menu);
        menu->AppendSeparator();
        append_menu_item_export_stl(menu, true);
        append_menu_item_export_step(menu);
        append_menu_item_save_custom_model(menu);
    }
    else {
        append_menu_item_center(menu);
        append_menu_item_drop(menu);
        append_menu_item_assemble_separately(menu);
        append_menu_item_merge_some_parts_to_single_part(menu);
        // Ultra: visibility for multi-part selections
        append_menu_items_visibility(menu);
        // Ultra (support groups): one assignment for the whole multi-part selection
        append_menu_items_support_group(menu);
        append_menu_item_fix_through_netfabb(menu);
        //append_menu_item_simplify(menu);
        append_menu_item_delete(menu);
        append_menu_items_convert_unit(menu);
        append_menu_item_change_filament(menu);
        wxMenu* split_menu = new wxMenu();
        if (split_menu) {
            append_menu_item(split_menu, wxID_ANY, _L("To objects"), _L("Split the selected object into multiple objects"),
                [](wxCommandEvent&) { plater()->split_object(); }, "menu_split_objects", menu,
                []() { return plater()->can_split(true); }, m_parent);
            append_menu_item(split_menu, wxID_ANY, _L("To parts"), _L("Split the selected object into multiple parts"),
                [](wxCommandEvent&) { plater()->split_volume(); }, "menu_split_parts", menu,
                []() { return plater()->can_split(false); }, m_parent);
            append_menu_item(split_menu, wxID_ANY, _L("By painted colour"), _L("Convert the part's colour painting into separate parts, one per filament"),
                [](wxCommandEvent&) { plater()->split_by_color(); }, "menu_split_parts", menu,
                []() { return plater()->can_split_by_color(); }, m_parent);

            append_submenu(menu, split_menu, wxID_ANY, _L("Split"), _L("Split the selected object"), "",
                []() { return plater()->can_split(true) || plater()->can_split_by_color(); }, m_parent);
        }
        menu->AppendSeparator();
        append_menu_item_change_filament(menu);
        append_menu_item_change_type(menu);
        append_menu_item_precise_seam_submenu(menu);
    }
    return menu;
}

wxMenu* MenuFactory::assemble_multi_selection_menu()
{
    wxDataViewItemArray sels;
    obj_list()->GetSelections(sels);

    for (const wxDataViewItem& item : sels)
        if (!(list_model()->GetItemType(item) & (itVolume | itObject | itInstance)))
            // show this menu only for Objects(Instances mixed with Objects)/Volumes selection
            return nullptr;

    wxMenu* menu = new MenuWithSeparators();
    append_menu_item_set_visible(menu);
    //append_menu_item_fix_through_netfabb(menu);
    //append_menu_item_simplify(menu);
    append_menu_item_delete(menu);
    menu->AppendSeparator();
    append_menu_item_change_extruder(menu);
    return menu;
}


//BBS: add partplate related logic
wxMenu* MenuFactory::plate_menu()
{
    if (m_custom_models_plate)
        m_custom_models_plate->refresh();
    append_menu_item_locked(&m_plate_menu);
    append_menu_item_plate_name(&m_plate_menu);
    return &m_plate_menu;
}

wxMenu* MenuFactory::filament_action_menu(int active_filament_menu_id) {
    create_filament_action_menu(false, active_filament_menu_id);
    return &m_filament_action_menu;
}

wxMenu* MenuFactory::assemble_object_menu()
{
    wxMenu* menu = new MenuWithSeparators();
    // Set Visible
    append_menu_item_set_visible(menu);
    // Delete
    append_menu_item_delete(menu);
    //// Object Repair
    //append_menu_item_fix_through_netfabb(menu);
    //// Object Simplify
    //append_menu_item_simplify(menu);
    menu->AppendSeparator();

    // Set filament
    append_menu_item_change_extruder(menu);
    //// Enter per object parameters
    //append_menu_item_per_object_settings(menu);
    return menu;
}

wxMenu* MenuFactory::assemble_part_menu()
{
    wxMenu* menu = new MenuWithSeparators();

    append_menu_item_set_visible(menu);
    append_menu_item_delete(menu);
    //append_menu_item_simplify(menu);
    menu->AppendSeparator();

    append_menu_item_change_extruder(menu);
    //append_menu_item_per_object_settings(menu);
    return menu;
}

void MenuFactory::append_menu_item_clone(wxMenu* menu)
{
#ifdef __APPLE__
    static const wxString ctrl = ("Ctrl+");
#else
    // FIXME: maybe should be using GUI::shortkey_ctrl_prefix() or equivalent?
    static const wxString ctrl = _L("Ctrl+");
#endif
    append_menu_item(menu, wxID_ANY, _L("Clone") + "\t" + ctrl + "K", "",
        [this](wxCommandEvent&) {
            plater()->clone_selection();
        }, "", nullptr,
        []() {
            return true;
        }, m_parent);
}

void MenuFactory::append_menu_item_simplify(wxMenu* menu)
{
    wxMenuItem* menu_item = append_menu_item(menu, wxID_ANY, _L("Simplify Model"), "",
        [](wxCommandEvent&) { obj_list()->simplify(); }, "", menu,
        []() {return plater()->can_simplify(); }, m_parent);
}

void MenuFactory::append_menu_item_image_fill(wxMenu* menu)
{
    append_menu_item(menu, wxID_ANY, _L("Apply image fill..."), _L("Put an image or a gradient on this part, printed with the filaments you allow"),
        [](wxCommandEvent&) { plater()->apply_image_fill(); }, "", menu,
        []() { return plater()->can_apply_image_fill(); }, m_parent);
}

void MenuFactory::append_menu_item_center(wxMenu* menu)
{
     append_menu_item(menu, wxID_ANY, _L("Center") , "",
        [this](wxCommandEvent&) {
            plater()->center_selection();
        }, "", nullptr,
        []() {
            if (plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasView3D)
                return false;
            else {
                Selection& selection = plater()->get_view3D_canvas3D()->get_selection();
                PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
                Vec3d model_pos = selection.get_bounding_box().center();
                Vec3d center_pos = plate->get_center_origin();
                return !( (model_pos.x() == center_pos.x()) && (model_pos.y() == center_pos.y()) );
            } //disable if model is at center / not in View3D
        }, m_parent);
}

void MenuFactory::append_menu_item_drop(wxMenu* menu)
{
     append_menu_item(menu, wxID_ANY, _L("Drop") , "",
        [this](wxCommandEvent&) {
            plater()->drop_selection();
        }, "", nullptr,
        []() {
            if (plater()->canvas3D()->get_canvas_type() != GLCanvas3D::ECanvasType::CanvasView3D)
                return false;
            else {
                return (plater()->get_view3D_canvas3D()->get_selection().get_bounding_box().min.z() != 0);
            } //disable if model is on the bed / not in View3D
        }, m_parent);
}

void MenuFactory::append_menu_item_per_object_process(wxMenu* menu)
{
    const std::vector<wxString> names = { _L("Edit Process Settings"), _L("Edit Process Settings") };
    append_menu_item(menu, wxID_ANY, names[0], names[1],
        [](wxCommandEvent&) {
            wxGetApp().obj_list()->switch_to_object_process();
        }, "", nullptr,
        []() {
            Selection& selection = plater()->canvas3D()->get_selection();
            return selection.is_single_full_object() ||
                selection.is_multiple_full_object() ||
                selection.is_single_full_instance() ||
                selection.is_multiple_full_instance() ||
                selection.is_single_volume() ||
                selection.is_multiple_volume();
        }, m_parent);
}

void MenuFactory::append_menu_item_per_object_settings(wxMenu* menu)
{
    const std::vector<wxString> names = { _L("Edit in Parameter Table"), _L("Edit print parameters for a single object") };
    // Delete old menu item
    for (const wxString& name : names) {
        const int item_id = menu->FindItem(name);
        if (item_id != wxNOT_FOUND)
            menu->Destroy(item_id);
    }

    append_menu_item(menu, wxID_ANY, names[0], names[1],
        [](wxCommandEvent&) {
            plater()->PopupObjectTableBySelection();
        }, "", nullptr,
        []() {
            Selection& selection = plater()->canvas3D()->get_selection();
            return selection.is_single_full_object() || selection.is_single_full_instance() || selection.is_single_volume();
        }, m_parent);
}

void MenuFactory::append_menu_item_change_filament(wxMenu* menu)
{
    const std::vector<wxString> names = { _L("Change Filament"), _L("Set Filament for selected items") };
    // Delete old menu item
    for (const wxString& name : names) {
        const int item_id = menu->FindItem(name);
        if (item_id != wxNOT_FOUND)
            menu->Destroy(item_id);
    }

    const std::vector<unsigned int> ordered_filament_ids = ui_ordered_filament_ids();
    if (ordered_filament_ids.size() <= 1)
        return;

    wxDataViewItemArray sels;
    obj_list()->GetSelections(sels);
    if (sels.IsEmpty())
        return;

    if (sels.Count() == 1) {
        const auto sel_vol = obj_list()->get_selected_model_volume();
        if (sel_vol && sel_vol->type() != ModelVolumeType::MODEL_PART && sel_vol->type() != ModelVolumeType::PARAMETER_MODIFIER)
            return;
    }

    std::vector<wxBitmap*> icons = get_extruder_color_icons(true);
    if (icons.size() < ordered_filament_ids.size()) {
        BOOST_LOG_TRIVIAL(warning) << boost::format("Warning: icons size %1%, filaments_cnt=%2%") % icons.size() % ordered_filament_ids.size();
        if (icons.size() <= 1)
            return;
    }
    wxMenu* extruder_selection_menu = new wxMenu();
    const wxString& name = sels.Count() == 1 ? names[0] : names[1];

    int initial_extruder = -1; // negative value for multiple object/part selection
    if (sels.Count() == 1) {
        const ModelConfig& config = obj_list()->get_item_config(sels[0]);
        // BBS
        const auto sel_vol = obj_list()->get_selected_model_volume();
        if (sel_vol && sel_vol->type() == ModelVolumeType::PARAMETER_MODIFIER)
            initial_extruder = config.has("extruder") ? config.extruder() : 0;
        else
            initial_extruder = config.has("extruder") ? config.extruder() : 0;
    }

    for (size_t display_idx = 0; display_idx <= ordered_filament_ids.size(); ++display_idx)
    {
        const int actual_filament_id = display_idx == 0 ? 0 : int(ordered_filament_ids[display_idx - 1]);
        bool is_active_extruder = false;

        wxString item_name = filament_menu_item_name(actual_filament_id, int(display_idx));

        if (is_active_extruder) {
            item_name << " (" + _L("current") + ")";
        }

        append_menu_item(extruder_selection_menu, wxID_ANY, item_name, "",
            [actual_filament_id](wxCommandEvent&) { obj_list()->set_extruder_for_selected_items(actual_filament_id); },
            actual_filament_id == 0 || size_t(actual_filament_id - 1) >= icons.size() ? wxNullBitmap : *icons[size_t(actual_filament_id - 1)], menu,
            [is_active_extruder]() { return !is_active_extruder; }, m_parent);
    }
    menu->Append(wxID_ANY, name, extruder_selection_menu, _L("Change Filament"));
}

void MenuFactory::append_menu_item_set_printable(wxMenu* menu)
{
    const Selection& selection = plater()->canvas3D()->get_selection();
    bool all_printable = true;
    ObjectList* list = obj_list();
    wxDataViewItemArray sels;
    list->GetSelections(sels);

    for (wxDataViewItem item : sels) {
        ItemType type = list->GetModel()->GetItemType(item);
        if (type != itInstance && type != itObject)
            continue;
        else {
            int obj_idx = list->GetModel()->GetObjectIdByItem(item);
            int inst_idx = type == itObject ? 0 : list->GetModel()->GetInstanceIdByItem(item);
            all_printable &= list->object(obj_idx)->instances[inst_idx]->printable;
        }
    }

    wxString menu_text = _L("Printable");
    wxMenuItem* menu_item_set_printable = append_menu_check_item(menu, wxID_ANY, menu_text, "", [this, all_printable](wxCommandEvent&) {
        Selection& selection = plater()->canvas3D()->get_selection();
        selection.set_printable(!all_printable);
        }, menu);
    m_parent->Bind(wxEVT_UPDATE_UI, [all_printable](wxUpdateUIEvent& evt) {
        evt.Check(all_printable);
        plater()->set_current_canvas_as_dirty();

        }, menu_item_set_printable->GetId());
}

void MenuFactory::append_menu_item_locked(wxMenu* menu)
{
    const std::vector<wxString> names = { _L("Unlock"), _L("Lock") };
    // Delete old menu item
    for (const wxString& name : names) {
        const int item_id = menu->FindItem(name);
        if (item_id != wxNOT_FOUND)
            menu->Destroy(item_id);
    }

    PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
    assert(plate);
    wxString lock_text = plate->is_locked() ? names[0] : names[1];

    auto item = append_menu_item(menu, wxID_ANY, lock_text, "",
        [plate](wxCommandEvent&) {
            bool lock = plate->is_locked();
            plate->lock(!lock);
        }, "", nullptr, []() { return true; }, m_parent);

    m_parent->Bind(wxEVT_UPDATE_UI, [](wxUpdateUIEvent& evt) {
        PartPlate* plate = plater()->get_partplate_list().get_selected_plate();
        assert(plate);
        //bool check = plate->is_locked();
        //evt.Check(check);
        plater()->set_current_canvas_as_dirty();
    }, item->GetId());
}

void MenuFactory::append_menu_item_plate_name(wxMenu *menu)
{
    wxString name= _L("Edit Plate Name");
    // Delete old menu item
    const int item_id = menu->FindItem(name);
    if (item_id != wxNOT_FOUND) menu->Destroy(item_id);

    PartPlate *plate = plater()->get_partplate_list().get_selected_plate();
    assert(plate);

    auto item = append_menu_item(
        menu, wxID_ANY, name, "",
        [plate](wxCommandEvent &e) {
            int hover_idx =plater()->canvas3D()->GetHoverId();
            if (hover_idx == -1) {
                int plate_idx=plater()->GetPlateIndexByRightMenuInLeftUI();
                plater()->select_plate_by_hover_id(plate_idx * PartPlate::GRABBER_COUNT, false, true);
            }
            else
            {
                plater()->select_plate_by_hover_id(hover_idx, false, true);
            }
            plater()->get_current_canvas3D()->post_event(SimpleEvent(EVT_GLCANVAS_PLATE_NAME_CHANGE));
        },
        "", nullptr, []() { return true; }, m_parent);

    m_parent->Bind(
        wxEVT_UPDATE_UI,
        [](wxUpdateUIEvent &evt) {
            PartPlate *plate = plater()->get_partplate_list().get_selected_plate();
            assert(plate);
            plater()->set_current_canvas_as_dirty();
        },
        item->GetId());
}

void MenuFactory::update_object_menu()
{
    append_menu_items_add_volume(&m_object_menu);
}

void MenuFactory::update_default_menu()
{
    for (auto& name : { _L("Add Primitive") , _L("Add Handy models"), _L("Add Custom Models"), _L("Show Labels") }) {
        const auto menu_item_id = m_default_menu.FindItem(name);
        if (menu_item_id != wxNOT_FOUND)
            m_default_menu.Destroy(menu_item_id);
    }
    create_default_menu();
}

void MenuFactory::msw_rescale()
{
    for (MenuWithSeparators* menu : { &m_object_menu, &m_sla_object_menu, &m_part_menu, &m_default_menu })
        msw_rescale_menu(dynamic_cast<wxMenu*>(menu));
}

#ifdef _WIN32
// For this class is used code from stackoverflow:
// https://stackoverflow.com/questions/257288/is-it-possible-to-write-a-template-to-check-for-a-functions-existence
// Using this code we can to inspect of an existence of IsWheelInverted() function in class T
template <typename T>
class menu_has_update_def_colors
{
    typedef char one;
    struct two { char x[2]; };

    template <typename C> static one test(decltype(&C::UpdateDefColors));
    template <typename C> static two test(...);

public:
    static constexpr bool value = sizeof(test<T>(0)) == sizeof(char);
};

template<typename T>
static void update_menu_item_def_colors(T* item)
{
    if constexpr (menu_has_update_def_colors<wxMenuItem>::value) {
        item->UpdateDefColors();
    }
}
#endif

void MenuFactory::sys_color_changed()
{
    for (MenuWithSeparators* menu : { &m_object_menu, &m_sla_object_menu, &m_part_menu, &m_default_menu }) {
        msw_rescale_menu(dynamic_cast<wxMenu*>(menu));// msw_rescale_menu updates just icons, so use it
#ifdef _WIN32
        // but under MSW we have to update item's bachground color
        for (wxMenuItem* item : menu->GetMenuItems())
            update_menu_item_def_colors(item);
#endif
    }
}

void MenuFactory::sys_color_changed(wxMenuBar* menubar)
{
    // BBS: fix
#if 0
    for (size_t id = 0; id < menubar->GetMenuCount(); id++) {
        wxMenu* menu = menubar->GetMenu(id);
        msw_rescale_menu(menu);
#ifdef _WIN32
        // but under MSW we have to update item's bachground color
        for (wxMenuItem* item : menu->GetMenuItems())
            update_menu_item_def_colors(item);
#endif
    }
    menubar->Refresh();
#endif
}


} //namespace GUI
} //namespace Slic3r
