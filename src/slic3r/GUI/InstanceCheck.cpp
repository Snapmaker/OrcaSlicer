#include "GUI_App.hpp"
#include "InstanceCheck.hpp"
#include "Plater.hpp"
#include <boost/regex.hpp>

#ifdef _WIN32
  #include "MainFrame.hpp"
#endif

#include "libslic3r/Utils.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/Config.hpp"
#include "slic3r/Utils/InstanceRouting.hpp"

#include "boost/nowide/convert.hpp"
#include <boost/log/trivial.hpp>
#include <boost/filesystem/operations.hpp>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <fcntl.h>
#include <errno.h>
#include <optional>
#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <strsafe.h>
#endif //WIN32

#if __linux__
#include <dbus/dbus.h> /* Pull in all of D-Bus headers. */
#endif //__linux__

namespace Slic3r {

#ifdef __APPLE__
	bool unlock_lockfile(const std::string& name, const std::string& path)
	{
		std::string dest_dir = path + name;
		//BOOST_LOG_TRIVIAL(debug) << "full lock path: " << dest_dir;
		struct      flock fl;
		int         fdlock;
		fl.l_type = F_UNLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start = 0;
		fl.l_len = 1;
		if ((fdlock = open(dest_dir.c_str(), O_WRONLY | O_CREAT, 0666)) == -1)
			return false;

		if (fcntl(fdlock, F_SETLK, &fl) == -1)
			return false;

		return true;
	}
#endif //__APPLE__

namespace instance_check_internal
{
	static bool        s_created_lockfile = false;

	struct CommandLineAnalysis
	{
		std::optional<bool>	should_send;
		std::string    		cl_string;
	};
	static CommandLineAnalysis process_command_line(int argc, char** argv)
	{
		CommandLineAnalysis ret;
		//if (argc < 2)
		//	return ret;
		std::vector<std::string> arguments { argv[0] };
        for (int i = 1; i < argc; ++i) {
			const std::string token = argv[i];
			// Processing of boolean command line arguments shall match DynamicConfig::read_cli().
			if (token == "--single-instance")
				ret.should_send = true;
			else if (token == "--no-single-instance")
				ret.should_send = false;
			else
				arguments.emplace_back(token);
		} 
		ret.cl_string = escape_strings_cstyle(arguments);
		BOOST_LOG_TRIVIAL(debug) << "single instance: " << 
            (ret.should_send.has_value() ? (*ret.should_send ? "true" : "false") : "undefined") <<
			". other params: " << ret.cl_string;
		return ret;
	}

	

#ifdef _WIN32

