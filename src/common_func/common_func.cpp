#include "common_func.hpp"
#include <boost/asio/ip/host_name.hpp>

#ifdef _WIN32
#include <windows.h>
#include <Shlobj.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#elif __APPLE__
#include <stdlib.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/network/IONetworkInterface.h>
#include <IOKit/network/IONetworkController.h>
#include <AvailabilityMacros.h>
#endif

#include <cstring>
#include <cwchar>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <csignal>
#include <cerrno>
#include <unistd.h>
#endif
#include <filesystem>
#include <fstream>
#include <vector>
#include <nlohmann/json.hpp>

#ifdef __linux__
static std::string get_linux_config_dir()
{
    const char* xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && xdg_config[0] != '\0')
        return std::string(xdg_config) + "/" SLIC3R_APP_KEY;
    const char* home = getenv("HOME");
    if (home && home[0] != '\0')
        return std::string(home) + "/.config/" SLIC3R_APP_KEY;
    return std::string();
}
#endif

// The --datadir given on the command line, UTF-8; empty when there was none. Set by
// common::set_datadir_from_command_line() before initSentry() runs.
static std::string g_datadir_override;

#ifdef _WIN32
static std::string wide_to_utf8(const wchar_t* w)
{
    if (w == nullptr || *w == L'\0')
        return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1)
        return std::string();
    std::string out(size_t(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], len, nullptr, nullptr);
    return out;
}
#endif

static std::filesystem::path path_from_utf8(const std::string& s)
{
#ifdef _WIN32
    return std::filesystem::u8path(s);
#else
    return std::filesystem::path(s);
#endif
}

// The directory holding the running executable ("" if unknown).
static std::filesystem::path executable_dir()
{
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD  n = ::GetModuleFileNameW(nullptr, &buf[0], DWORD(buf.size()));
    if (n == 0 || n >= buf.size())
        return {};
    buf.resize(n);
    return std::filesystem::path(buf).parent_path();
#elif __APPLE__
    char     exe[PATH_MAX] = {0};
    uint32_t size          = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0)
        return {};
    return std::filesystem::path(exe).parent_path();
#else
    std::error_code ec;
    auto            exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : exe.parent_path();
#endif
}

// The data directory AppConfig will use, worked out the same way GUI_App::init_app_config()
// and CLI::setup() do it, but before either exists: initSentry() is the first thing main()
// does, so there is no AppConfig, no wxWidgets and no parsed command line yet.
//   1. --datadir on the command line
//   2. a "data_dir" folder next to the application (portable installs)
//   3. the per-user default: %APPDATA%\EdgeSlicer, ~/Library/Application Support/EdgeSlicer,
//      $XDG_CONFIG_HOME/EdgeSlicer
static std::filesystem::path app_data_dir()
{
    if (!g_datadir_override.empty())
        return path_from_utf8(g_datadir_override);

    std::error_code ec;
    std::filesystem::path app_folder = executable_dir();
#ifdef __APPLE__
    // <folder>/EdgeSlicer.app/Contents/MacOS/EdgeSlicer: the portable folder sits beside the bundle.
    app_folder = app_folder.parent_path().parent_path().parent_path();
#endif
    if (!app_folder.empty() && std::filesystem::is_directory(app_folder / "data_dir", ec))
        return app_folder / "data_dir";

#ifdef _WIN32
    PWSTR   psz = nullptr;
    std::filesystem::path dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &psz)) && psz != nullptr)
        dir = std::filesystem::path(psz) / SLIC3R_APP_KEY;
    if (psz != nullptr)
        CoTaskMemFree(psz);
    return dir;
#elif __APPLE__
    const char* home_env = getenv("HOME");
    if (!home_env || home_env[0] == '\0')
        return {};
    return std::filesystem::path(home_env) / "Library" / "Application Support" / SLIC3R_APP_KEY;
#else
    return std::filesystem::path(get_linux_config_dir());
#endif
}

// The app config file (EdgeSlicer.conf) in that data directory.
static std::filesystem::path app_config_file()
{
    const std::filesystem::path dir = app_data_dir();
    if (dir.empty())
        return {};
    return dir / (SLIC3R_APP_KEY ".conf");
}

