#ifndef slic3r_Tab_hpp_
#define slic3r_Tab_hpp_

//	 The "Expert" tab at the right of the main tabbed window.
//
//	 This file implements following packages:
//	   Slic3r::GUI::Tab;
//	       Slic3r::GUI::Tab::Print;
//	       Slic3r::GUI::Tab::Filament;
//	       Slic3r::GUI::Tab::Printer;
//	   Slic3r::GUI::Tab::Page
//	       - Option page: For example, the Slic3r::GUI::Tab::Print has option pages "Layers and perimeters", "Infill", "Skirt and brim" ...
//	   Slic3r::GUI::SavePresetWindow
//	       - Dialog to select a new preset name to store the configuration.
//	   Slic3r::GUI::Tab::Preset;
//	       - Single preset item: name, file is default or external.

#include <wx/panel.h>
#include <wx/notebook.h>
#include <wx/listbook.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/bmpcbox.h>
#include <wx/bmpbuttn.h>
#include <wx/treectrl.h>
#include <wx/imaglist.h>

#include <map>
#include <set>
#include <vector>
#include <memory>

//#include "BedShapeDialog.hpp"
#include "wxExtensions.hpp"
#include "ConfigManipulation.hpp"
#include "OptionsGroup.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PerHeadProcess.hpp"
//BBS: GUI refactor
#include "Notebook.hpp"
#include "ParamsPanel.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/CheckBox.hpp" // ORCA

class TabCtrl;
class ModeSwitchButton;
class SwitchButton;
class MultiSwitchButton;

class ComboBox;

namespace Slic3r {

class ModelConfig;
class ObjectBase;

namespace GUI {

class TabPresetComboBox;
class OG_CustomCtrl;
class HyperLink;

std::vector<InputShaperType> input_shaper_types_for_flavor(GCodeFlavor flavor);

// Single Tab page containing a{ vsizer } of{ optgroups }
// package Slic3r::GUI::Tab::Page;
using ConfigOptionsGroupShp = std::shared_ptr<ConfigOptionsGroup>;
class Page: public std::enable_shared_from_this<Page>// : public wxScrolledWindow
{
	//BBS: GUI refactor
	wxPanel*		m_tab_owner;
	wxWindow*		m_parent;
	wxString		m_title;
	size_t			m_iconID;
	wxBoxSizer*		m_vsizer;
	// BBS: new layout
	wxStaticText*	m_page_title;
    bool            m_show = true;
public:
	//BBS: GUI refactor
    Page(wxWindow* parent, const wxString& title, int iconID, wxPanel* tab_owner);
	~Page() {}

	bool				m_is_modified_values{ false };
	bool				m_is_nonsys_values{ true };
	// BBS
    bool            m_split_multi_line      = false;
    bool            m_option_label_at_right = false;

public:
	std::vector <ConfigOptionsGroupShp> m_optgroups;
	DynamicPrintConfig* m_config;

	wxBoxSizer*	vsizer() const { return m_vsizer; }
	wxWindow*	parent() const { return m_parent; }
	const wxString&	title()	 const { return m_title; }
	size_t		iconID() const { return m_iconID; }
	void		set_config(DynamicPrintConfig* config_in) { m_config = config_in; }
	void		reload_config();
    void        update_visibility(ConfigOptionMode mode, bool update_contolls_visibility);
    void        activate(ConfigOptionMode mode, std::function<void()> throw_if_canceled);
    // Whether an option group has no controls yet.
    bool        build_pending() const;
    // Builds the next option group that has no controls yet; true while some remain.
    bool        build_step(ConfigOptionMode mode);
    void        clear();
    void        msw_rescale();
    void        sys_color_changed();
    void        refresh();
	Field*		get_field(const t_config_option_key& opt_key, int opt_index = -1) const;
    Line *      get_line(const t_config_option_key &opt_key, int opt_index = -1);
	bool		set_value(const t_config_option_key& opt_key, const boost::any& value);
	// BBS. Add is_extruder_og parameter.
	ConfigOptionsGroupShp	new_optgroup(const wxString& title, const wxString& icon = wxEmptyString, int noncommon_label_width = -1, bool is_extruder_og = false);
	const ConfigOptionsGroupShp	get_optgroup(const wxString& title) const;

	bool		set_item_colour(const wxColour *clr) {
		if (m_item_color != clr) {
			m_item_color = clr;
			return true;
		}
		return false;
	}

	const wxColour	get_item_colour() {
			return *m_item_color;
	}
    bool get_show() const { return m_show; }

