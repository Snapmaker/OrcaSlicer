// This file is part of OrcaSlicer.

#include "GLToolbarBackgroundTextureCache.hpp"

#include "libslic3r/libslic3r.h"
#include "libslic3r/Utils.hpp"

namespace Slic3r {
namespace GUI {

GLToolbarBackgroundTextureCache::GLToolbarBackgroundTextureCache()
{
}

GLToolbarBackgroundTextureCache::~GLToolbarBackgroundTextureCache()
{
    reset();
}

bool GLToolbarBackgroundTextureCache::load(bool is_dark)
{
    if (m_loaded && m_is_dark == is_dark && m_background_texture.get_id() != 0)
        return true;

    reset();

    const std::string filename = is_dark ? "toolbar_background_dark.png" : "toolbar_background.png";
    const std::string path = Slic3r::resources_dir() + "/images/" + filename;
    if (!m_background_texture.load_from_file(path, false, GLTexture::SingleThreaded, false)) {
        m_loaded = false;
        return false;
    }

    m_loaded = true;
    m_is_dark = is_dark;
    return true;
}

void GLToolbarBackgroundTextureCache::reset()
{
    m_background_texture.reset();
    m_loaded = false;
    m_is_dark = false;
}

const GLTexture* GLToolbarBackgroundTextureCache::get_texture() const
{
    return m_loaded && m_background_texture.get_id() != 0 ? &m_background_texture : nullptr;
}

} // namespace GUI
} // namespace Slic3r
