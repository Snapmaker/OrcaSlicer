// Thin adapter: a wxTextCtrl subclass on MSW (MSW-specific colour hook),
// a plain typedef elsewhere.
#ifndef slic3r_widgets_TextCtrl_h_
#define slic3r_widgets_TextCtrl_h_

#ifdef __WXMSW__
class TextCtrl : public wxTextCtrl
{
public:
    using wxTextCtrl::wxTextCtrl;
    WXHBRUSH DoMSWControlColor(WXHDC pDC, wxColour colBg, WXHWND hWnd) { return wxTextCtrl::DoMSWControlColor(pDC, wxColour(), hWnd); }
};
#else
typedef wxTextCtrl TextCtrl;
#endif

#endif // slic3r_widgets_TextCtrl_h_