    std::map<std::string, std::string> m_opt_id_map;

protected:
    size_t      next_group_to_build() const;
    bool        activate_group(size_t i, ConfigOptionMode mode, std::function<void()> throw_if_canceled);
	// Color of TreeCtrlItem. The wxColour will be updated only if the new wxColour pointer differs from the currently rendered one.
	const wxColour*		m_item_color;
};


using PageShp = std::shared_ptr<Page>;
class Tab: public wxPanel
{
	//BBS: GUI refactor
protected:
	ParamsPanel*		m_parent;
/*#ifdef __WXOSX__
	wxPanel*			m_tmp_panel;
	int					m_size_move = -1;
#endif // __WXOSX__*/

    Preset::Type        m_type;
	std::string			m_name;
	const wxString		m_title;
	TabPresetComboBox*	m_presets_choice { nullptr };

	//BBS: GUI refactor
	wxPanel*			m_top_panel;
	wxBoxSizer* m_main_sizer;
	wxBoxSizer* m_top_sizer;
	wxBoxSizer* m_top_left_sizer;
	wxGridSizer* m_top_right_sizer;
	wxBoxSizer* m_select_sizer;
	wxBoxSizer* m_tree_sizer;

	ScalableButton*		m_btn_compare_preset;
	ScalableButton*		m_btn_save_preset;
	ScalableButton*		m_btn_delete_preset;
	//ScalableButton*		m_btn_edit_ph_printer {nullptr};
	//ScalableButton*		m_btn_hide_incompatible_presets;
	//wxBoxSizer*			m_hsizer;
	//wxBoxSizer*			m_left_sizer;
	TabCtrl*			m_tabctrl;
	wxImageList*		m_icons;

	wxScrolledWindow*	m_page_view {nullptr};
	//wxBoxSizer*			m_page_sizer {nullptr};

   	struct PresetDependencies {
		Preset::Type type	  = Preset::TYPE_INVALID;
		::CheckBox*   checkbox = nullptr;
		wxStaticText* checkbox_title = nullptr;
		Button 	*btn  = nullptr;
		std::string  key_list; // "compatible_printers"
		std::string  key_condition;
		wxString     dialog_title;
		wxString     dialog_label;
	};
	PresetDependencies 	m_compatible_printers;
	PresetDependencies 	m_compatible_prints;

    /* Indicates, that default preset or preset inherited from default is selected
     * This value is used for a options color updating
     * (use green color only for options, which values are equal to system values)
     */
    bool                    m_is_default_preset {false};

	// just be used for edit filament dialog
    bool m_just_edit{false};

	ScalableButton*			m_undo_btn;
	ScalableButton*			m_undo_to_sys_btn;
	//ScalableButton*			m_question_btn;
	ScalableButton*			m_btn_search;
    StaticBox *				m_search_item;
    TextInput *				m_search_input;

	// Cached bitmaps.
	// A "flag" icon to be displayned next to the preset name in the Tab's combo box.
	ScalableBitmap			m_bmp_show_incompatible_presets;
	ScalableBitmap			m_bmp_hide_incompatible_presets;
	// Bitmaps to be shown on the "Revert to system" aka "Lock to system" button next to each input field.
	ScalableBitmap 			m_bmp_value_lock;
	ScalableBitmap 			m_bmp_value_unlock;
	ScalableBitmap 			m_bmp_white_bullet;
	// The following bitmap points to either m_bmp_value_unlock or m_bmp_white_bullet, depending on whether the current preset has a parent preset.
	ScalableBitmap 		   *m_bmp_non_system;
	// Bitmaps to be shown on the "Undo user changes" button next to each input field.
	ScalableBitmap 			m_bmp_value_revert;
    // Bitmaps to be shown on the "Undo user changes" button next to each input field.
    ScalableBitmap 			m_bmp_edit_value;

    std::vector<ScalableButton*>	m_scaled_buttons = {};
    std::vector<ScalableBitmap*>	m_scaled_bitmaps = {};
    std::vector<ScalableBitmap>     m_scaled_icons_list = {};

	// Colors for ui "decoration"
	wxColour			m_sys_label_clr;
	wxColour			m_modified_label_clr;
	wxColour			m_default_text_clr;

	// Tooltip text for reset buttons (for whole options group)
	wxString			m_ttg_value_lock;
	wxString			m_ttg_value_unlock;
	wxString			m_ttg_white_bullet_ns;
	// The following text points to either m_ttg_value_unlock or m_ttg_white_bullet_ns, depending on whether the current preset has a parent preset.
	wxString			*m_ttg_non_system;
	// Tooltip text to be shown on the "Undo user changes" button next to each input field.
	wxString			m_ttg_white_bullet;
	wxString			m_ttg_value_revert;

	// Tooltip text for reset buttons (for each option in group)
	wxString			m_tt_value_lock;
	wxString			m_tt_value_unlock;
	// The following text points to either m_tt_value_unlock or m_ttg_white_bullet_ns, depending on whether the current preset has a parent preset.
	wxString			*m_tt_non_system;
	// Tooltip text to be shown on the "Undo user changes" button next to each input field.
	wxString			m_tt_white_bullet;
	wxString			m_tt_value_revert;

