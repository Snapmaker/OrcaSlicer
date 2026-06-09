#include "FlutterChannel.h"
#include "FlutterMiddleware.h"
#include <wx/app.h>
#include <boost/log/trivial.hpp>

// ── FlutterChannel ────────────────────────────────────────────────

FlutterChannel::FlutterChannel(const std::string& name)
    : m_channelName(name), m_state(std::make_shared<State>())
{
    m_routes["system.ready"] = [state = m_state](const std::string&, FlutterViewHost::Reply reply) {
        if (!state->view) {
            BOOST_LOG_TRIVIAL(warning) << "[flutter] drain: view is null, dropping "
                                       << state->pendingMessages.size() << " messages";
            state->pendingMessages.clear();
            return;
        }
        state->dartReady = true;
        reply("{\"ok\":true,\"data\":null}");
        while (!state->pendingMessages.empty()) {
            auto& msg = state->pendingMessages.front();
            state->view->invokeMethod(msg.first, msg.second);
            state->pendingMessages.pop_front();
        }
    };
}

FlutterChannel& FlutterChannel::on(const std::string& method, Handler h) {
    if (m_handlerCalled) {
        BOOST_LOG_TRIVIAL(error) << "[flutter] FlutterChannel::on: called after handler(), ignoring '" << method << "'";
        return *this;
    }
    if (!h) {
        BOOST_LOG_TRIVIAL(error) << "[flutter] FlutterChannel::on: null handler for '" << method << "', ignoring";
        return *this;
    }
    if (method == "system.ready") {
        BOOST_LOG_TRIVIAL(warning) << "[flutter] system.ready is reserved, ignoring user handler";
        return *this;
    }
    if (m_routes.find(method) != m_routes.end()) {
        BOOST_LOG_TRIVIAL(warning) << "[flutter] FlutterChannel::on: duplicate method '" << method << "', keeping first handler";
        return *this;
    }
    m_routes[method] = std::move(h);
    return *this;
}

FlutterChannel& FlutterChannel::use(std::shared_ptr<Middleware> m) {
    if (!m) {
        BOOST_LOG_TRIVIAL(error) << "[flutter] FlutterChannel::use: null middleware, ignoring";
        return *this;
    }
    if (m_handlerCalled) {
        BOOST_LOG_TRIVIAL(error) << "[flutter] FlutterChannel::use: called after handler(), ignoring";
        return *this;
    }
    m_middleware.push_back(std::move(m));
    return *this;
}

FlutterViewHost::MethodCallHandler
FlutterChannel::handler(FlutterViewHost* view) {
    if (!view) {
        return [](const std::string&, const std::string&, FlutterViewHost::Reply r) {
            r("Error: channel not bound — null view");
        };
    }
    if (m_handlerCalled) {
        return [](const std::string&, const std::string&, FlutterViewHost::Reply r) {
            r("Error: handler already consumed");
        };
    }
    m_state->view = view;
    m_handlerCalled = true;

    auto routes     = m_routes;
    auto middleware = std::make_shared<std::vector<std::shared_ptr<Middleware>>>(
        m_middleware);

    return [routes = std::move(routes), middleware](
               const std::string& method, const std::string& args,
               FlutterViewHost::Reply reply) {

        auto& mws = *middleware;

        auto rollbackOnOutbound = [&mws](const std::string& m, const std::string& a,
                                          size_t lastIdx) {
            for (int j = static_cast<int>(lastIdx); j >= 0; --j)
                mws[j]->onOutbound(m, a);
        };

        std::function<void(const std::string&, const std::string&, FlutterViewHost::Reply, size_t)> executeChain;
        executeChain = [&](const std::string& m, const std::string& a,
                           FlutterViewHost::Reply r, size_t startIdx = 0) {
            for (size_t i = startIdx; i < mws.size(); ++i) {
                if (!mws[i]->onInbound(m, a, r)) {
                    if (auto guard = std::dynamic_pointer_cast<ThreadGuardMiddleware>(mws[i])) {
                        guard->CallAfter([=]() { executeChain(m, a, r, i); });
                        return;
                    }
                    rollbackOnOutbound(m, a, i - 1);
                    return;
                }
            }
            auto it = routes.find(m);
            if (it != routes.end())
                it->second(a, r);
            else
                r("");
            for (int i = static_cast<int>(mws.size()) - 1; i >= 0; --i)
                mws[i]->onOutbound(m, a);
        };

        executeChain(method, args, reply, 0);
    };
}

bool FlutterChannel::invoke(const std::string& method, const std::string& args)
{
    if (!wxThread::IsMain()) {
        BOOST_LOG_TRIVIAL(error)
            << "[flutter] invoke(" << method << ") called from non-UI thread, dropping";
        return false;
    }
    auto& st = *m_state;
    if (!st.dartReady) {
        if (st.pendingMessages.size() >= 256) {
            BOOST_LOG_TRIVIAL(warning) << "[flutter] queue full, dropping oldest message";
            st.pendingMessages.pop_front();
        }
        st.pendingMessages.emplace_back(method, args);
        return true;
    }
    if (st.view) st.view->invokeMethod(method, args);
    return true;
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
