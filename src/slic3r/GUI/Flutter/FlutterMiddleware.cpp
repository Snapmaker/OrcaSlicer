#include "FlutterChannel.h"
#include "FlutterMiddleware.h"
#include <algorithm>
#include <wx/app.h>
#include <wx/timer.h>
#include <boost/log/trivial.hpp>

// ── LoggingMiddleware ─────────────────────────────────────────

class LoggingMiddleware : public FlutterChannel::Middleware {
    uint64_t m_nextId = 0;
    uint64_t m_currentId = 0;
    bool onInbound(const std::string& method, const std::string& args,
                   FlutterViewHost::Reply) override {
        m_currentId = ++m_nextId;
#ifdef ORCA_FLUTTER_VERBOSE
        BOOST_LOG_TRIVIAL(trace) << "[flutter][" << m_currentId << "] " << method << " " << args;
#endif
        return true;
    }
    void onOutbound(const std::string& method,
                    const std::string& args) override {
#ifdef ORCA_FLUTTER_VERBOSE
        BOOST_LOG_TRIVIAL(trace) << "[flutter][" << m_currentId << "] out " << method << " " << args;
#endif
    }
};

// ── ThreadGuardMiddleware ─────────────────────────────────────

bool ThreadGuardMiddleware::onInbound(const std::string&, const std::string&,
                                      FlutterViewHost::Reply) {
    if (!wxThread::IsMain()) {
        BOOST_LOG_TRIVIAL(error) << "ThreadGuardMiddleware: onInbound called from non-UI thread";
        return false;
    }
    return true;
}

// ── TimeoutMiddleware ─────────────────────────────────────────

class TimeoutMiddleware : public FlutterChannel::Middleware,
                          public wxEvtHandler {
    std::chrono::seconds m_timeout;
public:
    explicit TimeoutMiddleware(std::chrono::seconds s) : m_timeout(s) {}
        ~TimeoutMiddleware() {
            for (auto& t : m_timers) {
                if (t->IsRunning()) t->Stop();
            }
        }

    bool onInbound(const std::string&, const std::string&,
                   FlutterViewHost::Reply reply) override {
        // Clean up fired one-shot timers
        m_timers.erase(
            std::remove_if(m_timers.begin(), m_timers.end(),
                [](const auto& t) { return !t->IsRunning(); }),
            m_timers.end());
        auto fn    = std::make_shared<FlutterViewHost::Reply>(std::move(reply));
        auto fired = std::make_shared<bool>(false);
        auto t     = std::make_unique<wxTimer>(this);
        long long sec = m_timeout.count();
        t->Bind(wxEVT_TIMER, [this, fired, fn, sec, rawPtr = t.get()](wxTimerEvent&) {
            if (!*fired) {
                *fired = true;
                (*fn)("{\"code\":\"TIMEOUT\","
                      "\"message\":\"Handler timed out after " +
                      std::to_string(sec) + "s\"}");
            }
            m_timers.erase(
                std::remove_if(m_timers.begin(), m_timers.end(),
                    [rawPtr](const auto& tp) { return tp.get() == rawPtr; }),
                m_timers.end());
        });
        t->Start(static_cast<int>(sec * 1000), true);
        m_timers.push_back(std::move(t));
        return true;
    }

private:
    std::vector<std::unique_ptr<wxTimer>> m_timers;
};

// ── Factories ─────────────────────────────────────────────────

std::shared_ptr<FlutterChannel::Middleware> makeLoggingMiddleware()
    { return std::make_shared<LoggingMiddleware>(); }

std::shared_ptr<FlutterChannel::Middleware> makeThreadGuardMiddleware()
    { return std::make_shared<ThreadGuardMiddleware>(); }

std::shared_ptr<FlutterChannel::Middleware> makeTimeoutMiddleware(std::chrono::seconds s)
    { return std::make_shared<TimeoutMiddleware>(s); }
