#include "FlutterPanel.hpp"

FlutterPanel::FlutterPanel(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxFULL_REPAINT_ON_RESIZE) {
    SetBackgroundColour(*wxBLACK);
}

bool FlutterPanel::startView(FlutterEngineHost* engine,
                              const std::string& entrypoint,
                              const std::string& channelName) {
    m_view = engine->createView(entrypoint, channelName);
    if (!m_view) return false;
    m_view->embedInto(GetHandle());
    return true;
}

void FlutterPanel::setHandler(FlutterViewHost::MethodCallHandler handler) {
    if (m_view) m_view->setMethodCallHandler(std::move(handler));
}