	int					m_icon_count;
	std::map<std::string, size_t>	m_icon_index;		// Map from an icon file name to its index
	std::map<wxString, std::string>	m_category_icon;	// Map from a category name to an icon file name
	std::vector<PageShp>			m_pages;
	Page*				m_active_page {nullptr};
	bool				m_disable_tree_sel_changed_event {false};
	bool				m_show_incompatible_presets;
	int					m_last_select_item = -1;

    std::vector<Preset::Type>	m_dependent_tabs;
	enum OptStatus { osSystemValue = 1, osInitValue = 2 };
	std::map<std::string, int>	m_options_list;
    std::map<std::string, int> m_all_extruder_options_status;
	int							m_opt_status_value = 0;

	bool				m_is_modified_values{ false };
	bool				m_is_nonsys_values{ true };
	bool				m_postpone_update_ui {false};

    void                set_type();

    int                 m_em_unit;
    // To avoid actions with no-completed Tab
    bool                m_completed { false };
    ConfigOptionMode    m_mode = comAdvanced; // to correct first Tab update_visibility() set mode to Advanced

	struct Highlighter
	{
		void set_timer_owner(wxEvtHandler* owner, int timerid = wxID_ANY);
		void init(std::pair<OG_CustomCtrl*, bool*>);
		void blink();
		void invalidate();

	private:
		OG_CustomCtrl*	m_custom_ctrl	{nullptr};
		bool*			m_show_blink_ptr{nullptr};
		int				m_blink_counter	{0};
	    wxTimer         m_timer;
	}
    m_highlighter;

	DynamicPrintConfig 	m_cache_config;
    std::vector<std::string> m_cache_options;
    // Snapmaker Orca: the process layout the cached rows were taken from (its ids, variants,
    // marker and variant keys), so that rows of a preset with values per tool head are transferred
    // by (id, variant) and not by index (PerHeadProcess::transfer_columns).
    DynamicPrintConfig  m_cache_process_source;


	bool				m_page_switch_running = false;
	bool				m_page_switch_planned = false;

    bool				m_is_timelapse_wipe_tower_already_prompted = false;

public:
	PresetBundle*		m_preset_bundle;
	bool				m_show_btn_incompatible_presets = false;
	PresetCollection*	m_presets = nullptr;
	DynamicPrintConfig*	m_config;
	ogStaticText*		m_parent_preset_description_line = nullptr;
	ScalableButton*		m_detach_preset_btn	= nullptr;

	// map of option name -> wxColour (color of the colored label, associated with option)
    // Used for options which don't have corresponded field
	std::map<std::string, wxColour>	m_colored_Label_colors;

    // Counter for the updating (because of an update() function can have a recursive behavior):
    // 1. increase value from the very beginning of an update() function
    // 2. decrease value at the end of an update() function
    // 3. propagate changed configuration to the Plater when (m_update_cnt == 0) only
    int                 m_update_cnt = 0;

	ModeSwitchButton *m_mode_view = nullptr;
	ScalableButton* m_mode_icon = nullptr; // ORCA m_static_title replacement
    wxSizer *       m_variant_sizer   = nullptr;
    MultiSwitchButton *  m_extruder_switch = nullptr;
    MultiSwitchButton *  m_variant_combo   = nullptr;
    ScalableButton *m_extruder_sync   = nullptr;
	wxPanel *       m_extruder_sync_box  = nullptr;
    std::vector<NozzleVolumeType> m_actual_nozzle_volumes;
    // Snapmaker Orca: flow selector mode of m_extruder_switch. The process preset holds one column
    // per flow type (HighFlowNotices::flow_selector_types) and the switch offers these types
    // instead of extruders; empty in every other case.
    std::vector<NozzleVolumeType> m_flow_selector_types;
    // Snapmaker Orca: Process-tab speed selector (PerHeadProcess.hpp), "All extruders" plus one entry per tool head
    // (not on Bambu two-head printers). All edits the shared columns of m_all_flow (set by m_flow_toggle);
    // a head edits its own columns, created on first edit.
    bool                          m_head_selector { false };
    std::vector<NozzleVolumeType> m_head_flow_types;
    NozzleVolumeType              m_all_flow { NozzleVolumeType::nvtStandard };
    MultiSwitchButton            *m_flow_toggle { nullptr };
    // -1: the toggle picks the shared column under All (m_all_flow); else the High Flow head whose printed column
    // it picks (PerHeadProcess::flow_key). m_flow_toggle_updating mutes the handler during show_flow_toggle.
    int                           m_flow_toggle_head { -1 };
    bool                          m_flow_toggle_updating { false };
    // Set while the code selects an entry of m_extruder_switch (a rebuild of the row, the
    // sidebar's page change through select_tool_head): the selection handler then shows no nozzle
    // tab in the sidebar; a click on a tool head does (Sidebar::show_nozzle_tab).
    bool                          m_head_selection_by_program { false };
    // Long ("Extruder 2 · 0.6") and short ("E2 · 0.6") selector labels from generate_extruder_options;
    // fit_head_selector picks the set that fits (HighFlowNotices::head_selector_fit) on resize and rebuild.
    std::vector<wxString>         m_head_labels_long;
    std::vector<wxString>         m_head_labels_short;
    bool                          m_head_labels_short_shown { false };
    bool                          m_head_fit_pending { false };
    void                          fit_head_selector();
    // Applies the rule of the flow toggle's visibility (see the definition); called after every
    // show of the row, which wxSizer::ShowItems shows the toggle with.
    void                          show_flow_toggle();
    // The active page has an indexed key whose columns differ by flow (a speed); the Quality page
    // (line widths alone, flow-independent) has none and shows no toggle.
    bool                          page_has_flow_dependent_key() const;

public:
	// BBS
	Tab(ParamsPanel* parent, const wxString& title, Preset::Type type);

