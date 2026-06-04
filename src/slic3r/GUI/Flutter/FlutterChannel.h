#pragma once
#include "flutter_host.h"
#include <string>
#include <functional>
#include <memory>
#include <chrono>
#include <deque>
#include <unordered_map>
#include <utility>
#include <vector>

class FlutterChannel {
public:
    using Handler = std::function<void(const std::string&, FlutterViewHost::Reply)>;
    using Reply   = FlutterViewHost::Reply;
    using MiddlewarePtr = std::shared_ptr<class Middleware>;

    explicit FlutterChannel(const std::string& name);
    FlutterChannel(const FlutterChannel&) = delete;
    FlutterChannel& operator=(const FlutterChannel&) = delete;
    FlutterChannel(FlutterChannel&&) = delete;
    FlutterChannel& operator=(FlutterChannel&&) = delete;

    // Handler registration (must precede handler(view))
    FlutterChannel& on(const std::string& method, Handler h);

    // Namespace — prefix.method routing
    class Namespace {
    public:
        Namespace(const Namespace&) = delete;
        Namespace& operator=(const Namespace&) = delete;
        Namespace(Namespace&&) = delete;
        Namespace& operator=(Namespace&&) = delete;
        Namespace& on(const std::string& method, Handler h);
        void invoke(const std::string& method, const std::string& args);
    private:
        friend class FlutterChannel;
        Namespace(FlutterChannel* parent, std::string prefix);
        FlutterChannel* m_parent;
        std::string m_prefix;
    };
    Namespace ns(const std::string& prefix);

    // Middleware pipeline
    class Middleware {
    public:
        virtual ~Middleware() = default;
        virtual bool onInbound(const std::string& method,
                               const std::string& args, Reply reply);
        virtual void onOutbound(const std::string& method,
                                const std::string& args);
    };
    FlutterChannel& use(std::shared_ptr<Middleware> m);

    // View binding (one-step, cannot call twice)
    FlutterViewHost::MethodCallHandler handler(FlutterViewHost* view);

    // C++ → Dart (fire-and-forget, returns false if called from wrong thread)
    bool invoke(const std::string& method, const std::string& args);

    const std::string& name() const { return m_channelName; }

private:
    struct State {
        FlutterViewHost* view = nullptr;
        bool dartReady = false;
        std::deque<std::pair<std::string, std::string>> pendingMessages;
    };
    std::shared_ptr<State> m_state;
    std::string m_channelName;
    std::unordered_map<std::string, Handler> m_routes;
    std::vector<std::shared_ptr<Middleware>> m_middleware;
    bool m_handlerCalled = false;
};

// Middleware factory functions
std::shared_ptr<FlutterChannel::Middleware> makeLoggingMiddleware();
std::shared_ptr<FlutterChannel::Middleware> makeThreadGuardMiddleware();
std::shared_ptr<FlutterChannel::Middleware> makeTimeoutMiddleware(std::chrono::seconds s);
