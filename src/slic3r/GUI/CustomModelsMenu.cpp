#include "CustomModelsMenu.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GLCanvas3D.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "Selection.hpp"
#include "format.hpp"
#include "wxExtensions.hpp"

#include "libslic3r/CustomModels.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/menu.h>
#include <wx/textdlg.h>
#include <wx/utils.h>
#include <wx/window.h>

#include <algorithm>

namespace fs = boost::filesystem;

namespace Slic3r {
namespace GUI {

fs::path custom_models_dir() { return custom_models::library_dir(Slic3r::data_dir()); }

// ---- actions -------------------------------------------------------------------------------------

void add_custom_model(const fs::path &file)
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;

    boost::system::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        show_error(plater, format_wxstr(_L("The custom model \"%1%\" is no longer in the Custom Models folder."), from_path(file.filename())));
        return;
    }

    LoadStrategy strategy = LoadStrategy::LoadModel;
    // A 3MF keeps its object and part settings; it is added to this project, never opened as one.
    if (custom_models::file_type(file) == custom_models::FileType::Project3mf)
        strategy = strategy | LoadStrategy::KeepObjectSettings;

    // One undo step for the whole drop (object, parts, settings, filament clamping).
    Plater::TakeSnapshot snapshot(plater, "Add Custom Model: " + file.stem().string());
    const std::vector<size_t> loaded_indexes = plater->load_files({file}, strategy);

    Model &         model = plater->model();
    ModelObjectPtrs loaded_objects;
    loaded_objects.reserve(loaded_indexes.size());
    for (const size_t idx : loaded_indexes)
        if (idx < model.objects.size())
            loaded_objects.push_back(model.objects[idx]);
    model.InitializeAssemblyPositions(loaded_objects);
}

namespace {

// Objects (with the instance to save) the selection covers, or none when it is not made of whole
// objects / instances.
std::vector<custom_models::SourceObject> selected_sources()
{
    std::vector<custom_models::SourceObject> sources;
    Plater *                                 plater = wxGetApp().plater();
    if (plater == nullptr || plater->canvas3D() == nullptr)
        return sources;
    const Selection &selection = plater->canvas3D()->get_selection();
    if (!(selection.is_single_full_instance() || selection.is_single_full_object() || selection.is_multiple_full_instance() ||
          selection.is_multiple_full_object()))
        return sources;
    const Model &model = plater->model();
    for (const auto &kv : selection.get_content()) {
        if (kv.first < 0 || size_t(kv.first) >= model.objects.size())
            continue;
        custom_models::SourceObject src;
        src.object       = model.objects[size_t(kv.first)];
        src.instance_idx = kv.second.empty() ? 0 : size_t(*kv.second.begin());
        sources.push_back(src);
    }
    return sources;
}

} // namespace

bool can_save_selection_as_custom_model() { return !selected_sources().empty(); }

void save_selection_as_custom_model()
{
    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;
    const std::vector<custom_models::SourceObject> sources = selected_sources();
    if (sources.empty())
        return;

    const wxString typed = wxGetTextFromUser(_L("Name for the custom model:"), _L("Save to Custom Models"),
                                             from_u8(sources.front().object->name), plater);
    if (typed.IsEmpty())
        return;

    const std::string file_name = custom_models::library_file_name(into_u8(typed));
    if (file_name.empty()) {
        show_error(plater, _L("That name cannot be used as a file name. Please choose another one."));
        return;
    }

    const fs::path target = custom_models_dir() / file_name;
    boost::system::error_code ec;
    if (fs::exists(target, ec)) {
        MessageDialog dlg(plater,
                          format_wxstr(_L("A custom model named \"%1%\" already exists. Do you want to replace it?"), from_u8(file_name)),
                          _L("Save to Custom Models"), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION);
        if (dlg.ShowModal() != wxID_YES)
            return;
    }

    custom_models::SaveResult result;
    {
        wxBusyCursor busy;
        result = custom_models::save_objects(sources, target);
    }
    if (!result.ok) {
        BOOST_LOG_TRIVIAL(error) << "Save to Custom Models failed: " << result.error;
        show_error(plater, format_wxstr(_L("Could not save the custom model: %1%"), from_u8(result.error)));
        return;
    }
    if (NotificationManager *notifications = plater->get_notification_manager())
        notifications->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
                                         into_u8(format_wxstr(_L("Saved to Custom Models: %1%"), from_u8(file_name))));
}

void open_custom_models_folder()
{
    fs::path dir = custom_models_dir();
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    dir.make_preferred();
    desktop_open_folder(dir.string());
}

// ---- the submenu ---------------------------------------------------------------------------------

struct CustomModelsMenu::State
{
    wxMenu *             menu   = nullptr;
    wxMenu *             owner  = nullptr;
    wxWindow *           parent = nullptr;
    int                  first_id = 0;
    std::size_t          max_ids  = 0;
    std::vector<fs::path> paths;           // paths[id - first_id]
    std::size_t          dynamic_items = 0; // items at the top of `menu` that refresh() replaces
};