    ~Tab() {}

	wxWindow*	parent() const { return m_parent; }
	wxString	title()	 const { return m_title; }
	std::string	name()	 const { return m_presets->name(); }
    Preset::Type type()  const { return m_type; }
    // The tab is already constructed.
    bool 		completed() const { return m_completed; }
	virtual bool supports_printer_technology(const PrinterTechnology tech) const = 0;

	void		create_preset_tab();
    void        add_scaled_button(wxWindow* parent, ScalableButton** btn, const std::string& icon_name,
                                  const wxString& label = wxEmptyString,
                                  long style = wxBU_EXACTFIT | wxNO_BORDER);
    void        add_scaled_bitmap(wxWindow* parent, ScalableBitmap& btn, const std::string& icon_name);
	void		update_ui_items_related_on_parent_preset(const Preset* selected_preset_parent);
    void		load_current_preset();
    //BBS: reactive preset combo box
    void        reactive_preset_combo_box();
	void        rebuild_page_tree();
    void		update_btns_enabling();
    void		update_preset_choice();
    // Select a new preset, possibly delete the current one.
    bool select_preset(std::string preset_name = "", bool delete_current = false, const std::string &last_selected_ph_printer_name = "", bool force_select = false, bool force_no_transfer = false);
	bool		may_discard_current_dirty_preset(PresetCollection* presets = nullptr, const std::string& new_printer_name = "", bool no_transfer = false, bool no_transfer_variant = false);

    virtual void    clear_pages();
    virtual void    update_description_lines();
    virtual void    activate_selected_page(std::function<void()> throw_if_canceled);

	void		OnTreeSelChange(wxCommandEvent& event);
	void		OnKeyDown(wxKeyEvent& event);

	void		compare_preset();
	void		transfer_options(const std::string&name_from, const std::string&name_to, std::vector<std::string> options);
	//BBS: add project embedded preset relate logic
	void        save_preset(std::string name = std::string(), bool detach = false, bool save_to_project = false, bool from_input = false, std::string input_name = "");
	//void		save_preset(std::string name = std::string(), bool detach = false);

	void		delete_preset();
	void		toggle_show_hide_incompatible();
	void		update_show_hide_incompatible_button();
	void		update_ui_from_settings();
	void		update_label_colours();
	void		decorate();
	void		update_changed_ui();
	void		get_sys_and_mod_flags(const std::string& opt_key, bool& sys_page, bool& modified_page);
    void        update_changed_tree_ui();
	void		update_undo_buttons();
    void        update_extruder_switch_colors();
    void        update_all_extruder_options_status();
    void        check_extruder_options_status(int index, bool &sys_extruder, bool &modified_extruder, const std::vector<PageShp>& pages_to_check);

	void		on_roll_back_value(const bool to_sys = false);

	PageShp		add_options_page(const wxString& title, const std::string& icon, bool is_extruder_pages = false);
	static wxString translate_category(const wxString& title, Preset::Type preset_type);

	virtual void	OnActivate();
	virtual void	on_preset_loaded() {}
	virtual void	build() = 0;
	virtual void	update() = 0;
	virtual void	toggle_options() = 0;
	virtual void	init_options_list();
	std::string	options_list_storage_key(const std::string& opt_key) const;
    virtual void    update_custom_dirty(std::vector<std::string> &dirty_options, std::vector<std::string> &nonsys_options) {}
	void			load_initial_data();
	void			update_dirty();
	//BBS update plater presets if update_plater_presets = true
	void			update_tab_ui(bool update_plater_presets = false);
	void			load_config(const DynamicPrintConfig& config);
	virtual void	reload_config();
    void            update_mode();
    void            update_visibility();
    virtual void    msw_rescale();
    virtual void	sys_color_changed();
	Field*			get_field(const t_config_option_key& opt_key, int opt_index = -1) const;
	Line*			get_line(const t_config_option_key& opt_key);
	std::pair<OG_CustomCtrl*, bool*> get_custom_ctrl_with_blinking_ptr(const t_config_option_key& opt_key, int opt_index = -1);

