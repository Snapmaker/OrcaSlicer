#include "FlutterChannel.h"
#include <wx/app.h>
#include <wx/timer.h>
#include <boost/log/trivial.hpp>

// ── LoggingMiddleware ─────────────────────────────────────────

class LoggingMiddleware : public FlutterChannel::Middleware {
    bool onInbound(const std::string& method, const std::string& args,
                   FlutterViewHost::Reply) override {
        BOOST_LOG_TRIVIAL(trace) << "[flutter] " << method << " " << args;
        return true;
    }
    void onOutbound(const std::string& method,
                    const std::string& args) override {
        BOOST_LOG_TRIVIAL(trace) << "[flutter] out " << method << " " << args;
    }
};

// ── ThreadGuardMiddleware ─────────────────────────────────────

class ThreadGuardMiddleware : public FlutterChannel::Middleware {
    bool onInbound(const std::string&, const std::string&,
                   FlutterViewHost::Reply) override {
        assert(wxThread::IsMain());
        return true;
    }
};

// ── TimeoutMiddleware ─────────────────────────────────────────

class TimeoutMiddleware : public FlutterChannel::Middleware,
                          public wxEvtHandler {
    int m_sec;
public:
    explicit TimeoutMiddleware(int s) : m_sec(s) {}

    bool onInbound(const std::string&, const std::string&,
                   FlutterViewHost::Reply reply) override {
        auto fn    = std::make_shared<FlutterViewHost::Reply>(std::move(reply));
        auto fired = std::make_shared<bool>(false);
        auto t     = std::make_unique<wxTimer>(this);
        int sec = m_sec;
        t->Bind(wxEVT_TIMER, [fired, fn, sec](wxTimerEvent&) {
            if (!*fired) {
                *fired = true;
                (*fn)("{\"code\":\"TIMEOUT\","
                      "\"message\":\"Handler timed out after " +
                      std::to_string(sec) + "s\"}");
            }
        });
        t->Start(sec * 1000, true);
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

std::shared_ptr<FlutterChannel::Middleware> makeTimeoutMiddleware(int s)
    { return std::make_shared<TimeoutMiddleware>(s); }
