#include "FlutterPanel.hpp"
#include "FlutterMiddleware.h"
#include <cassert>

FlutterPanel::FlutterPanel(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxFULL_REPAINT_ON_RESIZE | wxCLIP_CHILDREN) {
    SetBackgroundColour(*wxBLACK);
    Bind(wxEVT_SIZE, &FlutterPanel::onSize, this);
    Bind(wxEVT_SHOW, &FlutterPanel::onShow, this);
    Bind(wxEVT_SET_FOCUS, &FlutterPanel::onSetFocus, this);
}

// Lazy-init — first call creates the channel with the given name; subsequent
// calls return the existing channel (name parameter is ignored).
FlutterChannel& FlutterPanel::channel(const std::string& name) {
    if (!m_channel) {
        m_channel = std::make_unique<FlutterChannel>(name);
    }
    return *m_channel;
}

FlutterChannel& FlutterPanel::channel() {
    assert(m_channel);
    return *m_channel;
}

bool FlutterPanel::startView(FlutterEngineHost* engine,
                              const std::string& entrypoint,
                              const std::string& channelName) {
    m_view = engine->createView(entrypoint, channelName);
    if (!m_view) return false;

    if (!m_channel) {
        m_channel = std::make_unique<FlutterChannel>(channelName);
    }

    m_channel->use(makeThreadGuardMiddleware());
    m_channel->use(makeLoggingMiddleware());

    m_view->setMethodCallHandler(m_channel->handler(m_view.get()));

    tryEmbed();
    if (m_embedded) {
        wxSize sz = GetSize();
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    return true;
}

void FlutterPanel::resizeView(int width, int height) {
    if (m_view) m_view->resize(width, height);
}

void FlutterPanel::onShow(wxShowEvent& event) {
    if (event.IsShown() && m_view) {
        tryEmbed();
        wxSize sz = GetSize();
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    event.Skip();
}

void FlutterPanel::tryEmbed() {
    if (m_embedded || !m_view) return;
    m_view->embedInto(GetHandle());
    m_embedded = true;
}

void FlutterPanel::onSetFocus(wxFocusEvent& event) {
    CallAfter([this] {
        if (m_view) m_view->focus();
    });
    event.Skip();
}

void FlutterPanel::SetFocus() {
#ifdef __WXMSW__
    wxWindow::SetFocus();
#else
    wxWindow::SetFocus();
    if (m_view) m_view->focus();
#endif
}

void FlutterPanel::onSize(wxSizeEvent& event) {
    if (m_view) {
        tryEmbed();
        wxSize sz = event.GetSize();
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    event.Skip();
}

#ifdef __WXMSW__
bool FlutterPanel::MSWShouldPreProcessMessage(WXMSG* msg) {
    if (m_view) {
        HWND child = (HWND)m_view->nativeHandle();
        if (child && msg->hwnd == child)
            return false;
    }
    return wxWindow::MSWShouldPreProcessMessage(msg);
}

WXHWND FlutterPanel::MSWGetFocusHWND() const {
    if (m_view) {
        void* childHwnd = m_view->nativeHandle();
        if (childHwnd)
            return (WXHWND)childHwnd;
    }
    return GetHWND();
}

bool FlutterPanel::ContainsHWND(WXHWND hWnd) const {
    if (m_view) {
        void* childHwnd = m_view->nativeHandle();
        if (childHwnd && hWnd == (WXHWND)childHwnd)
            return true;
    }
    return false;
}
#endif