    Field*          get_field(const t_config_option_key &opt_key, Page** selected_page, int opt_index = -1);
    void            toggle_option(const std::string &opt_key, bool toggle, int opt_index = -1);
    void            toggle_line(const std::string &opt_key, bool toggle, int opt_index = -1); // BBS: hide some line
    void            set_option_label(const std::string &opt_key, const wxString &label, int opt_index = -1);

    // Live state of the settings row that owns an option, read from the built pages.
    struct SettingRowState
    {
        bool     visible{true}; // false when ConfigManipulation hides the row
        wxString label;         // Line::label the row draws (may change at runtime)
        bool     multi{false};  // row packs several options, so label is precomposed
    };
    SettingRowState setting_row_state(const std::string &opt_id) const;

	wxSizer*		description_line_widget(wxWindow* parent, ogStaticText** StaticText, wxString text = wxEmptyString);
	bool			current_preset_is_dirty() const;
	bool			saved_preset_is_dirty() const;
	void            update_saved_preset_from_current_preset();
    void            update_pages_with_multi_variant();

	DynamicPrintConfig*	get_config() { return m_config; }
    PresetCollection *  get_presets() { return m_presets; }
    TabPresetComboBox *  get_combo_box() { return m_presets_choice; }

	virtual void    on_value_change(const std::string& opt_key, const boost::any& value);

    void            update_wiping_button_visibility();
	void			activate_option(const std::string& opt_key, const wxString& category);
    void			apply_searcher();
	void			cache_config_diff(const std::vector<std::string>& selected_options, const DynamicPrintConfig* config = nullptr);
	void			apply_config_from_cache();
    void            show_timelapse_warning_dialog();

	const std::map<wxString, std::string>& get_category_icon_map() { return m_category_icon; }
	//BBS: GUI refactor
	bool update_current_page_in_background(int& item);
	void unselect_tree_item();
	// BBS: new layout
	void set_expanded(bool value);
	void restore_last_select_item();
	// page_build_pending() says whether the selected page has groups without controls, and
	// page_build_step() builds one.
	bool page_build_pending() const;
	bool page_build_step();

	static bool validate_custom_gcode(const wxString& title, const std::string& gcode);
	bool        validate_custom_gcodes();
	bool        validate_filament_temperature_pairs();
    bool        validate_custom_gcodes_was_shown{ false };
    void        set_just_edit(bool just_edit);

    void						edit_custom_gcode(const t_config_option_key& opt_key);
    virtual const std::string&	get_custom_gcode(const t_config_option_key& opt_key);
    virtual void				set_custom_gcode(const t_config_option_key& opt_key, const std::string& value);

    void        update_extruder_variants(int extruder_id = -1, bool reload = true);
    void        switch_excluder(int extruder_id = -1, bool reload = true);
    void        sync_excluder();
	void        parse_extruder_selection(int selection, int &extruder_id, NozzleVolumeType &nozzle_type);
    int         calculate_selection_index_for_extruder(int extruder_id, NozzleVolumeType nozzle_type);
	bool        get_extruder_sync_enable_state(int extruder_id);
	int         get_current_active_extruder();

	std::vector<wxString>  generate_extruder_options();
    // Snapmaker Orca: shows the sync button next to a shown extruder switch, never in flow selector mode.
    void                   show_extruder_sync();
    // Snapmaker Orca: shows the Standard / High Flow column of a flow type in the filament tab's
    // variant list (HighFlowNotices::variant_column_for_type) or the process tab's flow selector.
    // No-op when the preset has no such column or the tab has no such control.
    void                   select_flow_column(NozzleVolumeType type);
    // Snapmaker Orca: the sidebar's nozzle tab of `head` was clicked: the speed selector selects
    // that head, the flow selector the column of `type`.
    void                   select_tool_head(size_t head, NozzleVolumeType type);
    // Activates the page of category `category` ("Speed") without focusing or highlighting a field.
    void                   select_page_by_category(const wxString &category);
    // Snapmaker Orca, speed selector: selected entry (0 = All, k = tool head k-1) and its head (-1 for All),
    // a head's flow (project_config), the preset column a selection edits, and the entries' labels and tooltips.
    int                    head_selection() const;
    int                    selected_head() const { return head_selection() - 1; }
    NozzleVolumeType       head_flow(size_t head) const;
    // The flow whose speeds column the tool head prints (PerHeadProcess::effective_flow): the
    // nozzle's, or the Standard column chosen for a High Flow nozzle with the flow toggle. Every
    // reader of a head's speeds on this tab asks this one; head_flow names the nozzle.
    NozzleVolumeType       head_speed_flow(size_t head) const;
    // The flow toggle under a selected High Flow tool head: writes the head's entry (the nozzle's
    // own flow clears it) and refreshes the page, the entries, the sidebar hint and the plate.
    void                   choose_head_flow(size_t head, NozzleVolumeType flow);
    int                    head_selection_column(int selection) const;
    void                   update_head_entries();
    NozzleVolumeType       get_actual_nozzle_volume_type(int extruder_id);

protected:
	void			create_line_with_widget(ConfigOptionsGroup* optgroup, const std::string& opt_key, const std::string& path, widget_t widget);
	wxSizer*		compatible_widget_create(wxWindow* parent, PresetDependencies &deps);
	void 			compatible_widget_reload(PresetDependencies &deps);
	void			load_key_value(const std::string& opt_key, const boost::any& value, bool saved_value = false);

