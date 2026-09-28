#ifndef slic3r_PresetBundle_hpp_
#define slic3r_PresetBundle_hpp_

#include "Preset.hpp"
#include "AppConfig.hpp"
#include "FilamentColorLibrary.hpp"
#include "enum_bitmask.hpp"
#include "MixedFilament.hpp"

#include <memory>
#include <functional>
#include <unordered_map>
#include <array>
#include <vector>
#include <set>
#include <boost/filesystem/path.hpp>

#define DEFAULT_USER_FOLDER_NAME "default"
#define BUNDLE_STRUCTURE_JSON_NAME "bundle_structure.json"

#define VALIDATE_PRESETS_SUCCESS                0
#define VALIDATE_PRESETS_PRINTER_NOT_FOUND      1
#define VALIDATE_PRESETS_FILAMENTS_NOT_FOUND    2
#define VALIDATE_PRESETS_MODIFIED_GCODES        3


// define an enum class of vendor type
enum class VendorType {
    Unknown = 0,
    Klipper,
    Marlin,
    Marlin_BBL
};

struct ConnectMachineInfo
{
    std::string filament_info {""};
    std::string filament_type {""};
    std::string nozzle_info {""};
    std::string nozzle_volume_type {""};
    std::string color_info{""};
    std::vector<std::string> multiColors;
    Slic3r::FilamentColorMode colorMode { Slic3r::FilamentColorMode::Segment };
    int index {0};
};

namespace Slic3r {

// Auto-enable newly added printer variants of a filament the user already ticked on (see the
// comment above PresetBundle::load_installed_filaments's use of this). Pulled out as a small pure
// function so the decision can be unit tested without touching the GUI, AppConfig, or the on-disk
// preset bundle.
struct FilamentVariantCandidate
{
    // Full preset name, e.g. "Panchroma CoPE @BBL H2D".
    std::string name;
    // Grouping key the filament-selection guide (WebGuideDialog / resources/web/guide/22,23)
    // uses to decide whether the whole group is shown as ticked: vendor id + the preset's alias
    // (or its name up to " @" when no explicit alias was set - see PresetBundle::load_system_presets_from_json).
    // Vendor id is included so same-named aliases from different vendors (e.g. two "Generic PLA")
    // never collide.
    std::string group_key;
    // Only system presets are considered; user presets are never auto-enabled and never used to
    // decide a group is "enabled".
    bool        is_system = false;
    // True if this preset is compatible with at least one currently installed/visible FFF printer preset.
    bool        compatible_with_installed_printer = false;
};

// Returns the full names, from `candidates`, that should be newly added to AppConfig's
// SECTION_FILAMENTS: for every system preset not already in `already_enabled_names`, enable it if
// another system preset sharing its group_key is already enabled AND the preset itself is
// compatible with at least one installed printer. A group the user fully unticked (no member
// enabled) stays untouched. Order of the result is unspecified.
std::vector<std::string> filaments_to_auto_enable(
    const std::vector<FilamentVariantCandidate> &candidates,
    const std::set<std::string>                 &already_enabled_names);

// Bundle of Print + Filament + Printer presets.
class PresetBundle
{
public:
    PresetBundle();
    PresetBundle(const PresetBundle &rhs);
    PresetBundle& operator=(const PresetBundle &rhs);

    // Remove all the presets but the "-- default --".
    // Optionally remove all the files referenced by the presets from the user profile directory.
    void            reset(bool delete_files);

    void            setup_directories();
    void            copy_files(const std::string& from);

    struct PresetPreferences {
        std::string printer_model_id;// name of a preferred printer model
        std::string printer_variant; // name of a preferred printer variant
        std::string filament;        // name of a preferred filament preset
        std::string sla_material;    // name of a preferred sla_material preset
    };

    // Progress hook for the long, blocking preset load. Preset loading is one uninterrupted
    // stretch of work (seconds, on a full profile set) with no event loop in sight, so the GUI
    // hands us a callback here and the loaders call it often enough that the caller can keep a
    // splash animation moving. Optional: null by default, and every call site checks before
    // invoking, so the non-GUI users of PresetBundle (tests, CLI, the profile validator) are
    // unaffected. Called once per preset file / per vendor JSON, on the loading thread; the
    // callback must be cheap and must not touch the bundle.
    using ProgressCallback = std::function<void()>;
    void set_progress_callback(ProgressCallback cb);
    void notify_progress() const { if (m_progress_callback) m_progress_callback(); }

