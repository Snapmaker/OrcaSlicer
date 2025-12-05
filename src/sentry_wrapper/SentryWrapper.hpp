#ifndef slic3r_SentryWrapper_hpp_
#define slic3r_SentryWrapper_hpp_

#include <string>

namespace Slic3r {

	void initSentry();

	void exitSentry();

    typedef enum SENTRY_LOG_LEVEL {
        SENTRY_LOG_TRACE   = -2,
        SENTRY_LOG_DEBUG   = -1,
        SENTRY_LOG_INFO    = 0,
        SENTRY_LOG_WARNING = 1,
        SENTRY_LOG_ERROR   = 2,
        SENTRY_LOG_FATAL   = 3,
    };

	void sentryReportLog(SENTRY_LOG_LEVEL   logLevel,
                         const std::string& logContent,
                         const std::string& funcModule  = "",
                         const std::string& logTagKey   = "",
                         const std::string& logTagValue = "",
                         const std::string& logTraceId = "");  
    } // namespace Slic3r

#endif // slic3r_SentryWrapper_hpp_