	static HWND l_bambu_studio_hwnd;
	static BOOL CALLBACK EnumWindowsProc(_In_ HWND   hwnd, _In_ LPARAM lParam)
	{
		//checks for other instances of prusaslicer, if found brings it to front and return false to stop enumeration and quit this instance
		//search is done by classname(wxWindowNR is wxwidgets thing, so probably not unique) and name in window upper panel
		//other option would be do a mutex and check for its existence
		//BOOST_LOG_TRIVIAL(error) << "ewp: version: " << l_version_wstring;
		TCHAR 		 wndText[1000];
		TCHAR 		 className[1000];
		int          err;
		err = GetClassName(hwnd, className, 1000);
		if (err == 0)
			return true;
		err = GetWindowText(hwnd, wndText, 1000);
		if (err == 0)
			return true;
		std::wstring classNameString(className);
		std::wstring wndTextString(wndText);
		// The main window title ends in " - <SLIC3R_APP_NAME>" ("EdgeSlicer" since the rebrand); a hard-coded
		// old literal never matched again, so a second launch handed off but never raised the window.
		// Match the current brand and every name the title carried before it (SLIC3R_LEGACY_APP_KEYS, plus
		// the spaced spelling the fork's title used), so an older instance is still found.
		static const std::vector<std::wstring> app_names = [] {
			std::vector<std::wstring> names{ boost::nowide::widen(SLIC3R_APP_NAME) };
			for (const std::string& legacy : std::vector<std::string>(SLIC3R_LEGACY_APP_KEYS))
				names.push_back(boost::nowide::widen(legacy));
			names.push_back(L"Snapmaker Orca");
			return names;
		}();
		const bool title_matches = std::any_of(app_names.begin(), app_names.end(),
			[&wndTextString](const std::wstring& name) { return wndTextString.find(name) != std::wstring::npos; });
		if (title_matches && classNameString == L"wxWindowNR") {
			//check if other instances has same instance hash
			//if not it is not same version(binary) as this version 
			HANDLE   handle = GetProp(hwnd, L"Instance_Hash_Minor");
			uint64_t other_instance_hash = PtrToUint(handle);
			uint64_t other_instance_hash_major;
			uint64_t my_instance_hash = GUI::wxGetApp().get_instance_hash_int();
			handle = GetProp(hwnd, L"Instance_Hash_Major");
			other_instance_hash_major = PtrToUint(handle);
			other_instance_hash_major = other_instance_hash_major << 32;
			other_instance_hash += other_instance_hash_major;
			// Only a window somebody can see receives the files. A hidden hub-managed slicer of the same
			// executable has a main window too (and answers to the same hash); handing it the files
			// would load them into a window nobody can look at.
			const bool visible = IsWindowVisible(hwnd) != 0;
			if (InstanceRouting::is_hand_off_target(my_instance_hash, other_instance_hash, visible))
			{
				BOOST_LOG_TRIVIAL(info) << "Instance check: found the visible instance of this executable (window " << (void*)hwnd << ")";
				l_bambu_studio_hwnd = hwnd;
				ShowWindow(hwnd, SW_SHOWMAXIMIZED);
				SetForegroundWindow(hwnd);
				return false;
			}
			if (my_instance_hash == other_instance_hash)
				BOOST_LOG_TRIVIAL(info) << "Instance check: skipping a hidden instance of this executable (window " << (void*)hwnd << "), it does not receive single-instance files";
			else
				BOOST_LOG_TRIVIAL(debug) << "win enum - found wrong instance";
		}
		return true;
	}
	static bool send_message(const std::string& message, const std::string &version)
	{
		if (!EnumWindows(EnumWindowsProc, 0)) {
			std::wstring wstr = boost::nowide::widen(message);
			std::unique_ptr<LPWSTR> command_line_args = std::make_unique<LPWSTR>(const_cast<LPWSTR>(wstr.c_str()));
			/*LPWSTR command_line_args = new wchar_t[wstr.size() + 1];
			copy(wstr.begin(), wstr.end(), command_line_args);
			command_line_args[wstr.size()] = 0;*/

			//Create a COPYDATASTRUCT to send the information
			//cbData represents the size of the information we want to send.
			//lpData represents the information we want to send.
			//dwData is an ID defined by us(this is a type of ID different than WM_COPYDATA).
			COPYDATASTRUCT data_to_send = { 0 };
			data_to_send.dwData = 1;
			data_to_send.cbData = sizeof(TCHAR) * (wcslen(*command_line_args.get()) + 1);
			data_to_send.lpData = *command_line_args.get();
			SendMessage(l_bambu_studio_hwnd, WM_COPYDATA, 0, (LPARAM)&data_to_send);
			BOOST_LOG_TRIVIAL(info) << "Instance check: handed the launch arguments to the running instance: " << message;
			return true;  
		}
		BOOST_LOG_TRIVIAL(warning) << "Instance check: no visible instance of this executable (hash " << version << ") to hand the arguments to: " << message;
	    return false;
	}

#else 

	static bool get_lock(const std::string& name, const std::string& path)
	{
		std::string dest_dir = path + name;
		BOOST_LOG_TRIVIAL(debug) <<"full lock path: "<< dest_dir;
		struct      flock fl;
		int         fdlock;
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start = 0;
		fl.l_len = 1;

        if (! boost::filesystem::is_directory(path)) {
            BOOST_LOG_TRIVIAL(debug) << "get_lock(): datadir does not exist yet, creating...";
            if (! boost::filesystem::create_directories(path))
                BOOST_LOG_TRIVIAL(debug) << "get_lock(): unable to create datadir !!!";
        }

		if ((fdlock = open(dest_dir.c_str(), O_WRONLY | O_CREAT, 0666)) == -1) {
			BOOST_LOG_TRIVIAL(debug) << "Not creating lockfile.";
			return true;
		}

		if (fcntl(fdlock, F_SETLK, &fl) == -1) {
			BOOST_LOG_TRIVIAL(debug) << "Not creating lockfile.";
			return true;
		}

		BOOST_LOG_TRIVIAL(debug) << "Creating lockfile.";
		s_created_lockfile = true;
		return false;
	}

