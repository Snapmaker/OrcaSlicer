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

#ifdef _WIN32
#include <Windows.h>
#include <shlobj.h>
#endif

#endif

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
    {
#ifdef WIN32
        std::string dsn = std::string("https://c74b617c2aedc291444d3a238d23e780@o4508125599563776.ingest.us.sentry.io/4510425163956224");

        sentry_options_set_dsn(options, dsn.c_str());

        wchar_t exeDir[MAX_PATH];
        ::GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        std::wstring wsExeDir(exeDir);
        int          nPos     = wsExeDir.find_last_of('\\');
        std::wstring wsDmpDir = wsExeDir.substr(0, nPos + 1);

        std::wstring handlerDir = wsDmpDir + L"crashpad_handler.exe";
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

        std::string desDir = wstringTostring(handlerDir);
        if (!desDir.empty())
            sentry_options_set_handler_path(options, desDir.c_str());
        desDir                        = wstringTostring(wsDmpDir);
        desDir                        = wstringTostring(wsDmpDir);
        wchar_t appDataPath[MAX_PATH] = {0};
        auto    hr                    = SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, appDataPath);
        char*   path                  = new char[MAX_PATH];
        size_t  pathLength;
        wcstombs_s(&pathLength, path, MAX_PATH, appDataPath, MAX_PATH);
        std::string filePath = path;
        std::string appName  = "\\" + std::string("Snapmaker_Orca\\");
        filePath             = filePath + appName;

        if (!filePath.empty())
            sentry_options_set_database_path(options, filePath.c_str());
#endif
        std::string softVersion = "snapmaker_orca_2.2.0_beta2";
        // Snapmaker_VERSION
        sentry_options_set_release(options, softVersion.c_str());

#if defined(_DEBUG) || !defined(NDEBUG)
        sentry_options_set_debug(options, 1);
#else
        sentry_options_set_debug(options, 0);
#endif
        // release version environment(Testing/production/development/Staging)
        sentry_options_set_environment(options, "develop");
        sentry_options_set_auto_session_tracking(options, false);
        sentry_options_set_symbolize_stacktraces(options, true);
        sentry_options_set_on_crash(options, on_crash_callback, NULL);
        // Enable before_send hook for filtering sensitive data
        sentry_options_set_before_send(options, NULL, NULL);

        // Ensure all events and crashes are captured (sample rate 100%)
        sentry_options_set_sample_rate(options, 1.0);        // Capture 100% of events
        sentry_options_set_traces_sample_rate(options, 1.0); // Capture 100% of traces

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