	//BBS: GUI refactor
	// return true if cancelled
	bool			tree_sel_change_delayed(wxCommandEvent& event);
	void			on_presets_changed();
	void			update_printer_agent_if_needed();
	void			build_preset_description_line(ConfigOptionsGroup* optgroup);
	void			update_preset_description_line();
	void			update_frequently_changed_parameters();
	void			set_tooltips_text();
    void			filter_diff_option(std::vector<std::string> &options);

    ConfigManipulation m_config_manipulation;
    std::string m_last_sparse_infill_rotate_template_value;
    ConfigManipulation get_config_manipulation();
    friend class EditGCodeDialog;
    friend class PublishSettingsDialog;
};

class TabPrint : public Tab
{
public:
	//BBS: GUI refactor
	TabPrint(ParamsPanel* parent, Preset::Type type = Preset::TYPE_PRINT) :
        Tab(parent, _(L("Process")), type) {}
	~TabPrint() {}

	void		build() override;
	void		reload_config() override;
	void		update_description_lines() override;
	void		toggle_options() override;
	void		update() override;
	void		clear_pages() override;
	void		msw_rescale() override;
	void		sys_color_changed() override;
	bool 		supports_printer_technology(const PrinterTechnology tech) const override { return tech == ptFFF; }
	// Snapmaker Orca, the speed picker: shows the Speed page with tool head `head` selected and the
	// focus on the picker (the nozzle tab hint and the notices lead here).
	void		focus_speed_source_picker(size_t head);
	// Snapmaker Orca: clears the line widths set for tool head `head` (the "Clear" of the notice
	// raised when the head's nozzle size changes with an absolute width set for it).
	void		clear_head_widths(size_t head);

protected:
	// Snapmaker Orca, the speed selector (libslic3r/PerHeadProcess.hpp): the hooks of the option
	// groups of the Speed page, installed by build() for the process tab alone.
	void		install_head_hooks();
	bool		before_head_change(const std::string &opt_key, int &opt_index);
	void		after_head_change(const std::string &opt_key, int opt_index);
	bool		before_head_revert(const std::string &opt_key, bool to_sys);
	bool		head_display_source(const std::string &opt_key, int opt_index, const DynamicPrintConfig *&config, int &index);
	wxString	head_values_tooltip(const std::string &opt_key) const;
	// The line and the link under the selector: what the selection means, and the clear of the
	// values set for the selected tool head (no confirmation) or for every head (confirmed).
	wxSizer*	per_head_line_widget(wxWindow *parent, int label_em, bool stacked = false);
	wxString	head_selection_description() const;
	void		clear_head_values();
	// The Quality page carries the same selector, line, picker and link for the nine line widths:
	// which page is active, the width keys among a head's values, and the head-editable keys of
	// the active page (its clear link clears those alone).
	bool		quality_page_active() const;
	static std::vector<std::string> head_width_keys(const std::vector<std::string> &keys);
	std::set<std::string> page_head_keys() const;
	void		refresh_after_head_change(bool relayout_columns);
	// The "Speeds from" picker of the Speed page for the selected tool head. A pick calls PerHeadProcess::set_chosen
	// ("" = automatic) and refreshes page, entries, sidebar hint and plate. Keys: arrows move the highlight only,
	// Enter commits, Escape cancels.
	void		update_speed_source_picker();
	void		choose_speed_source(const std::string &name);
	void		on_speed_source_key(wxKeyEvent &event);

private:
	wxString	per_head_process_description() const;
	ogStaticText*	m_recommended_thin_wall_thickness_description_line = nullptr;
	ogStaticText*	m_top_bottom_shell_thickness_explanation = nullptr;
	// Snapmaker Orca: the line on the Speed page that names the process presets the tool heads of
	// another nozzle size print with (libslic3r/PerHeadProcess.hpp).
	ogStaticText*	m_per_head_process_line = nullptr;
	HyperLink*		m_per_head_clear_link = nullptr;
	// The speed picker row: label, combo and the reset to the automatic preset; the preset name
	// behind every item ("" for the automatic item and the headers). Cleared with the page.
	wxStaticText*	m_speed_source_label = nullptr;
	::ComboBox*		m_speed_source_combo = nullptr;
	ScalableButton*	m_speed_source_reset = nullptr;
	ogStaticText*	m_speed_source_note = nullptr;   // the line under the picker: what the preset supplies on this page
	int				m_speed_source_label_em = 15;    // the label column of the page the picker is on (15 Speed, 20 Quality)
	bool			m_speed_source_stacked = false;  // Quality page: the label on its own line above a full-width combo
	std::vector<std::string> m_speed_source_items;
	// The sources of the tool heads (PerHeadProcess::head_sources) and the composed keys edited under
	// All, refreshed with the entries; read by the display of a tool head's fields.
	std::vector<PerHeadProcess::Source> m_head_sources;
	std::set<std::string>               m_all_edited_keys;
	::CheckBox*		m_legacy_support_check = nullptr;
};

class TabPrintModel : public TabPrint
{
public:
	//BBS: GUI refactor
	TabPrintModel(ParamsPanel* parent, std::vector<std::string> const & keys);
	~TabPrintModel() {}