	// Whether another process holds the lock, without taking it. A hidden instance uses this: it may
	// hand its files to a visible instance, but must never become the one that receives them.
	static bool lock_is_held_by_other(const std::string& name, const std::string& path)
	{
		std::string dest_dir = path + name;
		struct      flock fl;
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start = 0;
		fl.l_len = 1;
		fl.l_pid = 0;
		int fdlock = open(dest_dir.c_str(), O_RDONLY);
		if (fdlock == -1)
			return false;
		const bool held = fcntl(fdlock, F_GETLK, &fl) != -1 && fl.l_type != F_UNLCK;
		close(fdlock);
		return held;
	}

	// Deletes lockfile if it was created by this instance
	// The Lockfile is created only on Linux a OSX. On Win, its handled by named mutex.
	// The lockfile is deleted by instance it created it.
	// On OSX message is passed to other instances to create a new lockfile after deletition.
	static void delete_lockfile()
	{
		//BOOST_LOG_TRIVIAL(debug) << "shuting down with lockfile: " << l_created_lockfile;
		if (s_created_lockfile)
		{
			std::string path = data_dir() + "/cache/" + GUI::wxGetApp().get_instance_hash_string() + ".lock";
			if( remove( path.c_str() ) != 0 )
	   			BOOST_LOG_TRIVIAL(error) << "Failed to delete lockfile " << path;
	  		//else
	    	//	BOOST_LOG_TRIVIAL(error) << "success delete lockfile " << path;
#ifdef __APPLE__
	   		send_message_mac_closing(GUI::wxGetApp().get_instance_hash_string(),GUI::wxGetApp().get_instance_hash_string());
#endif	    
		}
	}

#endif //WIN32
#if defined(__APPLE__)

	static bool send_message(const std::string &message_text, const std::string &version)
	{
		//std::string v(version);
		//std::replace(v.begin(), v.end(), '.', '-');
		//if (!instance_check_internal::get_lock(v)) 
		{
			send_message_mac(message_text, version);
			return true;
		}
		return false;
	}

#elif defined(__linux__)

	static bool  send_message(const std::string &message_text, const std::string &version)
	{
		/*std::string v(version);
		std::replace(v.begin(), v.end(), '.', '-');
		if (!instance_check_internal::get_lock(v))*/
		/*auto checker = new wxSingleInstanceChecker;
		if ( !checker->IsAnotherRunning() ) */
		{
			DBusMessage* msg;
            // DBusMessageIter args;
			DBusConnection* conn;
			DBusError 		err;
			dbus_uint32_t 	serial = 0;
			const char* sigval = message_text.c_str();
			//std::string		interface_name = "com.prusa3d.prusaslicer.InstanceCheck";
			std::string		interface_name = "io.github.acerage.EdgeSlicer.InstanceCheck.Object" + version;
			std::string   	method_name = "AnotherInstance";
			//std::string		object_name = "/com/prusa3d/prusaslicer/InstanceCheck";
			std::string		object_name = "/io/github/acerage/EdgeSlicer/InstanceCheck/Object" + version;


			// initialise the error value
			dbus_error_init(&err);

			// connect to bus, and check for errors (use SESSION bus everywhere!)
			conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
			if (dbus_error_is_set(&err)) {
				BOOST_LOG_TRIVIAL(error) << "DBus Connection Error. Message to another instance wont be send.";
				BOOST_LOG_TRIVIAL(error) << "DBus Connection Error: " << err.message;
				dbus_error_free(&err);
				return true;
			}
			if (NULL == conn) {
				BOOST_LOG_TRIVIAL(error) << "DBus Connection is NULL. Message to another instance wont be send.";
				return true;
			}

			//some sources do request interface ownership before constructing msg but i think its wrong.

			//create new method call message
			msg = dbus_message_new_method_call(interface_name.c_str(), object_name.c_str(), interface_name.c_str(), method_name.c_str());
			if (NULL == msg) {
				BOOST_LOG_TRIVIAL(error) << "DBus Message is NULL. Message to another instance wont be send.";
				dbus_connection_unref(conn);
				return true;
			}
			//the AnotherInstance method is not sending reply.
			dbus_message_set_no_reply(msg, TRUE);

			//append arguments to message
			if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &sigval, DBUS_TYPE_INVALID)) {
				BOOST_LOG_TRIVIAL(error) << "Ran out of memory while constructing args for DBus message. Message to another instance wont be send.";
				dbus_message_unref(msg);
				dbus_connection_unref(conn);
				return true;
			}

