#ifndef slic3r_EmbossInsert_hpp_
#define slic3r_EmbossInsert_hpp_

// "Insert" popup of the Emboss (text) tool: puts a built-in shape, a symbol or a user SVG into the text at
// the caret. Shapes and user SVGs become private-use placeholder characters with an entry in the volume's
// inline-shape table (libslic3r/InlineShapes.hpp); symbols are ordinary characters that the bundled symbol
// font draws when the selected font lacks them (libslic3r/FontFallback.hpp). The user never types syntax.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <imgui/imgui.h>

#include "libslic3r/InlineShapeTable.hpp"

namespace Slic3r {
class Model;
namespace Emboss { struct FontFile; }
} // namespace Slic3r

namespace Slic3r::GUI {

class EmbossInsert
{
public:
    EmbossInsert() = default;
    EmbossInsert(const EmbossInsert &) = delete;
    EmbossInsert &operator=(const EmbossInsert &) = delete;
    ~EmbossInsert();

    // ImGui callback of the text box (ImGuiInputTextFlags_CallbackAlways), user_data = this: remembers the
    // caret and the selection, so an insert from the popup lands there while the box is not active.
    static int text_callback(ImGuiInputTextCallbackData *data);

    // The text was replaced from outside (other volume, new text): caret at its end
    void on_text_reset(const std::string &text);

    struct Context
    {
        std::string                   &text;
        InlineShapeTable              &table;
        // selected font (codes it draws are not used as placeholders); may be null
        const Slic3r::Emboss::FontFile *font = nullptr;
        unsigned int                   font_index = 0;
        // user SVGs already used by text volumes of this project
        const Model                   *model = nullptr;
        float                          gui_scale = 1.f;
    };

    // Draws the "Insert" button and its popup. True when the text (and maybe the table) changed.
    bool draw(Context &ctx);

    // GL textures of the thumbnails; also released when the popup closes
    void release_textures();

private:
    struct Atlas
    {
        GLuint tex  = 0;
        int    cell = 0; // px
        int    cols = 0;
        int    rows = 0;
        int    count = 0;
        // per cell: drawn (false = nothing to show, e.g. an unusable SVG)
        std::vector<bool> drawn;
        ImVec2 uv0(int index) const;
        ImVec2 uv1(int index) const;
    };
    struct UserSvg
    {
        std::string                        name;
        std::shared_ptr<const std::string> data;
    };

    bool insert_text(Context &ctx, const std::string &utf8);
    bool insert_shape(Context &ctx, InlineShape entry);
    bool draw_shapes_tab(Context &ctx);
    bool draw_symbols_tab(Context &ctx);
    bool draw_svgs_tab(Context &ctx);
    void collect_user_svgs(const Context &ctx);

    static void release(Atlas &atlas);

    // caret and selection in bytes of the UTF-8 text (as ImGui reports them)
    size_t m_cursor    = 0;
    size_t m_sel_start = 0;
    size_t m_sel_end   = 0;
    bool   m_know_caret = false;

    bool  m_open = false;
    int   m_cell_px = 0;
    Atlas m_shapes_atlas;
    Atlas m_symbols_atlas;
    Atlas m_svgs_atlas;

    std::vector<UserSvg> m_svgs;         // shown in "My SVGs" (project + loaded in this session)
    std::vector<UserSvg> m_session_svgs; // loaded with "From SVG file..." in this session
    std::string          m_svgs_key;     // what m_svgs_atlas was drawn from

    std::string m_message; // last refusal, shown in the popup
};

} // namespace Slic3r::GUI

#endif // slic3r_EmbossInsert_hpp_
