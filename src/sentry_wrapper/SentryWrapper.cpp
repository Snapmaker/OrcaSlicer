/**
 * @file SentryWrapper.cpp
 * @brief Sentry crash reporting wrapper implementation for cross-platform support.
 * 
 * This implementation provides a unified API for Sentry integration.
 * When SLIC3R_SENTRY is not defined, all functions become no-ops.
 */

#include "SentryWrapper.hpp"

#ifdef SLIC3R_SENTRY
#include "sentry.h"
#endif

#ifdef _WIN32
#include <Windows.h>
#include <shlobj.h>
#endif

#ifdef __APPLE__
#include <unistd.h>
#include <mach-o/dyld.h>
#include <libgen.h>
#include <string.h>
#endif

#include <cstdlib>
#include <atomic>

namespace Slic3r {

#ifdef SLIC3R_SENTRY

static sentry_value_t on_crash_callback(const sentry_ucontext_t* uctx, sentry_value_t event, void* closure)
{
    (void) uctx;
    (void) closure;

    // tell the backend to retain the event
    return event;
}

void initSentryEx()
{
    sentry_options_t* options = sentry_options_new();
    std::string       dsn     = "";
    {
#ifdef __APPLE__

        std::string dsn = std::string("https://ac473187efb8877f36bd31694ffd5dec@o4508125599563776.ingest.us.sentry.io/4510425212059648");

#elif _WIN32
        std::string dsn = std::string("https://c74b617c2aedc291444d3a238d23e780@o4508125599563776.ingest.us.sentry.io/4510425163956224");
#endif
        sentry_options_set_dsn(options, dsn.c_str());
        std::string handlerDir  = "";
        std::string dataBaseDir = "";

#ifdef __APPLE__

        char     exe_path[PATH_MAX] = {0};
        uint32_t buf_size           = PATH_MAX;

        if (_NSGetExecutablePath(exe_path, &buf_size) != 0) {
            throw std::runtime_error("Buffer too small for executable path");
        }

        // Get the directory containing the executable, not the executable path itself
        // Use dirname() to get parent directory (need to copy string as dirname may modify it)
        char exe_path_copy[PATH_MAX];
        strncpy(exe_path_copy, exe_path, PATH_MAX);
        char* exe_dir = dirname(exe_path_copy);
        handlerDir = std::string(exe_dir) + "/crashpad_handler";

        const char* home_env = getenv("HOME");

        dataBaseDir = home_env;
        dataBaseDir = dataBaseDir + "/Library/Application Support/Snapmaker_Orca/SentryData";
#elif _WIN32
        wchar_t exeDir[MAX_PATH];
        ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        std::wstring wsExeDir(exeDir);
        int          nPos     = wsExeDir.find_last_of('\\');
        std::wstring wsDmpDir = wsExeDir.substr(0, nPos + 1);
        std::wstring desDir   = wsDmpDir + L"crashpad_handler.exe";
        wsDmpDir += L"dump";

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

        handlerDir = wstringTostring(desDir);

        wchar_t appDataPath[MAX_PATH] = {0};
        auto    hr                    = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath);
        char*   path                  = new char[MAX_PATH];
        size_t  pathLength;
        wcstombs_s(&pathLength, path, MAX_PATH, appDataPath, MAX_PATH);
        std::string filePath = path;
        std::string appName  = "\\" + std::string("Snapmaker_Orca\\");
        dataBaseDir          = filePath + appName;
#endif

        if (!handlerDir.empty())
            sentry_options_set_handler_path(options, handlerDir.c_str());

        if (!dataBaseDir.empty())
            sentry_options_set_database_path(options, dataBaseDir.c_str());

        std::string softVersion = "snapmaker_orca_2.2.0_beta2";
        sentry_options_set_release(options, softVersion.c_str());

#if defined(_DEBUG) || !defined(NDEBUG)
        sentry_options_set_debug(options, 1);
#else
        sentry_options_set_debug(options, 0);
#endif

        sentry_options_set_environment(options, "develop");

        sentry_options_set_auto_session_tracking(options, 0);
        sentry_options_set_symbolize_stacktraces(options, 1);
        sentry_options_set_on_crash(options, on_crash_callback, NULL);
        sentry_options_set_before_send(options, NULL, NULL);

        sentry_options_set_sample_rate(options, 1.0);
        sentry_options_set_traces_sample_rate(options, 1.0);

        sentry_init(options);
        sentry_start_session();
    }
}

void exitSentryEx()
{ 
    sentry_close();
}
void sentryReportLogEx(SENTRY_LOG_LEVEL   logLevel,
                         const std::string& logContent,
                         const std::string& funcModule,
                         const std::string& logTagKey,
                         const std::string& logTagValue,
                         const std::string& logTraceId)
{
    sentry_level_t sentry_msg_level;
    switch (logLevel)
    {
    case SENTRY_LOG_TRACE: 
        sentry_msg_level = SENTRY_LEVEL_TRACE;
        break;
    case SENTRY_LOG_DEBUG: 
        sentry_msg_level = SENTRY_LEVEL_DEBUG; 
        break;
    case SENTRY_LOG_INFO: 
        sentry_msg_level = SENTRY_LEVEL_INFO; 
        break;
    case SENTRY_LOG_WARNING: 
        sentry_msg_level = SENTRY_LEVEL_WARNING; 
        break;
    case SENTRY_LOG_ERROR: 
        sentry_msg_level = SENTRY_LEVEL_ERROR;
        break;
    case SENTRY_LOG_FATAL: 
        sentry_msg_level = SENTRY_LEVEL_FATAL;
        break;
    default:
        return;
    }

     sentry_value_t event = sentry_value_new_message_event(sentry_msg_level,           
                                                           funcModule.c_str(), 
                                                           logContent.c_str()
    );

    if (!logTraceId.empty())
        sentry_set_trace(logTraceId.c_str(), "");

    if (!logTagKey.empty())
        sentry_set_tag(logTagKey.c_str(), logTagValue.c_str());

    sentry_capture_event(event);
}


#else // SLIC3R_SENTRY not defined - provide no-op implementations


#endif // SLIC3R_SENTRY

void initSentry()
{
#ifdef SLIC3R_SENTRY
    initSentryEx();
#endif
}

void exitSentry()
{
#ifdef SLIC3R_SENTRY
    exitSentryEx();
#endif
}
void sentryReportLog(SENTRY_LOG_LEVEL   logLevel,
    const std::string& logContent,
    const std::string& funcModule,
    const std::string& logTagKey,
    const std::string& logTagValue,
    const std::string& logTraceId)
{
#ifdef SLIC3R_SENTRY
    sentryReportLogEx(logLevel, logContent, funcModule, logTagKey, logTagValue, logTraceId);
#endif
}

} // namespace Slic3r