	void build() override;

	void set_model_config(std::map<ObjectBase *, ModelConfig *> const & object_configs);

	bool has_model_config() const { return !m_object_configs.empty(); }

	void update_model_config();

	virtual void reset_model_config();

	bool has_key(std::string const &key);

protected:
	virtual void    activate_selected_page(std::function<void()> throw_if_canceled) override;

	virtual void    on_value_change(const std::string& opt_key, const boost::any& value) override;

	virtual void    notify_changed(ObjectBase * object) = 0;

	virtual void	reload_config() override;

	virtual void	update_custom_dirty(std::vector<std::string> &dirty_options, std::vector<std::string> &nonsys_options) override;

protected:
	std::vector<std::string> m_keys;
	PresetCollection m_prints;
	Tab * m_parent_tab;
	std::map<ObjectBase *, ModelConfig *> m_object_configs;
	std::vector<std::string> m_all_keys;
	std::vector<std::string> m_null_keys;
	bool m_back_to_sys = false;
};


class TabPrintPlate : public TabPrintModel
{
public:
	//BBS: GUI refactor
	TabPrintPlate(ParamsPanel* parent);
	~TabPrintPlate() {}
	void build() override;
	void reset_model_config() override;
	int show_spiral_mode_settings_dialog(bool is_object_config) { return m_config_manipulation.show_spiral_mode_settings_dialog(is_object_config); }
	// Disables the user-defined filament print order while a mixed-color filament exists.
	void update_mixed_filament_seq_state();

protected:
	virtual void    on_value_change(const std::string& opt_key, const boost::any& value) override;
	virtual void    notify_changed(ObjectBase* object) override;
	virtual void	update_custom_dirty(std::vector<std::string> &dirty_options, std::vector<std::string> &nonsys_options) override;
};

class TabPrintObject : public TabPrintModel
{
public:
	//BBS: GUI refactor
	TabPrintObject(ParamsPanel* parent);
	~TabPrintObject() {}
protected:
	virtual void    notify_changed(ObjectBase * object) override;
};

class TabPrintPart : public TabPrintModel
{
public:
	//BBS: GUI refactor
	TabPrintPart(ParamsPanel* parent);
	~TabPrintPart() {}
protected:
	virtual void    notify_changed(ObjectBase * object) override;
};

class TabPrintLayer : public TabPrintModel
{
public:
	//BBS: GUI refactor
	TabPrintLayer(ParamsPanel* parent);
	~TabPrintLayer() {}
protected:
	virtual void    notify_changed(ObjectBase* object) override;
	virtual void    update_custom_dirty(std::vector<std::string> &dirty_options, std::vector<std::string> &nonsys_options) override;
};

class TabFilament : public Tab
{
private:
	ogStaticText*	m_volumetric_speed_description_line {nullptr};
	ogStaticText*	m_cooling_description_line {nullptr};

    void            add_filament_overrides_page();
    void            update_filament_overrides_page(const DynamicPrintConfig* printers_config);
	void 			update_volumetric_flow_preset_hints();

    std::map<std::string, ::CheckBox*> m_overrides_options;

public:
	//BBS: GUI refactor
	TabFilament(ParamsPanel* parent) :
		Tab(parent, _(L("Filament")), Slic3r::Preset::TYPE_FILAMENT) {}
	~TabFilament() {}

