#include "ScalableBitmap.hpp"

#include <widgets/BitmapCache.hpp>
#include <widgets/UiAppearance.hpp>

#include <boost/algorithm/string/replace.hpp>
#include <libslic3r/Exception.hpp>

#include <algorithm>
#include <wx/log.h>

int em_unit(wxWindow* win)
{
    if (win)
    {
        const int em = Slic3r::GUI::ui().em_unit_of(win);
        if (em > 0)
            return em;
    }

    return Slic3r::GUI::ui().em_unit();
}

// ---- fix-widget-resource-crash: controlled degradation for missing resources ----
// Registers missing resource names (once per name) with a one-shot warning;
// used for runtime diagnostics and test assertions (never silently ignored).
std::vector<std::string> const& missing_bitmap_resources()
{
    static std::vector<std::string> inst;
    return inst;
}

void register_missing_bitmap(const std::string& bmp_name)
{
    auto& list = const_cast<std::vector<std::string>&>(missing_bitmap_resources());
    if (std::find(list.begin(), list.end(), bmp_name) == list.end()) {
        list.push_back(bmp_name);
        wxLogWarning("Bitmap resource not found, placeholder in use: %s", bmp_name);
    }
}

// Recognizable placeholder: magenta/dark-grey checkerboard (semi-transparent)
// that visibly reads as "resource missing".
wxBitmap missing_resource_placeholder(unsigned width, unsigned height)
{
    if (width == 0)
        width = 16;
    if (height == 0)
        height = 16;
    wxImage img(static_cast<int>(width), static_cast<int>(height));
    img.InitAlpha();
    for (int y = 0; y < static_cast<int>(height); ++y)
        for (int x = 0; x < static_cast<int>(width); ++x) {
            const bool magenta = ((x / 3) + (y / 3)) % 2 == 0;
            img.SetRGB(x, y, magenta ? 200 : 40, magenta ? 0 : 40, magenta ? 110 : 40);
            img.SetAlpha(x, y, 200);
        }
    return wxBitmap(img);
}


// win is used to get a correct em_unit value
// It's important for bitmaps of dialogs.
// if win == nullptr, em_unit value of MainFrame will be used
wxBitmap create_scaled_bitmap(  const std::string& bmp_name_in,
                                wxWindow *win/* = nullptr*/,
                                const int px_cnt/* = 16*/,
                                const bool grayscale/* = false*/,
                                const std::string& new_color/* = std::string()*/, // color witch will used instead of orange
                                const bool menu_bitmap/* = false*/,
                                const bool resize/* = false*/,
                                const bool bitmap2/* = false*/,
                                const std::vector<std::string>& array_new_color/* = vector<std::string>*/)//used for semi transparent material)
{
    static Slic3r::GUI::BitmapCache cache;
    if (win == nullptr)
        win = Slic3r::GUI::ui().mainframe();
    if (bitmap2) {
        return create_scaled_bitmap2(bmp_name_in, cache, win, px_cnt, grayscale, resize, array_new_color);
    }
    unsigned int width = 0;
    // win may be null (the default UiAppearance exposes no mainframe): fall back
// to raw pixels for the DIP conversion.
    unsigned int height =
        static_cast<unsigned int>((win ? win->FromDIP(px_cnt) : px_cnt) + 0.5f);

    std::string bmp_name = bmp_name_in;
    boost::replace_last(bmp_name, ".png", "");

    bool dark_mode =
#ifdef _WIN32
    menu_bitmap ? Slic3r::GUI::ui().menu_dark_mode() :
#endif
    Slic3r::GUI::ui().dark_mode();

    // Try loading an SVG first, then PNG if SVG was not found:
    wxBitmap *bmp = cache.load_svg(bmp_name, width, height, grayscale, dark_mode, new_color, resize ? em_unit(win) * 0.1f : 0.f);
    if (bmp == nullptr) {
        bmp = cache.load_png(bmp_name, width, height, grayscale, resize ? (win ? win->FromDIP(10) : 10) * 0.1f : 0.f);
    }

    if (bmp == nullptr) {
        // Controlled degradation (fix-widget-resource-crash): a missing resource must
// not throw - the exception would escape from widget
        // constructors (crashed in practice, see G4). Draw a placeholder bitmap and
// log the miss instead (never silent).
        register_missing_bitmap(bmp_name);
        unsigned pw =
            width != 0 ? width : static_cast<unsigned>((win ? win->FromDIP(16) : 16) + 0.5f);
        unsigned ph = height != 0 ? height : pw;
        return *cache.insert(bmp_name + "-h" + std::to_string(ph) + "-placeholder",
            wxBitmap(missing_resource_placeholder(pw, ph)));
    }

    return *bmp;
}

