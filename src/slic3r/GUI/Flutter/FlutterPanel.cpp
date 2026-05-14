#include "FlutterPanel.hpp"

FlutterPanel::FlutterPanel(wxWindow* parent)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
               wxFULL_REPAINT_ON_RESIZE | wxCLIP_CHILDREN) {
    SetBackgroundColour(*wxBLACK);
    Bind(wxEVT_SIZE, &FlutterPanel::onSize, this);
    Bind(wxEVT_SHOW, &FlutterPanel::onShow, this);
    Bind(wxEVT_SET_FOCUS, &FlutterPanel::onSetFocus, this);
}

bool FlutterPanel::startView(FlutterEngineHost* engine,
                              const std::string& entrypoint,
                              const std::string& channelName,
                              FlutterViewHost::MethodCallHandler handler) {
    m_view = engine->createView(entrypoint, channelName);
    if (!m_view) return false;
    if (handler) m_view->setMethodCallHandler(std::move(handler));

    // Defer embedInto until the panel has a valid size.  When this panel
    // is on a non-selected notebook page it has a 0×0 or 1×1 allocation,
    // and calling embedInto would fall back to a hard-coded default
    // (800×600) that is visible for one frame before the first wxEVT_SIZE
    // corrects it.  tryEmbed() is called from onShow / onSize and handles
    // the one-shot embed at the real client size.
    tryEmbed();
    if (m_embedded) {
        wxSize sz = GetSize();
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    return true;
}

void FlutterPanel::setHandler(FlutterViewHost::MethodCallHandler handler) {
    if (m_view) m_view->setMethodCallHandler(std::move(handler));
}

void FlutterPanel::onShow(wxShowEvent& event) {
    if (event.IsShown() && m_view) {
        tryEmbed();
        // Use GetSize() (wxWidgets-tracked size) rather than
        // GetClientSize() (GTK allocation).  After DoSetSelection
        // calls SetSize, the wx size is correct even if GTK hasn't
        // updated the widget allocation yet.
        wxSize sz = GetSize();
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    event.Skip();
}

void FlutterPanel::tryEmbed() {
    if (m_embedded || !m_view) return;
    wxSize sz = GetSize();
    if (sz.GetWidth() <= 1 || sz.GetHeight() <= 1) return;
    m_view->embedInto(GetHandle());
    m_embedded = true;
}

void FlutterPanel::onSetFocus(wxFocusEvent& event) {
    // Defer via CallAfter: on Windows, calling ::SetFocus during
    // WM_SETFOCUS processing is silently ignored by the OS.
    CallAfter([this] {
        if (m_view) m_view->focus();
    });
    event.Skip();
}

void FlutterPanel::SetFocus() {
#ifdef __WXMSW__
    // MSWGetFocusHWND() override returns the Flutter child HWND, so
    // wxWindow::SetFocus() calls ::SetFocus(childHwnd) directly.  No
    // two-step focus dance needed — eliminates the race where notebook
    // or mouse-click handling could reclaim focus from child to panel.
    wxWindow::SetFocus();
#else
    wxWindow::SetFocus();
    if (m_view) m_view->focus();
#endif
}

void FlutterPanel::onSize(wxSizeEvent& event) {
    if (m_view) {
        tryEmbed();
        // Use event.GetSize() rather than GetClientSize(): on wxGTK,
        // GetClientSize() queries the GTK allocation which may be stale
        // if GTK hasn't processed the size allocation yet.
        wxSize sz = event.GetSize();
        if (sz.GetWidth() > 1 && sz.GetHeight() > 1)
            m_view->resize(sz.GetWidth(), sz.GetHeight());
    }
    event.Skip();
}

#ifdef __WXMSW__
WXLRESULT FlutterPanel::MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) {
    if (m_view) {
        HWND child = (HWND)m_view->nativeHandle();
        if (child && (msg == WM_KEYDOWN || msg == WM_KEYUP ||
                      msg == WM_CHAR || msg == WM_DEADCHAR ||
                      msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
                      msg == WM_SYSCHAR || msg == WM_SYSDEADCHAR)) {
            // Forward keyboard messages directly to the Flutter child HWND.
            // TranslateMessage generates WM_CHAR for the HWND returned by
            // GetFocus(), which may briefly be the panel HWND rather than
            // the Flutter child.  Forwarding ensures the Flutter engine's
            // KeyboardManager always sees the WM_CHAR it expects after
            // each WM_KEYDOWN.
            return ::SendMessage(child, msg, wParam, lParam);
        }
    }
    return wxWindow::MSWWindowProc(msg, wParam, lParam);
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