	void		build() override;
	void		reload_config() override;
	void		update_description_lines() override;
	void		toggle_options() override;
	void		update() override;
    void        init_options_list() override;
    void        clear_pages() override;
	bool 		supports_printer_technology(const PrinterTechnology tech) const override { return tech == ptFFF; }

	void		on_value_change(const std::string& opt_key, const boost::any& value) override;

    const std::string&	get_custom_gcode(const t_config_option_key& opt_key) override;
    void				set_custom_gcode(const t_config_option_key& opt_key, const std::string& value) override;
};

class TabPrinter : public Tab
{
private:
	bool		m_use_silent_mode = false;
	void		append_option_line(ConfigOptionsGroupShp optgroup, const std::string opt_key, const std::string& label_path = "");
	bool		m_rebuild_kinematics_page = false;
	void        update_input_shaper_menu(GCodeFlavor flavor);


    std::vector<PageShp>			m_pages_fff;
    std::vector<PageShp>			m_pages_sla;

	// Snapmaker Orca: the flow combos of the extruder pages that are built at the moment, by
	// extruder index (page controls come and go with the active page).
	std::map<int, ::ComboBox*>		m_nozzle_flow_combos;

public:
	ScalableButton*	m_reset_to_filament_color = nullptr;

	size_t		m_extruders_count;
	size_t		m_extruders_count_old = 0;
	size_t		m_initial_extruders_count;
	size_t		m_sys_extruders_count;
	size_t		m_cache_extruder_count = 0;
	std::vector<std::string> m_extruder_variant_list;
	std::string m_base_preset_name;

    PrinterTechnology               m_printer_technology = ptFFF;

	//BBS: GUI refactor
    TabPrinter(ParamsPanel* parent) :
        Tab(parent, _L("Machine"), Slic3r::Preset::TYPE_PRINTER) {}
	~TabPrinter() {}

	void		build() override;
    void		build_fff();
    void		build_sla();
	void		reload_config() override;
	void		activate_selected_page(std::function<void()> throw_if_canceled) override;
	void		clear_pages() override;
	void		toggle_options() override;
    void		update() override;
    void		update_fff();
    void		update_sla();
    void        update_pages(); // update m_pages according to printer technology
	void        on_gcode_flavor_changed();
	void		extruders_count_changed(size_t extruders_count);
	PageShp		build_kinematics_page();
	void		build_unregular_pages(bool from_initial_build = false);
	void		on_preset_loaded() override;
	void		init_options_list() override;
	void		msw_rescale() override;
	bool 		supports_printer_technology(const PrinterTechnology /* tech */) const override { return true; }

	void		set_extruder_volume_type(int extruder_id, NozzleVolumeType type);
	// Snapmaker Orca: the "Nozzle flow" line of the extruder pages. It edits the project's
	// "nozzle_volume_type" (no key of the printer preset), so it is a widget, kept in step with
	// the Flow row of the sidebar by update_nozzle_flow_lines().
	wxSizer*	create_nozzle_flow_widget(wxWindow* parent, int extruder_idx);
	void		update_nozzle_flow_lines(bool refresh_page = true);
	void		on_value_change(const std::string& opt_key, const boost::any& value) override;

	wxSizer*	create_bed_shape_widget(wxWindow* parent);
	void		cache_extruder_cnt(const DynamicPrintConfig* config = nullptr);
	bool		apply_extruder_cnt_from_cache();
	void		refresh_printer_agent_dropdown() const;
};

class TabSLAMaterial : public Tab
{
public:
	//BBS: GUI refactor
    TabSLAMaterial(ParamsPanel* parent) :
		Tab(parent, _(_devL("Material Settings")), Slic3r::Preset::TYPE_SLA_MATERIAL) {}
    ~TabSLAMaterial() {}

	void		build() override;
	void		reload_config() override;
	void		toggle_options() override;
	void		update() override;
	bool 		supports_printer_technology(const PrinterTechnology tech) const override { return tech == ptSLA; }
};

class TabSLAPrint : public Tab
{
public:
	//BBS: GUI refactor
    TabSLAPrint(ParamsPanel* parent) :
        Tab(parent, _(L("Process Settings")), Slic3r::Preset::TYPE_SLA_PRINT) {}
    ~TabSLAPrint() {}

	ogStaticText* m_support_object_elevation_description_line = nullptr;

    void		build() override;
	void		reload_config() override;
	void		update_description_lines() override;
	void		toggle_options() override;
    void		update() override;
	void		clear_pages() override;
	bool 		supports_printer_technology(const PrinterTechnology tech) const override { return tech == ptSLA; }
};

} // GUI
} // Slic3r

#endif /* slic3r_Tab_hpp_ */
