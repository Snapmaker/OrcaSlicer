// This file is part of OrcaSlicer.

#ifndef slic3r_GLToolbarBackgroundTextureCache_hpp_
#define slic3r_GLToolbarBackgroundTextureCache_hpp_

#include "GLTexture.hpp"

namespace Slic3r {
namespace GUI {

class GLToolbarBackgroundTextureCache
{
public:
    GLToolbarBackgroundTextureCache();
    ~GLToolbarBackgroundTextureCache();

    bool load(bool is_dark);
    void reset();

    const GLTexture* get_texture() const;

private:
    GLTexture m_background_texture;
    bool m_loaded = false;
    bool m_is_dark = false;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLToolbarBackgroundTextureCache_hpp_
