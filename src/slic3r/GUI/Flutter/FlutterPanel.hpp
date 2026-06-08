#pragma once
#include <wx/wx.h>
#include <memory>
#include "FlutterChannel.h"

class FlutterPanel : public wxWindow {
public:
    FlutterPanel(wxWindow* parent);

    /// Creates the FlutterChannel with the given name. Must be called BEFORE
    /// startView() so that handlers registered via on() are captured when
    /// the channel is bound to the view.
    FlutterChannel& channel(const std::string& name);

    /// Creates the FlutterView and binds the channel.  Handlers must be
    /// registered via channel().on(...) before calling startView().
    bool startView(FlutterEngineHost* engine,
                   const std::string& entrypoint,
                   const std::string& channelName);

    /// Access the already-created channel (asserts if not yet created).
    FlutterChannel& channel();

    void resizeView(int width, int height);

    void onSize(wxSizeEvent& event);
    void onShow(wxShowEvent& event);
    void onSetFocus(wxFocusEvent& event);

    virtual void SetFocus() override;

    bool AcceptsFocus() const override { return false; }

    void tryEmbed();

protected:
    // Declaration order sets destruction order (reverse): m_channel dies AFTER m_view.
    std::unique_ptr<FlutterChannel>  m_channel;
    std::unique_ptr<FlutterViewHost> m_view;
    bool m_embedded = false;

#ifdef __WXMSW__
    virtual WXHWND MSWGetFocusHWND() const override;
    virtual bool ContainsHWND(WXHWND hWnd) const override;
    bool MSWShouldPreProcessMessage(WXMSG* msg) override;
#endif
};
