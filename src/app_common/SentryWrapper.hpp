#ifndef slic3r_SentryWrapper_hpp_
#define slic3r_SentryWrapper_hpp_

#include <string>
#include "app_common/bury_point.hpp"

namespace Slic3r {

	void initSentry();

	void exitSentry();

	// Bounded flush for paths that exit the process right after reporting
	// (the OOM farewell): waits up to timeout_ms for queued events to be
	// delivered, without shutting the SDK down (the launcher owns its
	// lifetime). Safe no-op when Sentry is not initialized.
	void flushSentry(unsigned int timeout_ms);

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

    void set_sentry_tags(const std::string& tag_key,const std::string& tag_value);
    } // namespace Slic3r

#endif // slic3r_SentryWrapper_hpp_