    // Load ini files of all types (print, filament, printer) from Slic3r::data_dir() / presets.
    // Load selections (current print, current filaments, current printer) from config.ini
    // select preferred presets, if any exist
    PresetsConfigSubstitutions load_presets(AppConfig &config, ForwardCompatibilitySubstitutionRule rule,
                                            const PresetPreferences& preferred_selection = PresetPreferences());

    // Load selections (current print, current filaments, current printer) from config.ini
    // This is done just once on application start up.
    //BBS: change it to public
    void     load_selections(AppConfig &config, const PresetPreferences& preferred_selection = PresetPreferences());

    // BBS Load user presets
    PresetsConfigSubstitutions load_user_presets(std::string user, ForwardCompatibilitySubstitutionRule rule);
    PresetsConfigSubstitutions load_user_presets(AppConfig &config, std::map<std::string, std::map<std::string, std::string>>& my_presets, ForwardCompatibilitySubstitutionRule rule);
    PresetsConfigSubstitutions import_presets(std::vector<std::string> &files, std::function<int(std::string const &)> override_confirm, ForwardCompatibilitySubstitutionRule rule);
    bool                       import_json_presets(PresetsConfigSubstitutions &            substitutions,
                                                   std::string &                           file,
                                                   std::function<int(std::string const &)> override_confirm,
                                                   ForwardCompatibilitySubstitutionRule    rule,
                                                   int &                                   overwrite,
                                                   std::vector<std::string> &              result);
    void save_user_presets(AppConfig& config, std::vector<std::string>& need_to_delete_list);
    void remove_users_preset(AppConfig &config, std::map<std::string, std::map<std::string, std::string>> * my_presets = nullptr);
    void update_user_presets_directory(const std::string preset_folder);
    void remove_user_presets_directory(const std::string preset_folder);
    void update_system_preset_setting_ids(std::map<std::string, std::map<std::string, std::string>>& system_presets);

    //BBS: add API to get previous machine
    int validate_presets(const std::string &file_name, DynamicPrintConfig& config, std::set<std::string>& different_gcodes);

    //BBS: add function to generate differed preset for save
    //the pointer should be freed by the caller
    Preset* get_preset_differed_for_save(Preset& preset);
    int get_differed_values_to_update(Preset& preset, std::map<std::string, std::string>& key_values);

    //BBS: get vendor's current version
    Semver get_vendor_profile_version(std::string vendor_name);

    // Orca: get vendor type
    VendorType get_current_vendor_type();
    // Vendor related handy functions
    bool is_bbl_vendor() { return get_current_vendor_type() == VendorType::Marlin_BBL; }
    // Whether using bbl network for print upload
    bool use_bbl_network();
    // Whether using bbl's device tab
    bool use_bbl_device_tab();

    // Ultra: last print profile the user worked with, keyed by layer_height_key();
    // consulted by update_compatible() when the active print preset turns incompatible.
    std::map<std::string, std::string> preferred_print_profiles_by_height;
    static std::string layer_height_key(double layer_height);

    bool backup_user_folder() const;

    //BBS: project embedded preset logic
    PresetsConfigSubstitutions load_project_embedded_presets(std::vector<Preset*> project_presets, ForwardCompatibilitySubstitutionRule substitution_rule);
    std::vector<Preset*> get_current_project_embedded_presets();
    void reset_project_embedded_presets();

    //BBS: find printer model
    std::string get_texture_for_printer_model(std::string model_name);
    std::string get_stl_model_for_printer_model(std::string model_name);
    std::string get_hotend_model_for_printer_model(std::string model_name);

    // Export selections (current print, current filaments, current printer) into config.ini
    void            export_selections(AppConfig &config);

    // BBS
    void            set_num_filaments(unsigned int n, std::string new_col = "");
    void            set_num_filaments(unsigned int n, std::vector<std::string> new_colors);
    void            update_num_filaments(unsigned int to_del_filament_id);
    // Physical slot count. filament_colour is the source of truth; mixed_filament_definitions
    // and a topped-up filament_presets list must not invent extra slots (Orca #15728 adapt).
    size_t          num_physical_filaments() const;
    unsigned int sync_ams_list(unsigned int & unknowns);
    //BBS: check whether this is the only edited filament
    bool is_the_only_edited_filament(unsigned int filament_index);

    std::vector<FilamentVolumeType> get_filament_volume_types() const;
    void                            set_filament_volume_types(const std::vector<FilamentVolumeType> &types);