namespace {

// A menu label from a file or folder name: '&' and tabs would be read as mnemonics / accelerators,
// and a very long name would make the menu wider than the screen.
wxString menu_label(const std::string &utf8)
{
    wxString label = from_u8(utf8);
    label.Replace("\t", " ");
    label.Replace("&", "&&");
    const size_t max_chars = 60;
    if (label.length() > max_chars)
        label = label.Left(max_chars - 3) + "...";
    return label;
}

void add_item(wxMenu *menu, wxMenuItem *item, int &pos)
{
    if (pos < 0)
        menu->Append(item);
    else
        menu->Insert(size_t(pos++), item);
}

void add_folder(CustomModelsMenu::State &st, wxMenu *menu, const custom_models::Folder &folder, int &pos)
{
    for (const custom_models::Folder &sub : folder.folders) {
        wxMenu *    sub_menu = new wxMenu;
        wxMenuItem *item     = new wxMenuItem(menu, wxID_ANY, menu_label(sub.label), wxEmptyString, wxITEM_NORMAL, sub_menu);
        add_item(menu, item, pos);
        int sub_pos = -1;
        add_folder(st, sub_menu, sub, sub_pos);
    }
    for (const custom_models::Entry &entry : folder.files) {
        if (st.paths.size() >= st.max_ids)
            break;
        const int   id   = st.first_id + int(st.paths.size());
        wxMenuItem *item = new wxMenuItem(menu, id, menu_label(entry.label));
        st.paths.push_back(entry.path);
        add_item(menu, item, pos);
    }
}

} // namespace

CustomModelsMenu::CustomModelsMenu() = default;
CustomModelsMenu::~CustomModelsMenu() = default;

wxMenu *CustomModelsMenu::create(wxMenu *owner, wxWindow *parent)
{
    m_state         = std::make_shared<State>();
    State &st       = *m_state;
    st.menu         = new wxMenu;
    st.owner        = owner;
    st.parent       = parent;
    st.max_ids      = custom_models::ScanLimits().max_files;
    st.first_id     = wxWindow::NewControlId(int(st.max_ids));

    // One handler for the whole id range; it only holds the shared state, so it stays harmless
    // when the menu is rebuilt (a theme change) and this object is replaced.
    std::shared_ptr<State> state = m_state;
    auto handler = [state](wxCommandEvent &evt) {
        const int idx = evt.GetId() - state->first_id;
        if (idx < 0 || size_t(idx) >= state->paths.size()) {
            evt.Skip();
            return;
        }
        const fs::path file = state->paths[size_t(idx)];
        // Let the menu close first: loading shows a progress dialog.
        wxGetApp().CallAfter([file]() { add_custom_model(file); });
    };
    const int last_id = st.first_id + int(st.max_ids) - 1;
#ifdef __WXMSW__
    // Like the other submenus: Windows delivers the item events to the menu that was popped up.
    (owner != nullptr ? owner : st.menu)->Bind(wxEVT_MENU, handler, st.first_id, last_id);
#else
    st.menu->Bind(wxEVT_MENU, handler, st.first_id, last_id);
#endif

    st.menu->AppendSeparator();
    append_menu_item(st.menu, wxID_ANY, _L("Save selected object to Custom Models") + dots,
                     _L("Save the selected object, with its parts, modifiers, settings overrides, painting and filament, to the Custom Models folder"),
                     [](wxCommandEvent &) { wxGetApp().CallAfter([]() { save_selection_as_custom_model(); }); }, "", owner,
                     []() { return can_save_selection_as_custom_model(); }, parent);
    append_menu_item(st.menu, wxID_ANY, _L("Open Custom Models folder"), _L("Open the folder the custom models are kept in"),
                     [](wxCommandEvent &) { open_custom_models_folder(); }, "", owner, []() { return true; }, parent);

    refresh();
    return st.menu;
}

void CustomModelsMenu::refresh()
{
    if (!m_state || m_state->menu == nullptr)
        return;
    State &st = *m_state;

    while (st.dynamic_items > 0 && st.menu->GetMenuItemCount() > 0) {
        st.menu->Destroy(st.menu->FindItemByPosition(0));
        --st.dynamic_items;
    }
    st.dynamic_items = 0;
    st.paths.clear();

    const custom_models::Folder library = custom_models::scan_library(custom_models_dir());
    int                         pos     = 0;
    const size_t                before  = st.menu->GetMenuItemCount();
    if (library.empty()) {
        wxMenuItem *empty = new wxMenuItem(st.menu, wxID_ANY, _L("No custom models yet"));
        add_item(st.menu, empty, pos);
        empty->Enable(false);
    } else {
        add_folder(st, st.menu, library, pos);
        if (library.truncated) {
            wxMenuItem *more = new wxMenuItem(st.menu, wxID_ANY, _L("More models are in the folder"));
            add_item(st.menu, more, pos);
            more->Enable(false);
        }
    }
    st.dynamic_items = st.menu->GetMenuItemCount() - before;
}

} // namespace GUI
} // namespace Slic3r
