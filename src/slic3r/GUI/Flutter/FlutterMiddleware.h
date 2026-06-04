#pragma once
#include "FlutterChannel.h"
#include <wx/event.h>

class ThreadGuardMiddleware : public FlutterChannel::Middleware,
                               public wxEvtHandler {
public:
    bool onInbound(const std::string& method, const std::string& args,
                   FlutterViewHost::Reply reply) override;
};
