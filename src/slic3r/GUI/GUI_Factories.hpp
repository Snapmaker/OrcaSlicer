#ifndef slic3r_GUI_Factories_hpp_
#define slic3r_GUI_Factories_hpp_

#include <map>
#include <vector>
#include <array>

#include <wx/bitmap.h>

#include "libslic3r/PrintConfig.hpp"
#include "wxExtensions.hpp"

class wxMenu;
class wxMenuItem;

namespace Slic3r {

enum class ModelVolumeType : int;
class ModelVolume;
class ModelObject;

namespace GUI {

struct SimpleSettingData
{
    std::string name;
    std::string label;
    int priority;
};

struct SettingsFactory
{
//				     category ->       vector ( option )
    typedef std::map<std::string, std::vector<std::string>> Bundle;
    static std::map<std::string, std::string>               CATEGORY_ICON;

    //BBS: add setting data for table
    static std::map<std::string, std::vector<SimpleSettingData>>  OBJECT_CATEGORY_SETTINGS;
    static std::map<std::string, std::vector<SimpleSettingData>>  PART_CATEGORY_SETTINGS;

    static wxBitmap                             get_category_bitmap(const std::string& category_name, bool menu_bmp = true);
    static Bundle                               get_bundle(const DynamicPrintConfig* config, bool is_object_settings, bool is_layer_settings = false);
    static std::vector<std::string>             get_options(bool is_part);
    // Ultra (support groups): the curated support keys a MODEL_PART volume may carry. The list
    // itself lives in libslic3r (Slic3r::part_support_keys()) because PrintObject's group
    // resolver and PrintApply's invalidation predicate read the same one; these are the GUI-side
    // spellings used by the settings bundle, the part parameter panel and the group menu.
    // docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 3.5.
    static const std::vector<std::string>&      part_support_keys();
    static bool                                 is_part_support_key(const std::string& opt_key);
    // The group label a MODEL_PART volume carries, "" when it carries none.
    static std::string                          part_support_group(const ModelVolume* volume);
    //BBS: add api to get options for catogary
    static std::vector<SimpleSettingData> get_visible_options(const std::string& category, const bool is_part);
    static std::map<std::string, std::vector<SimpleSettingData>> get_all_visible_options(const bool is_part);
};

// Ultra (support groups): the reads and the one write everything that touches a group goes
// through - the Object-List submenu here and SupportGroupsDialog. A group's values live on its
// parts (the curated part-level support keys on ModelVolume::config); support_group is the label.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2.2, 2.5, 3.4.
std::vector<ModelVolume*> selected_part_volumes();
std::vector<std::string>  object_support_group_names(const ModelObject* object);
std::string               unique_support_group_name(const ModelObject* object, const std::string& base);
// The object's own effective support values: the edited process preset plus the object's overrides.
DynamicPrintConfig        object_support_values(const ModelObject* object);
// The overrides an existing group carries, read off its first member.
DynamicPrintConfig        support_group_values(const ModelObject* object, const std::string& group);
// A saved support set resolved against the loaded filaments and reduced to the part-level keys.
DynamicPrintConfig        support_set_values_by_name(const std::string& set_name, std::string* warning);
// Write a group assignment onto every volume, under one undo snapshot. values == nullptr clears.
void                      assign_support_group(const std::vector<ModelVolume*>& volumes,
                                               const std::string&               group_name,
                                               const DynamicPrintConfig*        values,
                                               const std::string&               snapshot);

class MenuFactory
{
public:
	static std::vector<wxBitmap>    get_volume_bitmaps();
	static std::vector<wxBitmap> get_text_volume_bitmaps();
	static std::vector<wxBitmap> get_svg_volume_bitmaps();

    MenuFactory();
    ~MenuFactory() = default;

    void    init(wxWindow* parent);
    void    update();
    void    update_object_menu();
    void    update_default_menu();
    void    msw_rescale();
    void    sys_color_changed();

    static void sys_color_changed(wxMenuBar* menu_bar);

    wxMenu* default_menu();
    wxMenu* object_menu();
    wxMenu* sla_object_menu();
    wxMenu* part_menu();
    wxMenu* text_part_menu();
    wxMenu* svg_part_menu();
    wxMenu* instance_menu();
    wxMenu* layer_menu();
    wxMenu* multi_selection_menu();
    //BBS: add part plate related logic
    wxMenu* plate_menu();
    wxMenu* assemble_object_menu();
    wxMenu* assemble_part_menu();
    wxMenu* assemble_multi_selection_menu();

    wxMenu* filament_action_menu(int active_filament_menu_id);

private:
    enum MenuType {
        mtObjectFFF = 0,
        mtObjectSLA,
        mtCount
    };

    wxWindow* m_parent {nullptr};

    MenuWithSeparators m_object_menu;
    MenuWithSeparators m_part_menu;
    MenuWithSeparators m_text_part_menu;
    MenuWithSeparators m_svg_part_menu;
    MenuWithSeparators m_sla_object_menu;
    MenuWithSeparators m_default_menu;
    MenuWithSeparators m_instance_menu;
    //BBS: add part plate related logic
    MenuWithSeparators m_plate_menu;
    MenuWithSeparators m_assemble_object_menu;
    MenuWithSeparators m_assemble_part_menu;

    wxMenu m_filament_action_menu;
   