    // Orca: update selected filament and print
    void           update_selections(AppConfig &config);
    void set_calibrate_printer(std::string name);

    void set_is_validation_mode(bool mode) { validation_mode = mode; }
    void set_vendor_to_validate(std::string vendor) { vendor_to_validate = vendor; }

    std::set<std::string> get_printer_names_by_printer_type_and_nozzle(const std::string &printer_type, std::string nozzle_diameter_str);
    bool                  check_filament_temp_equation_by_printer_type_and_nozzle_for_mas_tray(const std::string &printer_type,
                                                                                               std::string &      nozzle_diameter_str,
                                                                                               std::string &      setting_id,
                                                                                               std::string &      tag_uid,
                                                                                               std::string &      nozzle_temp_min,
                                                                                               std::string &      nozzle_temp_max,
                                                                                               std::string &      preset_setting_id);

    Preset *                    get_similar_printer_preset(std::string printer_model, std::string printer_variant);
    
    PresetCollection            prints;
    PresetCollection            sla_prints;
    PresetCollection            filaments;
    PresetCollection            sla_materials;
	PresetCollection& 			materials(PrinterTechnology pt)       { return pt == ptFFF ? this->filaments : this->sla_materials; }
	const PresetCollection& 	materials(PrinterTechnology pt) const { return pt == ptFFF ? this->filaments : this->sla_materials; }
    PrinterPresetCollection     printers;
    PhysicalPrinterCollection   physical_printers;
    // Filament preset names for a multi-extruder or multi-material print.
    // extruders.size() should be the same as printers.get_edited_preset().config.nozzle_diameter.size()
    std::vector<std::string>    filament_presets;
    // BBS: ams
    std::map<int, DynamicPrintConfig> filament_ams_list;
    std::vector<std::vector<std::string>> ams_multi_color_filment;

    // Mixed (virtual) filaments for layer-based colour mixing.
    MixedFilamentManager        mixed_filaments;

    // Snapmaker
    std::map<int, std::pair<std::string, std::string>> machine_filaments;
    std::vector<ConnectMachineInfo>                    m_connect_machine_info_list;

    // Calibrate
    Preset const * calibrate_printer = nullptr;
    std::set<Preset const *> calibrate_filaments;

    // The project configuration values are kept separated from the print/filament/printer preset,
    // they are being serialized / deserialized from / to the .amf, .3mf, .config, .gcode,
    // and they are being used by slicing core.
    DynamicPrintConfig          project_config;

    // There will be an entry for each system profile loaded,
    // and the system profiles will point to the VendorProfile instances owned by PresetBundle::vendors.
    VendorMap                   vendors;

    // Orca: for OrcaFilamentLibrary
    std::map<std::string, DynamicPrintConfig> m_config_maps;
    std::map<std::string, std::string> m_filament_id_maps;

        struct ObsoletePresets
    {
        std::vector<std::string> prints;
        std::vector<std::string> sla_prints;
        std::vector<std::string> filaments;
        std::vector<std::string> sla_materials;
        std::vector<std::string> printers;
    };
    ObsoletePresets             obsolete_presets;

    bool                        has_defauls_only() const
        { return prints.has_defaults_only() && filaments.has_defaults_only() && printers.has_defaults_only(); }

    DynamicPrintConfig          full_config() const;
    // full_config() with the some "useless" config removed.
    DynamicPrintConfig          full_config_secure() const;

    // Load user configuration and store it into the user profiles.
    // This method is called by the configuration wizard.
    void                        load_config_from_wizard(const std::string &name, DynamicPrintConfig config, Semver file_version, bool is_custom_defined = false)
        { this->load_config_file_config(name, false, std::move(config), file_version, true, is_custom_defined); }

    // Load configuration that comes from a model file containing configuration, such as 3MF et al.
    // This method is called by the Plater.
    void                        load_config_model(const std::string &name, DynamicPrintConfig config, Semver file_version = Semver())
        { this->load_config_file_config(name, true, std::move(config), file_version); }

    // Load an external config file containing the print, filament and printer presets.
    // Instead of a config file, a G-code may be loaded containing the full set of parameters.
    // In the future the configuration will likely be read from an AMF file as well.
    // If the file is loaded successfully, its print / filament / printer profiles will be activated.
    ConfigSubstitutions         load_config_file(const std::string &path, ForwardCompatibilitySubstitutionRule compatibility_rule);

