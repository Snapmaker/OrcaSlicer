#include "Downloader.hpp"
#include "GUI_App.hpp"
#include "NotificationManager.hpp"
#include "format.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "RemoteAccess.hpp"
#include "I18N.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/regex.hpp>
#include <curl/curl.h>

#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace GUI {

namespace {
void open_folder(const std::string& path)
{
	// Code taken from NotificationManager.cpp

	// Execute command to open a file explorer, platform dependent.
	// FIXME: The const_casts aren't needed in wxWidgets 3.1, remove them when we upgrade.

#ifdef _WIN32
	const wxString widepath = from_u8(path);
	const wchar_t* argv[] = { L"explorer", widepath.GetData(), nullptr };
	::wxExecute(const_cast<wchar_t**>(argv), wxEXEC_ASYNC, nullptr);
#elif __APPLE__
	const char* argv[] = { "open", path.data(), nullptr };
	::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr);
#else
	const char* argv[] = { "xdg-open", path.data(), nullptr };

	// Check if we're running in an AppImage container, if so, we need to remove AppImage's env vars,
	// because they may mess up the environment expected by the file manager.
	// Mostly this is about LD_LIBRARY_PATH, but we remove a few more too for good measure.
	if (wxGetEnv("APPIMAGE", nullptr)) {
		// We're running from AppImage
		wxEnvVariableHashMap env_vars;
		wxGetEnvMap(&env_vars);

		env_vars.erase("APPIMAGE");
		env_vars.erase("APPDIR");
		env_vars.erase("LD_LIBRARY_PATH");
		env_vars.erase("LD_PRELOAD");
		env_vars.erase("UNION_PRELOAD");

		wxExecuteEnv exec_env;
		exec_env.env = std::move(env_vars);

		wxString owd;
		if (wxGetEnv("OWD", &owd)) {
			// This is the original work directory from which the AppImage image was run,
			// set it as CWD for the child process:
			exec_env.cwd = std::move(owd);
		}

		::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr, &exec_env);
	}
	else {
		// Looks like we're NOT running from AppImage, we'll make no changes to the environment.
		::wxExecute(const_cast<char**>(argv), wxEXEC_ASYNC, nullptr, nullptr);
	}
#endif
}

std::string filename_from_url(const std::string& url)
{
    std::string result = "";
    CURLU* h = curl_url();
    if (h) {
        if (curl_url_set(h, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
            char* path = nullptr;
            if (curl_url_get(h, CURLUPART_PATH, &path, 0) == CURLUE_OK) {
                std::string p(path);
                curl_free(path);
                size_t slash = p.find_last_of('/');
                if (slash != std::string::npos)
                    result = p.substr(slash + 1);
            }
        }
        curl_url_cleanup(h);
    }
    // fallback: strip query string manually if curl URL parsing failed
    if (result.empty()) {
        size_t slash = url.find_last_of('/');
        if (slash != std::string::npos) {
            result       = url.substr(slash + 1);
            size_t query = result.find('?');
            if (query != std::string::npos)
                result = result.substr(0, query);
        }
    }
    return result;
}

// Ultra: the name the downloaded file gets in the download folder. The link's "&name=" wins,
// then the last segment of the file URL; either way it is a plain file name (no folders, no "..",
// no device names) and, when the link says what type it is, a model type. A name without a model
// extension is left without one: FileGet then takes the type from the server's file name and
// refuses anything but a model.
static std::string resolve_download_filename(const std::string& file_url, const std::string& name_param)
{
    const std::string from_url = untrusted::sanitize_download_filename(untrusted::percent_decode(filename_from_url(file_url)));
    std::string       name     = untrusted::sanitize_download_filename(name_param);
    if (name.empty())
        name = from_url;
    if (name.empty())
        name = "model";
    if (!untrusted::has_model_extension(name) && untrusted::has_model_extension(from_url)) {
        const std::string ext = boost::filesystem::path(from_url).extension().string();
        name = untrusted::sanitize_download_filename(name + ext);
    }
    return name;
}
}