    // Removed/Prepended Items according to the view mode
    std::array<wxMenuItem*, mtCount> items_increase;
    std::array<wxMenuItem*, mtCount> items_decrease;
    std::array<wxMenuItem*, mtCount> items_set_number_of_copies;

    void        create_default_menu();
    void        create_common_object_menu(wxMenu *menu);
    void        create_object_menu();
    void        create_sla_object_menu();
    void        create_part_menu();
    void        create_text_part_menu();
    void        create_svg_part_menu();
    //BBS: add part plate related logic
    void        create_plate_menu();
    //BBS: add bbl object menu
    void        create_extra_object_menu();
    void        create_bbl_part_menu();
    void        create_bbl_assemble_object_menu();
    void        create_bbl_assemble_part_menu();
    
    void        create_filament_action_menu(bool init, int active_filament_menu_id);

    wxMenu*     append_submenu_add_generic(wxMenu* menu, ModelVolumeType type);
    // Orca: add submenu for adding handy models
    wxMenu*     append_submenu_add_handy_model(wxMenu* menu, ModelVolumeType type);
    void        append_menu_item_add_text(wxMenu* menu, ModelVolumeType type, bool is_submenu_item = true);
    void        append_menu_item_add_svg(wxMenu *menu, ModelVolumeType type, bool is_submenu_item = true);    
    void        append_menu_item_add_code(wxMenu *menu, ModelVolumeType type);
    void        append_menu_item_add_shape(wxMenu *menu, ModelVolumeType type);
    void        append_menu_items_add_volume(wxMenu* menu);
    wxMenuItem* append_menu_item_layers_editing(wxMenu* menu);
    wxMenuItem* append_menu_item_settings(wxMenu* menu);
    wxMenuItem* append_menu_item_change_type(wxMenu* menu);
    void        append_menu_item_precise_seam_submenu(wxMenu* menu);
    wxMenuItem* append_menu_item_instance_to_object(wxMenu* menu);
    wxMenuItem* append_menu_item_printable(wxMenu* menu);
    void        append_menu_item_rename(wxMenu* menu);
    wxMenuItem* append_menu_item_fix_through_netfabb(wxMenu* menu);
    //wxMenuItem* append_menu_item_simplify(wxMenu* menu);
    void        append_menu_item_export_stl(wxMenu* menu, bool is_mulity_menu = false);
    // Ultra: "Export part as STL" for a single selected ModelVolume (part, modifier,
    // negative volume or support blocker/enforcer - they are all meshes).
    void        append_menu_item_export_stl_part(wxMenu* menu);
    void        append_menu_item_export_step(wxMenu* menu);
    void        append_menu_item_reload_from_disk(wxMenu* menu);
    void        append_menu_item_replace_with_stl(wxMenu* menu);
    void        append_menu_item_edit_in_blender(wxMenu* menu);
    void        append_menu_item_edit_in_freecad(wxMenu* menu);
    void        append_menu_item_cad_fillet(wxMenu* menu);
    void        append_menu_item_change_extruder(wxMenu* menu);
    void        append_menu_item_set_visible(wxMenu* menu);
    void        append_menu_item_delete(wxMenu* menu);
    void        append_menu_item_scale_selection_to_fit_print_volume(wxMenu* menu);
    void        append_menu_items_convert_unit(wxMenu* menu); // Add "Conver/Revert..." menu items (from/to inches/meters) after "Reload From Disk"
    void        append_menu_items_flush_options(wxMenu* menu);
    void        append_menu_item_merge_to_multipart_object(wxMenu *menu);
    void        append_menu_item_assemble_separately(wxMenu *menu);
    void        append_menu_items_visibility(wxMenu *menu);
    void        append_menu_items_support_group(wxMenu *menu);
    void        append_menu_item_merge_to_single_object(wxMenu* menu);
    void        append_menu_item_merge_parts_to_single_part(wxMenu *menu);
    void        append_menu_item_merge_some_parts_to_single_part(wxMenu *menu);
    void        append_menu_items_mirror(wxMenu *menu);
    // RE-EDITABLE CUTS: reopen the Cut gizmo on an existing cut.
    void        append_menu_item_edit_cut(wxMenu *menu);
    // "Copy cut to...": a submenu of the other objects, setting the selected
    // object's cut up on whichever one is chosen. See the .cpp.
    void        append_menu_item_copy_cut(wxMenu *menu);
    void        append_menu_item_invalidate_cut_info(wxMenu *menu);
    void        append_menu_item_edit_text(wxMenu *menu);
    void        append_menu_item_edit_svg(wxMenu *menu);

    //void        append_menu_items_instance_manipulation(wxMenu *menu);
    //void        update_menu_items_instance_manipulation(MenuType type);
    //BBS add bbl menu item
    void        append_menu_item_clone(wxMenu* menu);
    void        append_menu_item_simplify(wxMenu* menu);
    void        append_menu_item_image_fill(wxMenu* menu);
    void        append_menu_item_center(wxMenu* menu);
    void        append_menu_item_drop(wxMenu* menu);
    void        append_menu_item_per_object_process(wxMenu* menu);
    void        append_menu_item_per_object_settings(wxMenu* menu);
    void        append_menu_item_change_filament(wxMenu* menu);
    void        append_menu_item_set_printable(wxMenu* menu);
    void        append_menu_item_locked(wxMenu* menu);
    void        append_menu_item_fill_bed(wxMenu *menu);
    void        append_menu_item_plate_name(wxMenu *menu);
};

}}

#endif //slic3r_GUI_Factories_hpp_