    // Ultra: called with every config read from a file the user imports - preset JSON files and
    // bundles (import_presets) and the config block of a G-code file (load_config_file) - before it
    // becomes a preset. source names the file. The GUI uses it to strip post-processing scripts and
    // similar settings the user did not write themselves (see UntrustedInput.hpp); project 3MFs go
    // through the same check in Plater. Empty = no filter (CLI, tests).
    std::function<void(const std::string &source, DynamicPrintConfig &config)> untrusted_config_filter;

    // Load a config bundle file, into presets and store the loaded presets into separate files
    // of the local configuration directory.
    // Load settings into the provided settings instance.
    // Activate the presets stored in the config bundle.
    // Returns the number of presets loaded successfully.
    enum LoadConfigBundleAttribute {
        // Save the profiles, which have been loaded.
        SaveImported,
        // Delete all old config profiles before loading.
        ResetUserProfile,
        // Load a system config bundle.
        LoadSystem,
        LoadVendorOnly,
        LoadFilamentOnly,
    };
    using LoadConfigBundleAttributes = enum_bitmask<LoadConfigBundleAttribute>;
    // Load the config bundle based on the flags.
    // Don't do any config substitutions when loading a system profile, perform and report substitutions otherwise.
    /*std::pair<PresetsConfigSubstitutions, size_t> load_configbundle(
        const std::string &path, LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule);*/
    //Orca: load config bundle from json, pass the base bundle to support cross vendor inheritance
    std::pair<PresetsConfigSubstitutions, size_t> load_vendor_configs_from_json(
        const std::string &path, const std::string &vendor_name, LoadConfigBundleAttributes flags, ForwardCompatibilitySubstitutionRule compatibility_rule, const PresetBundle* base_bundle = nullptr);

    // Export a config bundle file containing all the presets and the names of the active presets.
    //void                        export_configbundle(const std::string &path, bool export_system_settings = false, bool export_physical_printers = false);
    //BBS: add a function to export current configbundle as default
    //void export_current_configbundle(const std::string &path);
    //BBS: add a function to export system presets for cloud-slicer
    //void export_system_configs(const std::string &path);
    std::vector<std::string> export_current_configs(const std::string &path, std::function<int(std::string const &)> override_confirm,
        bool include_modify, bool export_system_settings = false);

    // Enable / disable the "- default -" preset.
    void                        set_default_suppressed(bool default_suppressed);

    // Set the filament preset name. As the name could come from the UI selection box,
    // an optional "(modified)" suffix will be removed from the filament name.
    void                        set_filament_preset(size_t idx, const std::string &name);

    // Read out the number of extruders from an active printer preset,
    // update size and content of filament_presets.
    void                        update_multi_material_filament_presets(size_t to_delete_filament_id = size_t(-1),
                                                                       size_t old_num_filaments = size_t(-1));
    // Rebuild old->new virtual filament mapping after mixed-row enable/delete
    // changes when the physical filament count itself did not change.
    void                        update_mixed_filament_id_remap(const std::vector<MixedFilament> &old_mixed,
                                                               size_t old_num_filaments,
                                                               size_t new_num_filaments,
                                                               size_t deleted_mixed_idx = size_t(-1),
                                                               const std::vector<unsigned int> &kept_physical_ids = {});
    // Mapping generated during the latest filament count change.
    // Index is old 1-based filament ID, value is new 1-based filament ID (0 = removed).
    const std::vector<unsigned int>& last_filament_id_remap() const { return m_last_filament_id_remap; }
    
    // Build custom remap for mixed filament merge operations
    // This is used when merging a mixed filament into another filament (physical or mixed)
    void build_merge_filament_remap(size_t from_id, size_t to_id, size_t total_filaments)
    {
        m_last_filament_id_remap.assign(total_filaments + 1, 0);
        for (size_t i = 0; i <= total_filaments; ++i) {
            if (i == from_id + 1) {
                // When from_id < to_id, the target also shifts down by 1 after
                // source removal, so its new 1-based ID is `to_id` (not to_id+1).
                if (from_id < to_id)
                    m_last_filament_id_remap[i] = (unsigned int)(to_id);
                else
                    m_last_filament_id_remap[i] = (unsigned int)(to_id + 1);
            } else if (i > from_id + 1) {
                m_last_filament_id_remap[i] = (unsigned int)(i - 1);  // Shift down IDs after deleted one
            } else {
                m_last_filament_id_remap[i] = (unsigned int)i;  // Keep unchanged
            }
        }
    }
    
