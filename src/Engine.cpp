#include "Engine.h"

#include "wx/app.h"
#include "wx/timer.h"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"

#include "slic3r/GUI/Plater.cpp"

using namespace Slic3r::GUI;

int Snapmaker_Orca_Engine::s_time_gui_load = 500;
int Snapmaker_Orca_Engine::s_time_check_export = 100;
int Snapmaker_Orca_Engine::s_time_delay_close  = 500;

Snapmaker_Orca_Engine::Snapmaker_Orca_Engine(std::vector<std::string>& user_inputs) : m_OriFiles(user_inputs) {}

void Snapmaker_Orca_Engine::init()
{
    if (!m_load_gui_timer) {
        m_load_gui_timer = new wxTimer(this, wxNewId());
    }
    this->Bind(wxEVT_TIMER, [this](wxTimerEvent& event) {
        this->on_gui_loaded();
    }, m_load_gui_timer->GetId());
    m_load_gui_timer->StartOnce(s_time_gui_load);
}

void Snapmaker_Orca_Engine::on_gui_loaded()
{
    // load information from gui
    m_gui_app = (GUI_App*) wxTheApp;
    if (!m_gui_app) {
        return;
    }

    m_gui_main_frame = m_gui_app->mainframe;
    if (!m_gui_main_frame) {
        return;
    }

    m_gui_plater = m_gui_main_frame->plater();
    if (!m_gui_plater) {
        return;
    }

    m_load_gui_timer->Stop();
    delete m_load_gui_timer;
    m_load_gui_timer = nullptr;

    run_engine();
}

void Snapmaker_Orca_Engine::on_time_check() {
    extern bool g_exported;
    if (g_exported) {
        m_check_export_timer->Stop();

        g_exported = false;

        ++m_task_index;

        if (m_task_index >= m_OriFiles.size()) {
            delete m_check_export_timer;
            m_check_export_timer = nullptr;

            close_engine();
        } else {
            do_next_task();
        }
    }
}

void Snapmaker_Orca_Engine::close_engine() {
    if (!m_delay_close_timer) {
        m_delay_close_timer = new wxTimer(this, wxNewId());
    }

    this->Bind(wxEVT_TIMER, [this](wxTimerEvent& event) {
        this->m_delay_close_timer->Stop();
        delete this->m_delay_close_timer;
        this->m_delay_close_timer = nullptr;

        this->m_gui_main_frame->Close(false);
    }, m_delay_close_timer->GetId());

    m_delay_close_timer->StartOnce(s_time_delay_close);
}


void Snapmaker_Orca_Engine::do_next_task() {

    if (m_task_index > 0) {
        m_gui_plater->new_project();
    }

    add_file_server(m_OriFiles[m_task_index]);
    slice_all_plates_server();
    export_gcode_server(false);

    if (!m_check_export_timer) {
        m_check_export_timer = new wxTimer(this, wxNewId());
    }
    this->Bind(wxEVT_TIMER, [this](wxTimerEvent& event) {
        this->on_time_check();    
    }, m_check_export_timer->GetId());
    m_check_export_timer->Start(s_time_check_export);

}

void Snapmaker_Orca_Engine::run_engine() {
    if (m_OriFiles.size() >= 1) {
        do_next_task();
    } else {
        close_engine();
    }
}