wxBitmap create_scaled_bitmap2(const std::string& bmp_name_in, Slic3r::GUI::BitmapCache& cache, wxWindow* win/* = nullptr*/ ,
    const int px_cnt/* = 16*/, const bool grayscale/* = false*/ , const bool resize/* = false*/ ,
    const std::vector<std::string>& array_new_color/* = vector<std::string>()*/) // color witch will used instead of orange
{
    unsigned int width = 0;
    unsigned int height =
        static_cast<unsigned int>((win ? win->FromDIP(px_cnt) : px_cnt) + 0.5f);

    std::string bmp_name = bmp_name_in;
    boost::replace_last(bmp_name, ".png", "");

    wxBitmap* bmp = cache.load_svg2(bmp_name, width, height, grayscale, false, array_new_color, resize ? em_unit(win) * 0.1f : 0.f);
    if (bmp == nullptr) {
        // Controlled degradation (as above): placeholder + registry entry, no throw.
        register_missing_bitmap(bmp_name);
        unsigned ph =
            static_cast<unsigned>((win ? win->FromDIP(px_cnt) : px_cnt) + 0.5f);
        return *cache.insert(bmp_name + "-h" + std::to_string(ph) + "-placeholder",
            wxBitmap(missing_resource_placeholder(ph, ph)));
    }
    return *bmp;
}

// ----------------------------------------------------------------------------
// ScalableBitmap
// ----------------------------------------------------------------------------
ScalableBitmap::ScalableBitmap( wxWindow *parent,
                                const std::string& icon_name/* = ""*/,
                                const int px_cnt/* = 16*/,
                                const bool grayscale/* = false*/,
                                const bool resize/* = false*/,
                                const bool bitmap2/* = false*/,
                                const std::vector<std::string>& new_color/* = vector<std::string>*/) :
    m_parent(parent), m_icon_name(icon_name),
    m_px_cnt(px_cnt), m_grayscale(grayscale), m_resize(resize) // BBS: support resize by fill border
{
    m_bmp = create_scaled_bitmap(icon_name, parent, px_cnt, m_grayscale, std::string(), false, resize, bitmap2, new_color);
    if (px_cnt == 0) {
        m_px_cnt = m_bmp.GetHeight(); // scale
        unsigned int height = (unsigned int) (parent->FromDIP(m_px_cnt) + 0.5f);
        if (height != GetBmpHeight())
            msw_rescale();
    }
}

wxSize ScalableBitmap::GetBmpSize() const
{
#ifdef __APPLE__
    return m_bmp.GetScaledSize();
#else
    return m_bmp.GetSize();
#endif
}

int ScalableBitmap::GetBmpWidth() const
{
#ifdef __APPLE__
    return m_bmp.GetScaledWidth();
#else
    return m_bmp.GetWidth();
#endif
}

int ScalableBitmap::GetBmpHeight() const
{
#ifdef __APPLE__
    return m_bmp.GetScaledHeight();
#else
    return m_bmp.GetHeight();
#endif
}


ScalableBitmap::ScalableBitmap(wxWindow *parent, const wxBitmap &bmp)
    : m_parent(parent), m_bmp(bmp), m_px_cnt(bmp.IsOk() ? bmp.GetHeight() : 16)
{
}

void ScalableBitmap::msw_rescale()
{
    if (m_icon_name.empty()) {
        // Bitmap-constructed instance: there is no resource name to reload,
        // and create_scaled_bitmap("") throws. Keep the current bitmap.
        return;
    }
    // BBS: support resize by fill border
    m_bmp = create_scaled_bitmap(m_icon_name, m_parent, m_px_cnt, m_grayscale, std::string(), false, m_resize);
}
