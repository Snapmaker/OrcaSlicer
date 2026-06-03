#pragma once
#include "flutter_host.h"
#include <string>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>
#include <cassert>

class FlutterChannel {
public:
    using Handler = std::function<void(const std::string&, FlutterViewHost::Reply)>;
    using Reply   = FlutterViewHost::Reply;
    using MiddlewarePtr = std::shared_ptr<class Middleware>;

    explicit FlutterChannel(const std::string& name);
    FlutterChannel(const FlutterChannel&) = delete;
    FlutterChannel& operator=(const FlutterChannel&) = delete;

    // Handler registration (must precede handler(view))
    FlutterChannel& on(const std::string& method, Handler h);

    // Namespace — prefix.method routing
    class Namespace {
    public:
        Namespace(const Namespace&) = delete;
        Namespace& operator=(const Namespace&) = delete;
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

    // C++ → Dart (fire-and-forget)
    void invoke(const std::string& method, const std::string& args);

    const std::string& name() const { return m_channelName; }

private:
    std::string m_channelName;
    std::unordered_map<std::string, Handler> m_routes;
    std::vector<std::shared_ptr<Middleware>> m_middleware;
    FlutterViewHost* m_view = nullptr; // main-thread only
    bool m_handlerCalled = false;
};

// Middleware factory functions
std::shared_ptr<FlutterChannel::Middleware> makeLoggingMiddleware();
std::shared_ptr<FlutterChannel::Middleware> makeThreadGuardMiddleware();
std::shared_ptr<FlutterChannel::Middleware> makeTimeoutMiddleware(int seconds);