void Snapmaker_Orca_Engine::add_file_server(std::string& filePath) {

    wxArrayString input_files;
    input_files.push_back(filePath);

    if (input_files.empty())
        return;

    std::vector<fs::path> paths;
    for (const auto& file : input_files)
        paths.emplace_back(into_path(file));

    std::string snapshot_label;
    assert(!paths.empty());

    snapshot_label = "Import Objects";
    snapshot_label += ": ";
    snapshot_label += encode_path(paths.front().filename().string().c_str());
    for (size_t i = 1; i < paths.size(); ++i) {
        snapshot_label += ", ";
        snapshot_label += encode_path(paths[i].filename().string().c_str());
    }

    // BBS: check file types
    auto loadfiles_type  = LoadFilesType::NoFile;
    auto amf_files_count = m_gui_plater->get_3mf_file_count(paths);

    if (paths.size() > 1 && amf_files_count < paths.size()) {
        loadfiles_type = LoadFilesType::Multiple3MFOther;
    }
    if (paths.size() > 1 && amf_files_count == paths.size()) {
        loadfiles_type = LoadFilesType::Multiple3MF;
    }
    if (paths.size() > 1 && amf_files_count == 0) {
        loadfiles_type = LoadFilesType::MultipleOther;
    }
    if (paths.size() == 1 && amf_files_count == 1) {
        loadfiles_type = LoadFilesType::Single3MF;
    };
    if (paths.size() == 1 && amf_files_count == 0) {
        loadfiles_type = LoadFilesType::SingleOther;
    };

    auto first_file = std::vector<fs::path>{};
    auto tmf_file   = std::vector<fs::path>{};
    auto other_file = std::vector<fs::path>{};

    switch (loadfiles_type) {
    case LoadFilesType::Single3MF: m_gui_plater->open_3mf_file(paths[0]); break;

    case LoadFilesType::SingleOther: {
        Plater::TakeSnapshot snapshot(m_gui_plater, snapshot_label);
        if (!m_gui_plater->load_files(paths, LoadStrategy::LoadModel, false).empty()) {
            if (m_gui_plater->get_project_name() == _L("Untitled") && paths.size() > 0) {
                m_gui_plater->p->set_project_filename(wxString::FromUTF8(paths[0].string()));
            }
            wxGetApp().mainframe->update_title();
        }
        break;
    }
    case LoadFilesType::Multiple3MF:
        first_file = std::vector<fs::path>{paths[0]};
        for (auto i = 0; i < paths.size(); i++) {
            if (i > 0) {
                other_file.push_back(paths[i]);
            }
        };

        m_gui_plater->open_3mf_file(first_file[0]);
        if (!m_gui_plater->load_files(other_file, LoadStrategy::LoadModel).empty()) {
            wxGetApp().mainframe->update_title();
        }
        break;

    case LoadFilesType::MultipleOther: {
        Plater::TakeSnapshot snapshot(m_gui_plater, snapshot_label);
        if (!m_gui_plater->load_files(paths, LoadStrategy::LoadModel, true).empty()) {
            if (m_gui_plater->get_project_name() == _L("Untitled") && paths.size() > 0) {
                m_gui_plater->p->set_project_filename(wxString::FromUTF8(paths[0].string()));
            }
            wxGetApp().mainframe->update_title();
        }
        break;
    }
    case LoadFilesType::Multiple3MFOther:
        for (const auto& path : paths) {
            if (wxString(encode_path(path.filename().string().c_str())).EndsWith("3mf")) {
                if (first_file.size() <= 0)
                    first_file.push_back(path);
                else
                    tmf_file.push_back(path);
            } else {
                other_file.push_back(path);
            }
        }

        m_gui_plater->open_3mf_file(first_file[0]);
        m_gui_plater->load_files(tmf_file, LoadStrategy::LoadModel);
        if (!m_gui_plater->load_files(other_file, LoadStrategy::LoadModel, false).empty()) {
            wxGetApp().mainframe->update_title();
        }
        break;
    default: break;
    }
}

void Snapmaker_Orca_Engine::slice_all_plates_server() {
    if (m_gui_plater != nullptr) {
        BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ":received slice project event\n";
        // BBS update extruder params and speed table before slicing
        const Slic3r::DynamicPrintConfig& config       = wxGetApp().preset_bundle->full_config();
        auto&                             print        = m_gui_plater->get_partplate_list().get_current_fff_print();
        auto                              print_config = print.config();
        int                               numExtruders = wxGetApp().preset_bundle->filament_presets.size();

        Model::setExtruderParams(config, numExtruders);
        Model::setPrintSpeedTable(config, print_config);
        m_gui_plater->p->m_slice_all                = true;
        m_gui_plater->p->m_slice_all_only_has_gcode = true;
        m_gui_plater->p->m_cur_slice_plate          = 0;
        // select plate
        m_gui_plater->select_plate(m_gui_plater->p->m_cur_slice_plate);
        m_gui_plater->reslice();
        if (!m_gui_plater->p->m_is_publishing)
            m_gui_plater->select_view_3D("Preview");
        // BBS: wish to select all plates stats item
        m_gui_plater->p->preview->get_canvas3d()->_update_select_plate_toolbar_stats_item(true);
    }
}