Download::Download(int ID, std::string url, wxEvtHandler* evt_handler, const boost::filesystem::path& dest_folder,
                   const std::string& preferred_filename)
    : m_id(ID)
	, m_filename(preferred_filename.empty() ? filename_from_url(url) : preferred_filename)
	, m_dest_folder(dest_folder)
{
	assert(boost::filesystem::is_directory(dest_folder));
	m_final_path = dest_folder / m_filename;
    m_file_get = std::make_shared<FileGet>(ID, std::move(url), m_filename, evt_handler, dest_folder);
}

void Download::start()
{
	m_state = DownloadState::DownloadOngoing;
	m_file_get->get();
}
void Download::cancel()
{
	m_state = DownloadState::DownloadStopped;
	m_file_get->cancel();
}
void Download::pause()
{
	//assert(m_state == DownloadState::DownloadOngoing);
	// if instead of assert - it can happen that user clicks on pause several times before the pause happens
	if (m_state != DownloadState::DownloadOngoing)
		return;
	m_state = DownloadState::DownloadPaused;
	m_file_get->pause();
}
void Download::resume()
{
	//assert(m_state == DownloadState::DownloadPaused);
	if (m_state != DownloadState::DownloadPaused)
		return;
	m_state = DownloadState::DownloadOngoing;
	m_file_get->resume();
}


Downloader::Downloader()
	: wxEvtHandler()
{
	Bind(EVT_DWNLDR_FILE_COMPLETE, &Downloader::on_complete, this);
	Bind(EVT_DWNLDR_FILE_PROGRESS, &Downloader::on_progress, this);
	Bind(EVT_DWNLDR_FILE_ERROR, &Downloader::on_error, this);
	//Bind(EVT_DWNLDR_FILE_NAME_CHANGE, &Downloader::on_name_change, this); //not work
	Bind(EVT_DWNLDR_FILE_PAUSED, &Downloader::on_paused, this);
	Bind(EVT_DWNLDR_FILE_CANCELED, &Downloader::on_canceled, this);
}

void Downloader::start_download(const std::string& full_url)
{
	assert(m_initialized);

    // Orca: Move to the 3D view
    MainFrame* mainframe = wxGetApp().mainframe;
    Plater* plater = wxGetApp().plater();

    mainframe->Freeze();
    mainframe->select_tab((size_t)MainFrame::TabPosition::tp3DEditor);
    plater->select_view_3D("3D");
    plater->select_view("plate");
    plater->get_current_canvas3D()->zoom_to_bed();
    mainframe->Thaw();

    NotificationManager* ntf_mngr = wxGetApp().notification_manager();
    auto refuse = [ntf_mngr](const std::string& why) {
        BOOST_LOG_TRIVIAL(error) << "Model link refused: " << why;
        ntf_mngr->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::ErrorNotificationLevel,
                                    into_u8(format_wxstr(_L("The model link was not opened: %1%"), from_u8(why))));
    };

    // Ultra: one parser for every "Open in" scheme (UntrustedInput.cpp), then a host check before
    // anything is fetched. A link used to fetch whatever its file= named - file:// paths, LAN
    // addresses, any host - with the OS "open this app?" prompt as the only gate.
    const untrusted::OpenLink link = untrusted::parse_open_link(full_url);
    if (!link.ok) {
        refuse(link.error);
        return;
    }
    const untrusted::DownloadCheck check = untrusted::check_model_download(link.file_url, link.scheme);
    BOOST_LOG_TRIVIAL(info) << "Model link (" << link.scheme << "): host " << check.host << ", " << check.reason;
    if (check.verdict == untrusted::DownloadVerdict::Refuse) {
        refuse(check.reason);
        return;
    }
    if (check.verdict == untrusted::DownloadVerdict::Ask) {
        // Nobody to ask on a hidden / phone-driven instance: an unknown host is refused there.
        if (RemoteAccess::dialog_mode() != RemoteAccess::Mode::Interactive) {
            refuse(check.reason);
            return;
        }
        MessageDialog dlg(mainframe,
                          format_wxstr(_L("A link asks EdgeSlicer to download and open a file from %1%, which is not one of the "
                                          "model sites it knows (Printables, MakerWorld, Thingiverse, Snapmaker, Cults3D).\n\n"
                                          "Only continue if you trust this site. Download and open the file?"),
                                       from_u8(check.host)),
                          _L("Download from an unknown site"), wxYES_NO | wxICON_WARNING);
        dlg.SetButtonLabel(wxID_NO, _L("Don't download"), true);
        dlg.SetButtonLabel(wxID_YES, _L("Download"));
        if (dlg.ShowModal() != wxID_YES) {
            BOOST_LOG_TRIVIAL(info) << "Model link from " << check.host << " declined by the user";
            return;
        }
    }

    size_t id = get_next_id();
    const std::string filename = resolve_download_filename(link.file_url, link.name);

    // MakerWorld (its own scheme, or an "Open in" link whose file lives on MakerWorld) goes through
    // Plater::import_model_id, which opens the 3MF as a project; it splits "&name=" itself.
    if (link.scheme == "bambustudio" || link.scheme == "bambustudioopen" || untrusted::is_makerworld_file_url(link.file_url)) {
        plater->request_model_download(wxString::FromUTF8(link.file_url + "&name=" + filename));
    } else {
        std::string file_url = link.file_url;
        m_downloads.emplace_back(std::make_unique<Download>(id, std::move(file_url), this, m_dest_folder, filename));

        ntf_mngr->push_download_URL_progress_notification(id, m_downloads.back()->get_filename(),
                                                          std::bind(&Downloader::user_action_callback, this, std::placeholders::_1,
                                                                    std::placeholders::_2));
        m_downloads.back()->start();
    }
    BOOST_LOG_TRIVIAL(debug) << "started download";
}

