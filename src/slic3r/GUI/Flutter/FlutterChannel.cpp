#include "FlutterChannel.h"

// ── FlutterChannel ────────────────────────────────────────────────

FlutterChannel::FlutterChannel(const std::string& name)
    : m_channelName(name) {}

FlutterChannel& FlutterChannel::on(const std::string& method, Handler h) {
    assert(!m_handlerCalled && "FlutterChannel::on: called after handler()");
    assert(h);
    assert(m_routes.find(method) == m_routes.end() &&
           "FlutterChannel::on: duplicate method");
    m_routes[method] = std::move(h);
    return *this;
}

FlutterChannel& FlutterChannel::use(std::shared_ptr<Middleware> m) {
    assert(m);
    assert(!m_handlerCalled && "FlutterChannel::use: called after handler()");
    if (!m_handlerCalled)
        m_middleware.push_back(std::move(m));
    return *this;
}

FlutterViewHost::MethodCallHandler
FlutterChannel::handler(FlutterViewHost* view) {
    assert(view);
    assert(!m_handlerCalled && "FlutterChannel::handler: already called");
    m_view = view;
    m_handlerCalled = true;

    auto routes     = m_routes;
    auto middleware = std::make_shared<std::vector<std::shared_ptr<Middleware>>>(
        m_middleware);

    auto replied = std::make_shared<bool>(false);
    return [routes = std::move(routes), middleware, replied](
               const std::string& method, const std::string& args,
               FlutterViewHost::Reply rawReply) {

        FlutterViewHost::Reply reply = [replied, rawReply = std::move(rawReply)]
            (const std::string& r) {
            if (*replied) return;
            *replied = true;
            rawReply(r);
        };

        // onInbound (forward)
        auto& mws = *middleware;
        for (size_t i = 0; i < mws.size(); ++i) {
            try {
                if (!mws[i]->onInbound(method, args, reply)) {
                    // short-circuit: fire earlier onOutbound (reverse)
                    for (int j = static_cast<int>(i) - 1; j >= 0; --j)
                        mws[j]->onOutbound(method, args);
                    return;
                }
            } catch (const std::exception& e) {
                reply(std::string("Error: ") + e.what());
                for (int j = static_cast<int>(i) - 1; j >= 0; --j)
                    mws[j]->onOutbound(method, args);
                return;
            } catch (...) {
                reply("Error: middleware exception");
                for (int j = static_cast<int>(i) - 1; j >= 0; --j)
                    mws[j]->onOutbound(method, args);
                return;
            }
        }

        // handler (with guard against unmatched routes + exceptions)
        auto it = routes.find(method);
        if (it != routes.end()) {
            try {
                it->second(args, reply);
            } catch (const std::exception& e) {
                reply(std::string("Error: ") + e.what());
            } catch (...) {
                reply("Error: unknown");
            }
        } else {
            reply(""); // unmatched route: prevent Dart hang
        }

        // onOutbound (reverse)
        for (int i = static_cast<int>(mws.size()) - 1; i >= 0; --i)
            mws[i]->onOutbound(method, args);
    };
}

void FlutterChannel::invoke(const std::string& method, const std::string& args)
{
    if (m_view) m_view->invokeMethod(method, args);
}

FlutterChannel::Namespace FlutterChannel::ns(const std::string& prefix) {
    return Namespace(this, prefix);
}

// ── Namespace ─────────────────────────────────────────────────────

FlutterChannel::Namespace::Namespace(FlutterChannel* parent,
                                     std::string prefix)
    : m_parent(parent), m_prefix(std::move(prefix)) {}

FlutterChannel::Namespace&
FlutterChannel::Namespace::on(const std::string& method, Handler h) {
    m_parent->on(m_prefix + "." + method, std::move(h));
    return *this;
}

void FlutterChannel::Namespace::invoke(const std::string& method,
                                       const std::string& args) {
    m_parent->invoke(m_prefix + "." + method, args);
}

// ── Middleware defaults ───────────────────────────────────────────

bool FlutterChannel::Middleware::onInbound(const std::string&,
                                           const std::string&, Reply) {
    return true;
}

void FlutterChannel::Middleware::onOutbound(const std::string&,
                                            const std::string&) {}