void Snapmaker_Orca_Engine::export_gcode_server(bool prefer_removable) {
    if (m_gui_plater->p->model.objects.empty())
        return;

    // if (get_view3D_canvas3D()->get_gizmos_manager().is_in_editing_mode(true))
    //     return;

    if (m_gui_plater->p->process_completed_with_error == m_gui_plater->p->partplate_list.get_curr_plate_index())
        return;

    // If possible, remove accents from accented latin characters.
    // This function is useful for generating file names to be processed by legacy firmwares.
    fs::path default_output_file;
    try {
        // Update the background processing, so that the placeholder parser will get the correct values for the ouput file template.
        // Also if there is something wrong with the current configuration, a pop-up dialog will be shown and the export will not be performed.
        unsigned int state = m_gui_plater->p->update_restart_background_process(false, false);
        if (state & Plater::priv::UPDATE_BACKGROUND_PROCESS_INVALID)
            return;
        default_output_file = m_gui_plater->p->background_process.output_filepath_for_project("");
    } catch (const Slic3r::PlaceholderParserError& ex) {
        // Show the error with monospaced font.
        show_error(m_gui_plater, ex.what(), true);
        return;
    } catch (const std::exception& ex) {
        show_error(m_gui_plater, ex.what(), false);
        return;
    }
    default_output_file                            = fs::path(Slic3r::fold_utf8_to_ascii(default_output_file.string()));
    AppConfig&             appconfig               = *wxGetApp().app_config;
    RemovableDriveManager& removable_drive_manager = *wxGetApp().removable_drive_manager();
    // Get a last save path, either to removable media or to an internal media.
    std::string start_dir = appconfig.get_last_output_dir(default_output_file.parent_path().string(), prefer_removable);
    if (prefer_removable) {
        // Returns a path to a removable media if it exists, prefering start_dir. Update the internal removable drives database.
        start_dir = removable_drive_manager.get_removable_drive_path(start_dir);
        if (start_dir.empty())
            // Direct user to the last internal media.
            start_dir = appconfig.get_last_output_dir(default_output_file.parent_path().string(), false);
    }

    fs::path input_path, output_path;
    input_path = m_OriFiles[m_task_index];
    output_path = input_path.parent_path() / (input_path.stem().string() + ".gcode"); 

    if (!output_path.empty()) {
        bool path_on_removable_media = removable_drive_manager.set_and_verify_last_save_path(output_path.string());
        // bool path_on_removable_media = false;
        m_gui_plater->p->notification_manager->new_export_began(path_on_removable_media);
        m_gui_plater->p->exporting_status = path_on_removable_media ? ExportingStatus::EXPORTING_TO_REMOVABLE :
                                                                      ExportingStatus::EXPORTING_TO_LOCAL;
        m_gui_plater->p->last_output_path = output_path.string();
        m_gui_plater->p->last_output_dir_path = output_path.parent_path().string();
        m_gui_plater->p->export_gcode(output_path, path_on_removable_media);
        // Storing a path to AppConfig either as path to removable media or a path to internal media.
        // is_path_on_removable_drive() is called with the "true" parameter to update its internal database as the user may have shuffled
        // the external drives while the dialog was open.
        appconfig.update_last_output_dir(output_path.parent_path().string(), path_on_removable_media);

        try {
            json j;
            auto printer_config = Slic3r::GUI::wxGetApp().preset_bundle->printers.get_edited_preset_with_vendor_profile().preset;
            if (printer_config.is_system) {
                j["printer_preset"] = printer_config.name;
            } else {
                j["printer_preset"] = printer_config.config.opt_string("inherits");
            }

            PresetBundle* preset_bundle = wxGetApp().preset_bundle;
            if (preset_bundle) {
                j["gcode_printer_model"] = preset_bundle->printers.get_edited_preset().get_printer_type(preset_bundle);
            }
        } catch (...) {}
    }
}