// The "app" section of the app config, or an empty object when the file is missing or broken.
static nlohmann::json read_app_section()
{
    const std::filesystem::path cfgfile = app_config_file();
    if (cfgfile.empty())
        return nlohmann::json::object();
    std::ifstream json_file(cfgfile);
    if (!json_file.is_open())
        return nlohmann::json::object();
    // A half-written or hand-edited config must not take the process down: this runs
    // before any of the app's own error reporting is up.
    try {
        nlohmann::json json_data;
        json_file >> json_data;
        auto app = json_data.value("app", nlohmann::json::object());
        return app.is_object() ? app : nlohmann::json::object();
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

namespace common
{
    static void take_datadir_arg(int argc, const std::vector<std::string>& args)
    {
        for (int i = 1; i < argc; ++i) {
            const std::string& a = args[size_t(i)];
            if (a == "--datadir" && i + 1 < argc) {
                g_datadir_override = args[size_t(i) + 1];
                ++i;
            } else if (a.rfind("--datadir=", 0) == 0) {
                g_datadir_override = a.substr(10);
            }
        }
    }

    void set_datadir_from_command_line(int argc, char** argv)
    {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i)
            args.emplace_back(argv[i] ? argv[i] : "");
        take_datadir_arg(argc, args);
    }

#ifdef _WIN32
    void set_datadir_from_command_line(int argc, wchar_t** argv)
    {
        std::vector<std::string> args;
        for (int i = 0; i < argc; ++i)
            args.emplace_back(wide_to_utf8(argv[i]));
        take_datadir_arg(argc, args);
    }
#endif

    std::string datadir_override() { return g_datadir_override; }

    bool is_relaunch_after_arg(const char* arg) { return arg != nullptr && std::strcmp(arg, "--relaunch-after") == 0; }
#ifdef _WIN32
    bool is_relaunch_after_arg(const wchar_t* arg) { return arg != nullptr && std::wcscmp(arg, L"--relaunch-after") == 0; }
#endif

    static void wait_for_process(unsigned long pid)
    {
        if (pid == 0)
            return;
        const auto started = std::chrono::steady_clock::now();
        const char* how    = "exited";
#ifdef _WIN32
        if (pid == ::GetCurrentProcessId())
            return;
        HANDLE h = ::OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
        if (h == nullptr)
            how = "gone"; // already exited (or not ours to wait for)
        else {
            if (::WaitForSingleObject(h, 120000) == WAIT_TIMEOUT)
                how = "timeout";
            ::CloseHandle(h);
        }
#else
        for (int i = 0; i < 2400; ++i) { // 2 minutes
            if (::kill(pid_t(pid), 0) != 0 && errno == ESRCH)
                break;
            if (i == 2399)
                how = "timeout";
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
#endif
        const long waited = long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
        const std::string note = "pid=" + std::to_string(pid) + " " + how + " waited_ms=" + std::to_string(waited);
#ifdef _WIN32
        ::SetEnvironmentVariableA("EDGESLICER_RELAUNCH_NOTE", note.c_str());
#else
        ::setenv("EDGESLICER_RELAUNCH_NOTE", note.c_str(), 1);
#endif
    }

    void wait_for_relaunch_parent(int argc, char** argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (is_relaunch_after_arg(argv[i])) {
                wait_for_process(std::strtoul(argv[i + 1] ? argv[i + 1] : "0", nullptr, 10));
                return;
            }
    }
#ifdef _WIN32
    void wait_for_relaunch_parent(int argc, wchar_t** argv)
    {
        for (int i = 1; i + 1 < argc; ++i)
            if (is_relaunch_after_arg(argv[i])) {
                wait_for_process(std::wcstoul(argv[i + 1] ? argv[i + 1] : L"0", nullptr, 10));
                return;
            }
    }
#endif

    std::string app_config_path()
    {
        const std::filesystem::path p = app_config_file();
#ifdef _WIN32
        return wide_to_utf8(p.wstring().c_str());
#else
        return p.string();
#endif
    }

    std::string get_app_config_string(const std::string& key)
    {
        const nlohmann::json app = read_app_section();
        auto it = app.find(key);
        if (it == app.end() || !it->is_string())
            return std::string();
        return it->get<std::string>();
    }

    bool get_app_config_bool(const std::string& key, bool default_value)
    {
        const nlohmann::json app = read_app_section();
        auto it = app.find(key);
        if (it == app.end())
            return default_value;
        // AppConfig writes "true"/"false" as JSON booleans; older files and hand edits use strings.
        if (it->is_boolean())
            return it->get<bool>();
        if (it->is_string()) {
            const std::string v = it->get<std::string>();
            return v == "true" || v == "1";
        }
        return default_value;
    }

    // Command-line options whose values are secrets. The phone hub's keeper process is started
    // with --hub-token <token>.
    static const char* const k_secret_flags[] = { "--hub-token" };

    void mask_secret_args(int argc, char** argv)
    {
        for (int i = 1; i < argc; ++i) {
            if (argv[i] == nullptr)
                continue;
            for (const char* flag : k_secret_flags) {
                const size_t n = strlen(flag);
                char*        value = nullptr;
                if (strcmp(argv[i], flag) == 0 && i + 1 < argc)
                    value = argv[i + 1];
                else if (strncmp(argv[i], flag, n) == 0 && argv[i][n] == '=')
                    value = argv[i] + n + 1;
                for (; value != nullptr && *value != '\0'; ++value)
                    *value = '*';
            }
        }
    }

#ifdef _WIN32
    void mask_secret_args_in_process_command_line()
    {
        // GetCommandLineW() hands out the buffer in the process parameters block (PEB), which is
        // exactly what crashpad copies into a Windows minidump.
        wchar_t* cl = ::GetCommandLineW();
        if (cl == nullptr)
            return;
        for (const char* flag : k_secret_flags) {
            std::wstring wflag(flag, flag + strlen(flag));
            for (wchar_t* p = wcsstr(cl, wflag.c_str()); p != nullptr; p = wcsstr(p, wflag.c_str())) {
                p += wflag.size();
                while (*p == L' ' || *p == L'\t' || *p == L'=' || *p == L'"')
                    ++p;
                for (; *p != L'\0' && *p != L' ' && *p != L'\t' && *p != L'"'; ++p)
                    *p = L'*';
            }
        }
    }
#endif

    std::string get_pc_name()
    {
        return boost::asio::ip::host_name();
    }

    std::string getMachineId()
    {
    std::string machineId = std::string();
#ifdef _WIN32

    auto wstringTostring = [](std::wstring wTmpStr) -> std::string {
        std::string resStr = std::string();
        int         len    = WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, nullptr, 0, nullptr, nullptr);

        if (len <= 0)
            return std::string();
        std::string desStr(len, 0);

        WideCharToMultiByte(CP_UTF8, 0, wTmpStr.c_str(), -1, &desStr[0], len, nullptr, nullptr);

        resStr = desStr;

        return resStr;
    };

    HKEY key = NULL;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
        wchar_t buffer[1024];
        memset(buffer, 0, sizeof(wchar_t) * 1024);
        DWORD size = sizeof(buffer);
        bool  ok   = (RegQueryValueEx(key, L"MachineGuid", NULL, NULL, (LPBYTE) buffer, &size) == ERROR_SUCCESS);
        RegCloseKey(key);
        if (ok) {
            machineId = wstringTostring(buffer);
        }
    }

#elif __APPLE__
    FILE* fp = NULL;
    char  buffer[1024];

    memset(buffer, 0, 1024);
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOPlatformExpertDevice"));
    CFStringRef  strRef  = (CFStringRef) IORegistryEntryCreateCFProperty(service, CFSTR(kIOPlatformUUIDKey), kCFAllocatorDefault, 0);
    CFStringGetCString(strRef, buffer, 1024, kCFStringEncodingMacRoman);
    machineId = buffer;

#endif // _WIN32
    return machineId;
    }
    std::string get_profile_version()
    { 
        std::string versionFilePath = "";
#ifdef _WIN32

       

        PWSTR   pszPath    = nullptr;
        char*   path       = new char[MAX_PATH]();
        size_t  pathLength = 0;
        HRESULT hr         = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
        if (SUCCEEDED(hr)) {
            wcstombs_s(&pathLength, path, MAX_PATH, pszPath, MAX_PATH);
            CoTaskMemFree(pszPath);
        }

        std::string filePath = path;
        versionFilePath      = filePath + "\\" + std::string(SLIC3R_APP_KEY "\\system\\Snapmaker.json");
        delete[] path;
#elif __APPLE__
        const char* home_env = getenv("HOME");
        versionFilePath      = home_env;
        versionFilePath      = versionFilePath + "/Library/Application Support/" SLIC3R_APP_KEY "/system/Snapmaker.json";
#else
        std::string config_dir = get_linux_config_dir();
        if (!config_dir.empty())
            versionFilePath = config_dir + "/system/Snapmaker.json";
#endif
        std::ifstream json_file(versionFilePath);
        if (!json_file.is_open()) {
            std::ifstream json_file(versionFilePath);
            return "";
        }
        nlohmann::json json_data;
        json_file >> json_data;
        std::string str_version      = json_data.value("version", "");

        return str_version;
    }

    std::string get_flutter_version()
    {

            std::string versionFilePath = "";

#ifdef _WIN32
            PWSTR   pszPath = nullptr;
            char*   path    = new char[MAX_PATH]();
            size_t  pathLength = 0;
            HRESULT hr         = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
            if (SUCCEEDED(hr)) {
                wcstombs_s(&pathLength, path, MAX_PATH, pszPath, MAX_PATH);
                CoTaskMemFree(pszPath);
            }

            std::string filePath = path;
            versionFilePath      = filePath + "\\" + std::string(SLIC3R_APP_KEY "\\web\\flutter_web\\version.json");

            delete[] path;
#elif __APPLE__
            const char* home_env = getenv("HOME");
            versionFilePath      = home_env;
            versionFilePath      = versionFilePath + "/Library/Application Support/" SLIC3R_APP_KEY "/web/flutter_web/version.json";
#else
            std::string config_dir = get_linux_config_dir();
            if (!config_dir.empty())
                versionFilePath = config_dir + "/web/flutter_web/version.json";
#endif

            std::ifstream json_file(versionFilePath);
            if (!json_file.is_open()) {
                std::ifstream json_file(versionFilePath);                
                return "";
            }
            nlohmann::json json_data;
            json_file >> json_data;
            std::string str_version = json_data.value("version", "");
            std::string str_build_number = json_data.value("build_number", "");

            std::string flutter_version = std::string("flutter_version: ") + str_version + std::string("  ") + std::string("build_number: ") +
                              str_build_number;
           
            return flutter_version;
    }


    std::string getLocalArea() 
    { 
        std::string localArea = "";
        std::string cfgfile = "";
        std::string versionFilePath = "";

#ifdef _WIN32

        PWSTR   pszPath = nullptr;
        char*   path    = new char[MAX_PATH]();
        size_t  pathLength = 0;
        HRESULT hr         = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
        if (SUCCEEDED(hr)) {
            wcstombs_s(&pathLength, path, MAX_PATH, pszPath, MAX_PATH);
            CoTaskMemFree(pszPath);
        } 

        std::string filePath = path;
        cfgfile              = filePath + "\\" + std::string(SLIC3R_APP_KEY "\\" SLIC3R_APP_KEY ".conf");
        delete[] path;

#elif __APPLE__
        const char* home_env = getenv("HOME");
        versionFilePath      = home_env;
        cfgfile              = versionFilePath + "/Library/Application Support/" SLIC3R_APP_KEY "/" SLIC3R_APP_KEY ".conf";
#else
        std::string config_dir = get_linux_config_dir();
        if (!config_dir.empty())
            cfgfile = config_dir + "/" SLIC3R_APP_KEY ".conf";
#endif
        std::ifstream json_file(cfgfile);
        if (!json_file.is_open()) {
            std::ifstream json_file(cfgfile);
            return "";
        }

        nlohmann::json json_data;
        json_file >> json_data;

        auto dataObj    = json_data.value("app", nlohmann::json::object());
        localArea = dataObj.value("region", "");

        return localArea;
    }

    std::string getLanguage() 
    {
        std::string localLanguage = "";
        std::string versionFilePath = "";
        std::string cfgfile       = "";
#ifdef _WIN32

        PWSTR   pszPath = nullptr;
        char*   path    = new char[MAX_PATH]();
        size_t  pathLength = 0;
        HRESULT hr         = SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &pszPath);
        if (SUCCEEDED(hr)) {
            wcstombs_s(&pathLength, path, MAX_PATH, pszPath, MAX_PATH);
            CoTaskMemFree(pszPath);
        } 

        std::string filePath = path;
        cfgfile              = filePath + "\\" + std::string(SLIC3R_APP_KEY "\\" SLIC3R_APP_KEY ".conf");
        delete[] path;

#elif __APPLE__
        const char* home_env = getenv("HOME");
        versionFilePath      = home_env;
        cfgfile              = versionFilePath + "/Library/Application Support/" SLIC3R_APP_KEY "/" SLIC3R_APP_KEY ".conf";
#else
        std::string config_dir = get_linux_config_dir();
        if (!config_dir.empty())
            cfgfile = config_dir + "/" SLIC3R_APP_KEY ".conf";
#endif
        std::ifstream json_file(cfgfile);
        if (!json_file.is_open()) {
            std::ifstream json_file(cfgfile);
            return "";
        }

        nlohmann::json json_data;
        json_file >> json_data;

       auto dataObj  = json_data.value("app", nlohmann::json::object());
       localLanguage = dataObj.value("language", "");

       return localLanguage;
    }
}