			// send the message and flush the connection
			if (!dbus_connection_send(conn, msg, &serial)) {
				BOOST_LOG_TRIVIAL(error) << "Ran out of memory while sending DBus message.";
				dbus_message_unref(msg);
				dbus_connection_unref(conn);
				return true;
			}
			dbus_connection_flush(conn);

			BOOST_LOG_TRIVIAL(trace) << "DBus message sent.";

			// free the message and close the connection
			dbus_message_unref(msg);                                                                                                                                                                                    
			dbus_connection_unref(conn);
			return true;
		}
		return false;
	}

#endif //__APPLE__/__linux__
} //namespace instance_check_internal

#ifdef _WIN32
// The text an instance's identity is hashed from. It is the running module's own path, resolved
// through junctions and symlinks (the EdgeSlicerBuilds "current" folder is a junction) and normalised
// for case and separators: the same install reached through two spellings - the path Blender was
// given, the one Explorer or the hub launched it by - must be one instance, or a hand-off never
// finds it.
static std::string windows_instance_key(const char* argv0)
{
	std::string full;
	static wchar_t module_path[32768];
	const DWORD n = GetModuleFileNameW(nullptr, module_path, (DWORD)(sizeof(module_path) / sizeof(module_path[0])));
	if (n > 0 && n < sizeof(module_path) / sizeof(module_path[0]))
		full = boost::nowide::narrow(module_path);
	else
		full = boost::filesystem::system_complete(argv0).string();
	const std::wstring wide = boost::nowide::widen(full);
	HANDLE h = CreateFileW(wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (h != INVALID_HANDLE_VALUE) {
		static wchar_t final_path[32768];
		const DWORD m = GetFinalPathNameByHandleW(h, final_path, (DWORD)(sizeof(final_path) / sizeof(final_path[0])), FILE_NAME_NORMALIZED);
		CloseHandle(h);
		if (m > 0 && m < sizeof(final_path) / sizeof(final_path[0]))
			full = boost::nowide::narrow(final_path);
	}
	return InstanceRouting::normalize_exe_path_key(full);
}
#endif // _WIN32

bool instance_check(int argc, char** argv, bool app_config_single_instance, bool hidden_start)
{
	std::size_t hashed_path;
#ifdef _WIN32
	hashed_path = std::hash<std::string>{}(windows_instance_key(argv[0]));
#else
	boost::system::error_code ec;
#ifdef __linux__
	// If executed by an AppImage, start the AppImage, not the main process.
	// see https://docs.appimage.org/packaging-guide/environment-variables.html#id2
	const char *appimage_env = std::getenv("APPIMAGE");
	bool appimage_env_valid = false;
	if (appimage_env) {
		try {
			auto appimage_path = boost::filesystem::canonical(boost::filesystem::path(appimage_env));
			if (boost::filesystem::exists(appimage_path)) {
				hashed_path = std::hash<std::string>{}(appimage_path.string());
				appimage_env_valid = true;
			}
		} catch (std::exception &) {			
		}
		if (! appimage_env_valid)
			BOOST_LOG_TRIVIAL(error) << "APPIMAGE environment variable was set, but it does not point to a valid file: " << appimage_env;
	}
	if (! appimage_env_valid)
#endif // __linux__
		hashed_path = std::hash<std::string>{}(boost::filesystem::canonical(boost::filesystem::system_complete(argv[0]), ec).string());
	if (ec.value() > 0) { // canonical was not able to find the executable (can happen with appimage on some systems. Does it fail on Fuse file systems?)
		ec.clear();
		// Compose path with boost canonical of folder and filename
		hashed_path = std::hash<std::string>{}(boost::filesystem::canonical(boost::filesystem::system_complete(argv[0]).parent_path(), ec).string() + "/" + boost::filesystem::system_complete(argv[0]).filename().string());
		if (ec.value() > 0) {
			// Still not valid, process without canonical
			hashed_path = std::hash<std::string>{}(boost::filesystem::system_complete(argv[0]).string());
		}
	}
#endif // _WIN32

	std::string lock_name 	= std::to_string(hashed_path);
	GUI::wxGetApp().set_instance_hash(hashed_path);
	BOOST_LOG_TRIVIAL(debug) <<"full path: "<< lock_name;
	instance_check_internal::CommandLineAnalysis cla = instance_check_internal::process_command_line(argc, argv);
	if (! cla.should_send.has_value())
		cla.should_send = app_config_single_instance;
	BOOST_LOG_TRIVIAL(info) << "Instance check: instance " << lock_name << (hidden_start ? ", starting hidden (does not claim the single-instance lock)" : "")
		<< ", hand-off " << (*cla.should_send ? "requested" : "not requested") << ", arguments " << cla.cl_string;
	const bool claims_lock = InstanceRouting::claims_instance_lock(hidden_start);
#ifdef _WIN32
	// The named lock is taken here. A hidden instance gives it back below, so it never stands in the
	// way of a visible instance.
	GUI::wxGetApp().init_single_instance_checker(lock_name + ".lock", data_dir() + "\cache\\");
	const bool other_holds_lock = GUI::wxGetApp().single_instance_checker()->IsAnotherRunning();
#else // mac & linx
	// get_lock() creates the lockfile, so only a visible instance may call it.
	const bool other_holds_lock = claims_lock ? instance_check_internal::get_lock(lock_name + ".lock", data_dir() + "/cache/")
	                                          : instance_check_internal::lock_is_held_by_other(lock_name + ".lock", data_dir() + "/cache/");
#endif
	if (*cla.should_send && other_holds_lock) {
		const bool delivered = instance_check_internal::send_message(cla.cl_string, lock_name);
		if (InstanceRouting::should_hand_off(true, true, delivered)) {
			BOOST_LOG_TRIVIAL(info) << "Instance check: another instance received the arguments. This instance will terminate. Lock file of current running instance is located at " << data_dir() <<
#ifdef _WIN32
				"\cache\\"
#else // mac & linx
				"/cache/"
#endif
				<< lock_name << ".lock";
			return true;
		}
		// The lock is held (by a hidden slicer, one that is still starting, or one that has hung) but
		// no visible instance took the arguments. Exiting here would drop them; start normally instead.
		BOOST_LOG_TRIVIAL(warning) << "Instance check: the lock is held but no visible instance received the arguments; starting this instance with them instead.";
	}
#ifdef _WIN32
	if (!claims_lock)
		GUI::wxGetApp().release_single_instance_checker();
#endif
	BOOST_LOG_TRIVIAL(info) << "Instance check: no other instance took over this launch; starting normally.";

	return false;
}


namespace GUI {

wxDEFINE_EVENT(EVT_LOAD_MODEL_OTHER_INSTANCE, LoadFromOtherInstanceEvent);
wxDEFINE_EVENT(EVT_START_DOWNLOAD_OTHER_INSTANCE, StartDownloadOtherInstanceEvent);
wxDEFINE_EVENT(EVT_INSTANCE_GO_TO_FRONT, InstanceGoToFrontEvent);

void OtherInstanceMessageHandler::init(wxEvtHandler* callback_evt_handler)
{
	assert(!m_initialized);
	assert(m_callback_evt_handler == nullptr);
	if (m_initialized) 
		return;

	m_initialized = true;
	m_callback_evt_handler = callback_evt_handler;

#if defined(__APPLE__)
	this->register_for_messages(wxGetApp().get_instance_hash_string());
#endif //__APPLE__

#ifdef BACKGROUND_MESSAGE_LISTENER
	m_thread = boost::thread((boost::bind(&OtherInstanceMessageHandler::listen, this)));
#endif //BACKGROUND_MESSAGE_LISTENER
}
void OtherInstanceMessageHandler::shutdown(MainFrame* main_frame)
{
	BOOST_LOG_TRIVIAL(debug) << "message handler shutdown().";
#ifndef _WIN32
	instance_check_internal::delete_lockfile();
#endif //!_WIN32
	assert(m_initialized);
	if (m_initialized) {
#ifdef _WIN32
		HWND hwnd = main_frame->GetHandle();
		RemoveProp(hwnd, L"Instance_Hash_Minor");
		RemoveProp(hwnd, L"Instance_Hash_Major");
#endif //_WIN32
#if __APPLE__
		//delete macos implementation
		this->unregister_for_messages();
#endif //__APPLE__
#ifdef BACKGROUND_MESSAGE_LISTENER
		if (m_thread.joinable()) {
			// Stop the worker thread, if running.
			{
				// Notify the worker thread to cancel wait on detection polling.
				std::lock_guard<std::mutex> lck(m_thread_stop_mutex);
				m_stop = true;
			}
			m_thread_stop_condition.notify_all();
			// Wait for the worker thread to stop.
			m_thread.join();
			m_stop = false;
		}
#endif //BACKGROUND_MESSAGE_LISTENER
		m_callback_evt_handler = nullptr;
		m_initialized = false;
	}
}

#ifdef _WIN32 
void OtherInstanceMessageHandler::init_windows_properties(MainFrame* main_frame, size_t instance_hash)
{
	size_t       minor_hash = instance_hash & 0xFFFFFFFF;
	size_t       major_hash = (instance_hash & 0xFFFFFFFF00000000) >> 32;
	HWND         hwnd = main_frame->GetHandle();
	HANDLE       handle_minor = UIntToPtr(minor_hash);
	HANDLE       handle_major = UIntToPtr(major_hash);
	SetProp(hwnd, L"Instance_Hash_Minor", handle_minor);
	SetProp(hwnd, L"Instance_Hash_Major", handle_major);
	//BOOST_LOG_TRIVIAL(debug) << "window properties initialized " << instance_hash << " (" << minor_hash << " & "<< major_hash;
}

#if 0

void OtherInstanceMessageHandler::print_window_info(HWND hwnd)
{
	std::wstring instance_hash = boost::nowide::widen(wxGetApp().get_instance_hash_string());
	TCHAR 		 wndText[1000];
	TCHAR 		 className[1000];
	GetClassName(hwnd, className, 1000);
	GetWindowText(hwnd, wndText, 1000);
	std::wstring classNameString(className);
	std::wstring wndTextString(wndText);
	HANDLE       handle = GetProp(hwnd, L"Instance_Hash_Minor");
	size_t       result = PtrToUint(handle);
	handle = GetProp(hwnd, L"Instance_Hash_Major");
	size_t       r2 = PtrToUint(handle);
	r2 = (r2 << 32);
	result += r2;
	BOOST_LOG_TRIVIAL(info) << "window info: " << result;
}
#endif //0
#endif  //WIN32
namespace MessageHandlerInternal
{
   // returns ::path to possible model or empty ::path if input string is not existing path
	static boost::filesystem::path get_path(const std::string& possible_path)
	{
		BOOST_LOG_TRIVIAL(debug) << "message part:" << possible_path;

		if (possible_path.empty() || possible_path.size() < 3) {
			BOOST_LOG_TRIVIAL(debug) << "empty";
			return boost::filesystem::path();
		}
		if (boost::filesystem::exists(possible_path)) {
			BOOST_LOG_TRIVIAL(debug) << "is path";
			return boost::filesystem::path(possible_path);
		} else if (possible_path[0] == '\"') {
			if(boost::filesystem::exists(possible_path.substr(1, possible_path.size() - 2))) {
				BOOST_LOG_TRIVIAL(debug) << "is path in quotes";
				return boost::filesystem::path(possible_path.substr(1, possible_path.size() - 2));
			}
		}
		BOOST_LOG_TRIVIAL(debug) << "is NOT path";
		return boost::filesystem::path();
	}
} //namespace MessageHandlerInternal

void OtherInstanceMessageHandler::handle_message(const std::string& message) 
{
	BOOST_LOG_TRIVIAL(info) << "message from other instance: " << message;

	std::vector<std::string> args;
	bool parsed = unescape_strings_cstyle(message, args);
	assert(parsed);
	if (! parsed) {
		BOOST_LOG_TRIVIAL(error) << "message from other instance is incorrectly formatted: " << message;
		return;
	}

	std::vector<boost::filesystem::path> paths;
	std::vector<std::string> downloads;
	boost::regex re(R"(^(edgeslicer|ultraone|snapmaker-orca|Snapmaker_Orca|prusaslicer|cura|bambustudio):\/\/open[\/]?\?file=)", boost::regbase::icase);
	boost::regex re2(R"(^(bambustudioopen):\/\/)", boost::regex::icase);
	boost::smatch results;

	// Skip the first argument, it is the path to the slicer executable.
	auto it = args.begin();
	for (++ it; it != args.end(); ++ it) {
		boost::filesystem::path p = MessageHandlerInternal::get_path(*it);
		if (! p.string().empty())
			paths.emplace_back(p);
		else if (boost::regex_search(*it, results, re) || boost::regex_search(*it, results, re2))
			downloads.emplace_back(*it);
	}
	BOOST_LOG_TRIVIAL(info) << "Instance check: the message from the other instance carries " << paths.size() << " file(s) and " << downloads.size() << " download link(s)";
	if (args.size() > 1 && paths.empty() && downloads.empty())
		BOOST_LOG_TRIVIAL(warning) << "Instance check: none of the arguments from the other instance is an existing file: " << message;
	if (! paths.empty()) {
		//wxEvtHandler* evt_handler = wxGetApp().plater(); //assert here?
		//if (evt_handler) {
			wxPostEvent(m_callback_evt_handler, LoadFromOtherInstanceEvent(GUI::EVT_LOAD_MODEL_OTHER_INSTANCE, std::vector<boost::filesystem::path>(std::move(paths))));
		//}
	}
	if (!downloads.empty())
	{
		wxPostEvent(m_callback_evt_handler, StartDownloadOtherInstanceEvent(GUI::EVT_START_DOWNLOAD_OTHER_INSTANCE, std::vector<std::string>(std::move(downloads))));
	}
}

#ifdef __APPLE__
void OtherInstanceMessageHandler::handle_message_other_closed() 
{
	instance_check_internal::get_lock(wxGetApp().get_instance_hash_string() + ".lock", data_dir() + "/cache/");
}
#endif //__APPLE__

#ifdef BACKGROUND_MESSAGE_LISTENER

namespace MessageHandlerDBusInternal
{
	//reply to introspect makes our DBus object visible for other programs like D-Feet
	static void respond_to_introspect(DBusConnection *connection, DBusMessage *request) 
	{
    	DBusMessage *reply;
	    const char  *introspection_data =
	        " <!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" "
	        "\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
	        " <!-- dbus-sharp 0.8.1 -->"
	        " <node>"
	        "   <interface name=\"org.freedesktop.DBus.Introspectable\">"
	        "     <method name=\"Introspect\">"
	        "       <arg name=\"data\" direction=\"out\" type=\"s\" />"
	        "     </method>"
	        "   </interface>"
	        "   <interface name=\"io.github.acerage.EdgeSlicer.InstanceCheck\">"
	        "     <method name=\"AnotherInstance\">"
	        "       <arg name=\"data\" direction=\"in\" type=\"s\" />"
	        "     </method>"
	        "   </interface>"
	        " </node>";
	     
	    reply = dbus_message_new_method_return(request);
	    dbus_message_append_args(reply, DBUS_TYPE_STRING, &introspection_data, DBUS_TYPE_INVALID);
	    dbus_connection_send(connection, reply, NULL);
	    dbus_message_unref(reply);
	}
	//method AnotherInstance receives message from another Snapmaker_Orca instance 
	static void handle_method_another_instance(DBusConnection *connection, DBusMessage *request)
	{
	    DBusError     err;
	    char*         text = nullptr;
		wxEvtHandler* evt_handler;

	    dbus_error_init(&err);
	    dbus_message_get_args(request, &err, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
	    if (dbus_error_is_set(&err)) {
	    	BOOST_LOG_TRIVIAL(trace) << "Dbus method AnotherInstance received with wrong arguments.";
	    	dbus_error_free(&err);
	        return;
	    }
	    wxGetApp().other_instance_message_handler()->handle_message(text);

		evt_handler = wxGetApp().plater();
		if (evt_handler) {
			wxPostEvent(evt_handler, InstanceGoToFrontEvent(EVT_INSTANCE_GO_TO_FRONT));
		}
	}
	//every dbus message received comes here
	static DBusHandlerResult handle_dbus_object_message(DBusConnection *connection, DBusMessage *message, void *user_data)
	{
		const char* interface_name = dbus_message_get_interface(message);
	    const char* member_name    = dbus_message_get_member(message);
	    std::string our_interface  = "io.github.acerage.EdgeSlicer.InstanceCheck.Object" + wxGetApp().get_instance_hash_string();
	    BOOST_LOG_TRIVIAL(trace) << "DBus message received: interface: " << interface_name << ", member: " << member_name;
	    if (0 == strcmp("org.freedesktop.DBus.Introspectable", interface_name) && 0 == strcmp("Introspect", member_name)) {		
	        respond_to_introspect(connection, message);
	        return DBUS_HANDLER_RESULT_HANDLED;
	    } else if (0 == strcmp(our_interface.c_str(), interface_name) && 0 == strcmp("AnotherInstance", member_name)) {
	        handle_method_another_instance(connection, message);
	        return DBUS_HANDLER_RESULT_HANDLED;
	    } 
	    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
} //namespace MessageHandlerDBusInternal

void OtherInstanceMessageHandler::listen()
{
    DBusConnection* 	 conn;
    DBusError 			 err;
    int 				 name_req_val;
    DBusObjectPathVTable vtable;
    std::string 		 instance_hash  = wxGetApp().get_instance_hash_string();
	std::string			 interface_name = "io.github.acerage.EdgeSlicer.InstanceCheck.Object" + instance_hash;
    std::string			 object_name 	= "/io/github/acerage/EdgeSlicer/InstanceCheck/Object" + instance_hash;

    //BOOST_LOG_TRIVIAL(debug) << "init dbus listen " << interface_name << " " << object_name;
    dbus_error_init(&err);

    // connect to the bus and check for errors (use SESSION bus everywhere!)
    conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (dbus_error_is_set(&err)) { 
	    BOOST_LOG_TRIVIAL(error) << "DBus Connection Error: "<< err.message;
	    BOOST_LOG_TRIVIAL(error) << "Dbus Messages listening terminating.";
        dbus_error_free(&err); 
        return;
    }
    if (NULL == conn) { 
		BOOST_LOG_TRIVIAL(error) << "DBus Connection is NULL. Dbus Messages listening terminating.";
        return;
    }

	// request our name on the bus and check for errors
	name_req_val = dbus_bus_request_name(conn, interface_name.c_str(), DBUS_NAME_FLAG_REPLACE_EXISTING , &err);
	if (dbus_error_is_set(&err)) {
	    BOOST_LOG_TRIVIAL(error) << "DBus Request name Error: "<< err.message; 
	    BOOST_LOG_TRIVIAL(error) << "Dbus Messages listening terminating.";
	    dbus_error_free(&err); 
	    dbus_connection_unref(conn);
	    return;
	}
	if (DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER != name_req_val) {
		BOOST_LOG_TRIVIAL(error) << "Not primary owner of DBus name - probably another Snapmaker_Orca instance is running.";
	    BOOST_LOG_TRIVIAL(error) << "Dbus Messages listening terminating.";
	    dbus_connection_unref(conn);
	    return;
	}

	// Set callbacks. Unregister function should not be nessary.
	vtable.message_function = MessageHandlerDBusInternal::handle_dbus_object_message;
    vtable.unregister_function = NULL;

    // register new object - this is our access to DBus
    dbus_connection_try_register_object_path(conn, object_name.c_str(), &vtable, NULL, &err);
   	if ( dbus_error_is_set(&err) ) {
   		BOOST_LOG_TRIVIAL(error) << "DBus Register object Error: "<< err.message; 
	    BOOST_LOG_TRIVIAL(error) << "Dbus Messages listening terminating.";
	    dbus_connection_unref(conn);
		dbus_error_free(&err);
		return;
	}

	BOOST_LOG_TRIVIAL(trace) << "Dbus object "<< object_name <<" registered. Starting listening for messages.";

	for (;;) {
		// Wait for 1 second 
		// Cancellable.
		{
			std::unique_lock<std::mutex> lck(m_thread_stop_mutex);
			m_thread_stop_condition.wait_for(lck, std::chrono::seconds(1), [this] { return m_stop; });
		}
		if (m_stop)
			// Stop the worker thread.

			break;
		//dispatch should do all the work with incoming messages
		//second parameter is blocking time that funciton waits for new messages
		//that is handled here with our own event loop above
		dbus_connection_read_write_dispatch(conn, 0);
     }
     
   	 dbus_connection_unref(conn);
}
#endif //BACKGROUND_MESSAGE_LISTENER
} // namespace GUI
} // namespace Slic3r