void Downloader::on_progress(wxCommandEvent& event)
{
	size_t id = event.GetInt();
	float percent = (float)std::stoi(into_u8(event.GetString())) / 100.f;
	//BOOST_LOG_TRIVIAL(error) << "progress " << id << ": " << percent;
	NotificationManager* ntf_mngr = wxGetApp().notification_manager();
	BOOST_LOG_TRIVIAL(trace) << "Download "<< id << ": " << percent;
	ntf_mngr->set_download_URL_progress(id, percent);
}
void Downloader::on_error(wxCommandEvent& event)
{
	size_t id = event.GetInt();
    set_download_state(event.GetInt(), DownloadState::DownloadError);
    BOOST_LOG_TRIVIAL(error) << "Download error: " << event.GetString();
	NotificationManager* ntf_mngr = wxGetApp().notification_manager();
    ntf_mngr->set_download_URL_error(id, into_u8(event.GetString()));
	show_error(nullptr, format_wxstr(L"%1%\n%2%", _L("The download has failed") + ":", event.GetString()));
}
void Downloader::on_complete(wxCommandEvent& event)
{
	// TODO: is this always true? :
	// here we open the file itself, notification should get 1.f progress from on progress.
    set_download_state(event.GetInt(), DownloadState::DownloadDone);
	wxArrayString paths;
	paths.Add(event.GetString());
	wxGetApp().mark_web_download(into_path(event.GetString()));
	// from_url: the user clicked "Open in ..." on this specific project.
	wxGetApp().plater()->load_files(paths, /* from_url */ true);
}
bool Downloader::user_action_callback(DownloaderUserAction action, int id)
{
	for (size_t i = 0; i < m_downloads.size(); ++i) {
		if (m_downloads[i]->get_id() == id) {
			switch (action) {
			case DownloadUserCanceled:
				m_downloads[i]->cancel();
				return true;
			case DownloadUserPaused:
				m_downloads[i]->pause();
				return true;
			case DownloadUserContinued:
				m_downloads[i]->resume();
				return true;
			case DownloadUserOpenedFolder:
				open_folder(m_downloads[i]->get_dest_folder());
				return true;
			default:
				return false;
			}
		}
	}
	return false;
}

void Downloader::on_name_change(wxCommandEvent& event)
{
   
}

void Downloader::on_paused(wxCommandEvent& event)
{
	size_t id = event.GetInt();
	NotificationManager* ntf_mngr = wxGetApp().notification_manager();
	ntf_mngr->set_download_URL_paused(id);
}

void Downloader::on_canceled(wxCommandEvent& event)
{
	size_t id = event.GetInt();
	NotificationManager* ntf_mngr = wxGetApp().notification_manager();
	ntf_mngr->set_download_URL_canceled(id);
}

void Downloader::set_download_state(int id, DownloadState state)
{
    for (size_t i = 0; i < m_downloads.size(); ++i) {
        if (m_downloads[i]->get_id() == id) {
            m_downloads[i]->set_state(state);
            return;
        }
    }
}

}
}