    // Build custom remap for physical to mixed filament merge operations
    // This accounts for virtual ID changes when a physical filament is deleted
    // AND accounts for mixed filaments that depend on the physical filament being deleted
    // num_physical: number of physical filaments before deletion
    void build_merge_filament_remap(size_t from_id, size_t to_id, size_t total_filaments, size_t num_physical)
    {
        m_last_filament_id_remap.assign(total_filaments + 1, 0);
        
        // First, identify which mixed filaments will be deleted (those that depend on from_id)
        std::set<size_t> deleted_mixed_indices;
        unsigned int from_1based = (unsigned int)(from_id + 1);

        std::vector<size_t> dependent = mixed_filaments.mixed_filaments_using_physical(from_1based);
        size_t visible = 0;
        const auto& mfs_ref = mixed_filaments.mixed_filaments();
        for (size_t k = 0; k < mfs_ref.size(); ++k) {
            if (!mfs_ref[k].enabled || mfs_ref[k].deleted) continue;
            if (std::find(dependent.begin(), dependent.end(), k) != dependent.end())
                deleted_mixed_indices.insert(num_physical + visible);
            ++visible;
        }
        
        for (size_t i = 0; i <= total_filaments; ++i) {
            if (i == from_id + 1) {
                // Source physical filament maps to target mixed filament
                // The target's new virtual ID after deletion: new_num_physical + adjusted_mixed_idx
                size_t target_old_virtual_id = to_id;  // 0-based
                size_t target_old_mixed_idx = target_old_virtual_id - num_physical;
                size_t new_num_physical = num_physical - 1;
                // Subtract deleted mixed filaments that appear before the target
                size_t deleted_before_target = 0;
                for (size_t vid : deleted_mixed_indices)
                    if (vid < target_old_virtual_id) ++deleted_before_target;
                size_t target_new_virtual_id = new_num_physical + target_old_mixed_idx - deleted_before_target;
                m_last_filament_id_remap[i] = (unsigned int)(target_new_virtual_id + 1);  // Convert to 1-based
            } else if (i > from_id + 1 && i <= num_physical) {
                // Subsequent physical filaments shift down by 1
                m_last_filament_id_remap[i] = (unsigned int)(i - 1);
            } else if (i > num_physical) {
                // Mixed filament indices will change (because num_physical decreases)
                size_t old_virtual_id = i - 1;
                size_t old_mixed_idx = old_virtual_id - num_physical;
                
                // Check if this mixed filament will be deleted
                if (deleted_mixed_indices.find(old_virtual_id) != deleted_mixed_indices.end()) {
                    // This mixed filament will be deleted, map to 0 (invalid)
                    m_last_filament_id_remap[i] = 0;
                } else {
                    // This mixed filament will be kept, calculate its new virtual ID.
                    // Subtract deleted mixed filaments that appear before this one.
                    size_t new_num_physical = num_physical - 1;
                    size_t deleted_before = 0;
                    for (size_t vid : deleted_mixed_indices)
                        if (vid < old_virtual_id) ++deleted_before;
                    size_t new_virtual_id = new_num_physical + old_mixed_idx - deleted_before;
                    m_last_filament_id_remap[i] = (unsigned int)(new_virtual_id + 1);  // Convert to 1-based
                }
            } else {
                // Physical filaments before source remain unchanged
                m_last_filament_id_remap[i] = (unsigned int)i;
            }
        }
    }
    
    // Set custom remap table directly
    void set_filament_id_remap(const std::vector<unsigned int>& remap)
    {
        m_last_filament_id_remap = remap;
    }
    
    std::vector<unsigned int> consume_last_filament_id_remap()
    {
        std::vector<unsigned int> out = std::move(m_last_filament_id_remap);
        m_last_filament_id_remap.clear();
        return out;
    }

    // Update the is_compatible flag of all print and filament presets depending on whether they are marked
    // as compatible with the currently selected printer (and print in case of filament presets).
    // Also updates the is_visible flag of each preset.
    // If select_other_if_incompatible is true, then the print or filament preset is switched to some compatible
    // preset if the current print or filament preset is not compatible.
    void                        update_compatible(PresetSelectCompatibleType select_other_print_if_incompatible, PresetSelectCompatibleType select_other_filament_if_incompatible);
    void                        update_compatible(PresetSelectCompatibleType select_other_if_incompatible) { this->update_compatible(select_other_if_incompatible, select_other_if_incompatible); }

    // Set the is_visible flag for printer vendors, printer models and printer variants
    // based on the user configuration.
    // If the "vendor" section is missing, enable all models and variants of the particular vendor.
    // Also merges newly shipped nozzle variants into app config when the model was already enabled (no wizard needed).
    void                        load_installed_printers(AppConfig &config);

    // If the user already enabled a printer model (any nozzle variant in app config), enable every variant
    // currently defined in the vendor profile. Called from load_installed_printers; exposed for rare direct use.
    void                        install_missing_variants_for_enabled_models(AppConfig &config);

    const std::string&          get_preset_name_by_alias(const Preset::Type& preset_type, const std::string& alias) const;

    const int                   get_required_hrc_by_filament_type(const std::string& filament_type) const;
    // Save current preset of a provided type under a new name. If the name is different from the old one,
    // Unselected option would be reverted to the beginning values
    //BBS: add project embedded preset logic
    void                        save_changes_for_preset(const std::string& new_name, Preset::Type type, const std::vector<std::string>& unselected_options, bool save_to_project = false);

    std::pair<PresetsConfigSubstitutions, std::string> load_system_models_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    std::pair<PresetsConfigSubstitutions, std::string> load_system_filaments_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    VendorProfile                                      get_custom_vendor_models() const;

    // SM_FEATURE: add Snapmaker machine as default
    static const char *SM_BUNDLE;
    static const char* SM_DEFAULT_PRINTER_MODEL;
    static const char* SM_DEFAULT_PRINTER_VARIANT;
    static const char* SM_DEFAULT_FILAMENT;
    static const char *ORCA_FILAMENT_LIBRARY;


    static std::array<Preset::Type, 3>  types_list(PrinterTechnology pt) {
        if (pt == ptFFF)
            return  { Preset::TYPE_PRINTER, Preset::TYPE_PRINT, Preset::TYPE_FILAMENT };
        return      { Preset::TYPE_PRINTER, Preset::TYPE_SLA_PRINT, Preset::TYPE_SLA_MATERIAL };
    }

    // Orca: for validation only
    bool has_errors() const;

private:
    //std::pair<PresetsConfigSubstitutions, std::string> load_system_presets(ForwardCompatibilitySubstitutionRule compatibility_rule);
    //BBS: add json related logic
    std::pair<PresetsConfigSubstitutions, std::string> load_system_presets_from_json(ForwardCompatibilitySubstitutionRule compatibility_rule);
    // Merge one vendor's presets with the other vendor's presets, report duplicates.
    std::vector<std::string>    merge_presets(PresetBundle &&other);
    void                        build_filament_id_remap(const std::vector<MixedFilament> &old_mixed,
                                                        size_t old_num_filaments,
                                                        size_t new_num_filaments,
                                                        bool deleting_filament,
                                                        unsigned int deleted_1based,
                                                        size_t deleted_mixed_idx = size_t(-1),
                                                        const std::vector<unsigned int> &kept_physical_ids = {});
    // Update renamed_from and alias maps of system profiles.
    void 						update_system_maps();

    // Set the is_visible flag for filaments and sla materials,
    // apply defaults based on enabled printers when no filaments/materials are installed.
    void                        load_installed_filaments(AppConfig &config);
    void                        load_installed_sla_materials(AppConfig &config);

    // Load print, filament & printer presets from a config. If it is an external config, then the name is extracted from the external path.
    // and the external config is just referenced, not stored into user profile directory.
    // If it is not an external config, then the config will be stored into the user profile directory.
    void                        load_config_file_config(const std::string &name_or_path, bool is_external, DynamicPrintConfig &&config, Semver file_version = Semver(), bool selected = false, bool is_custom_defined = false);
    /*ConfigSubstitutions         load_config_file_config_bundle(
        const std::string &path, const boost::property_tree::ptree &tree, ForwardCompatibilitySubstitutionRule compatibility_rule);*/

    DynamicPrintConfig          full_fff_config() const;
    DynamicPrintConfig          full_sla_config() const;

    // Orca: used for validation only
    bool validation_mode = false;
    std::string vendor_to_validate = ""; 
    int m_errors = 0;
    std::vector<unsigned int> m_last_filament_id_remap;

    // Optional progress hook; see set_progress_callback(). Null unless a GUI set one.
    ProgressCallback m_progress_callback;

};

ENABLE_ENUM_BITMASK_OPERATORS(PresetBundle::LoadConfigBundleAttribute)

} // namespace Slic3r

#endif /* slic3r_PresetBundle_hpp_ */
