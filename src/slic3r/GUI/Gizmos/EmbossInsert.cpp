#include "EmbossInsert.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cmath>
#include <map>
#include <set>

#include <imgui/imgui_internal.h> // InputTextState: forget the text box's undo after an insert

#include <wx/filedlg.h>

#include <boost/filesystem.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/log/trivial.hpp>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/Emboss.hpp"
#include "libslic3r/FontFallback.hpp"
#include "libslic3r/InlineShapes.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/UntrustedInput.hpp"

#include "slic3r/GUI/3DScene.hpp" // glsafe
#include "slic3r/GUI/GUI.hpp"     // show_error
#include "slic3r/GUI/GUI_App.hpp" // file_wildcards
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/format.hpp"

namespace Slic3r::GUI {

namespace emboss_insert_detail {

// One thumbnail: an outline and how it maps into its cell
struct Thumb
{
    ExPolygons shape;
    Vec2d      origin = Vec2d::Zero();
    double     scale  = 0.;
};

// Fits the outline into a cell: centred, as large as possible, but never larger than max_scale (so a
// bullet stays small next to a star when both come from one font)
Thumb fit(ExPolygons shape, int cell, double max_scale = 0.)
{
    Thumb t;
    t.shape = std::move(shape);
    if (t.shape.empty())
        return t;
    const BoundingBox bb   = get_extents(t.shape);
    const double      size = double(std::max(bb.size().x(), bb.size().y()));
    if (!(size > 0.))
        return t;
    const double pad = std::max(2., cell * 0.12);
    t.scale          = (cell - 2. * pad) / size;
    if (max_scale > 0.)
        t.scale = std::min(t.scale, max_scale);
    const Vec2d c = bb.center().cast<double>();
    t.origin      = Vec2d(c.x() - cell / 2. / t.scale, c.y() + cell / 2. / t.scale);
    return t;
}

// White pixels with the coverage as alpha, so ImGui tints them with the text colour
GLuint upload_rgba(const std::vector<unsigned char> &rgba, int width, int height)
{
    GLint last_texture = 0;
    glsafe(::glGetIntegerv(GL_TEXTURE_BINDING_2D, &last_texture));
    GLuint tex = 0;
    glsafe(::glGenTextures(1, &tex));
    glsafe(::glBindTexture(GL_TEXTURE_2D, tex));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    glsafe(::glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    glsafe(::glPixelStorei(GL_UNPACK_ALIGNMENT, 4));
    glsafe(::glPixelStorei(GL_UNPACK_ROW_LENGTH, 0));
    glsafe(::glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data()));
    glsafe(::glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(last_texture)));
    return tex;
}

std::string utf8_of(uint32_t cp) { return codepoints_to_utf8({cp}); }

std::string hex_code(uint32_t cp)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "U+%04X", static_cast<unsigned>(cp));
    return buf;
}

// Translated titles of symbol_picker_groups(), same order
std::vector<std::string> symbol_group_titles()
{
    return {_u8L("Bullets and marks"), _u8L("Stars"),   _u8L("Hearts and cards"), _u8L("Geometric"),
            _u8L("Arrows"),            _u8L("Weather and nature"), _u8L("Objects"), _u8L("Signs")};
}

std::string shape_category_title(const std::string &category)
{
    if (category == "geometric") return _u8L("Geometric");
    if (category == "marks") return _u8L("Marks");
    if (category == "arrows") return _u8L("Arrows");
    if (category == "nature") return _u8L("Nature");
    if (category == "symbols") return _u8L("Symbols");
    if (category == "objects") return _u8L("Objects");
    if (category.empty()) return _u8L("Shapes");
    std::string t = category;
    t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

std::string choose_svg_file_for_text()
{
    wxString     wildcard = file_wildcards(FT_SVG);
    wxFileDialog dialog(nullptr, _L("Choose an SVG file to insert into the text:"), wxEmptyString, wxEmptyString, wildcard,
                        wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK)
        return {};
    std::string path = into_u8(dialog.GetPath());
    if (path.empty() || !boost::filesystem::exists(path))
        return {};
    return path;
}

} // namespace emboss_insert_detail

using namespace emboss_insert_detail;

ImVec2 EmbossInsert::Atlas::uv0(int index) const
{
    const float w = float(cols * cell), h = float(rows * cell);
    return ImVec2(float((index % cols) * cell) / w, float((index / cols) * cell) / h);
}
ImVec2 EmbossInsert::Atlas::uv1(int index) const
{
    const float w = float(cols * cell), h = float(rows * cell);
    return ImVec2(float((index % cols + 1) * cell) / w, float((index / cols + 1) * cell) / h);
}

EmbossInsert::~EmbossInsert()
{
    // textures are released when the popup closes; a texture still alive here belongs to a context that
    // is going away with the canvas
}

void EmbossInsert::release(Atlas &atlas)
{
    if (atlas.tex != 0)
        glsafe(::glDeleteTextures(1, &atlas.tex));
    atlas = Atlas{};
}

void EmbossInsert::release_textures()
{
    release(m_shapes_atlas);
    release(m_symbols_atlas);
    release(m_svgs_atlas);
    m_svgs_key.clear();
}

int EmbossInsert::text_callback(ImGuiInputTextCallbackData *data)
{
    if (data == nullptr || data->UserData == nullptr)
        return 0;
    EmbossInsert *self = static_cast<EmbossInsert *>(data->UserData);
    const int     a = std::max(0, std::min(data->SelectionStart, data->SelectionEnd));
    const int     b = std::max(0, std::max(data->SelectionStart, data->SelectionEnd));
    self->m_cursor     = static_cast<size_t>(std::max(0, data->CursorPos));
    self->m_sel_start  = static_cast<size_t>(a);
    self->m_sel_end    = static_cast<size_t>(b);
    self->m_know_caret = true;
    return 0;
}

void EmbossInsert::on_text_reset(const std::string &text)
{
    m_cursor = m_sel_start = m_sel_end = text.size();
    m_know_caret = false;
}

bool EmbossInsert::insert_text(Context &ctx, const std::string &utf8)
{
    if (utf8.empty())
        return false;
    size_t a = m_sel_start, b = m_sel_end;
    if (!m_know_caret || a > ctx.text.size() || b > ctx.text.size())
        a = b = ctx.text.size();
    const size_t caret = insert_utf8_at(ctx.text, a, b, utf8);
    m_cursor = m_sel_start = m_sel_end = caret;
    m_know_caret = true;

    // The text box keeps its own copy and undo stack while it is not active and reuses them when it
    // is clicked again; drop them so it starts from the new text (its undo cannot point past the end).
    if (ImGuiContext *g = ImGui::GetCurrentContext(); g != nullptr && g->ActiveId != g->InputTextState.ID)
        g->InputTextState.ID = 0;
    return true;
}

bool EmbossInsert::insert_shape(Context &ctx, InlineShape entry)
{
    if (count_inline_placeholders(ctx.text) >= InlineShapeLimits::max_placeholders_per_text) {
        m_message = GUI::format(_u8L("A text can hold at most %1% shapes."), InlineShapeLimits::max_placeholders_per_text);
        return false;
    }
    CodePointPredicate font_maps;
    if (ctx.font != nullptr)
        font_maps = make_font_coverage(*ctx.font, ctx.font_index);
    std::optional<uint16_t> code = add_inline_shape(ctx.table, std::move(entry), font_maps);
    if (!code.has_value()) {
        m_message = GUI::format(_u8L("A text can use at most %1% different shapes."), InlineShapeLimits::max_shapes_per_text);
        return false;
    }
    m_message.clear();
    return insert_text(ctx, utf8_of(*code));
}

bool EmbossInsert::draw(Context &ctx)
{
    bool changed = false;
    const char *popup_id = "##emboss_insert_popup";
    m_cell_px = std::max(24, static_cast<int>(std::lround(36.f * ctx.gui_scale)));

    if (ImGui::Button((_u8L("Insert") + "...").c_str())) {
        m_message.clear();
        ImGui::OpenPopup(popup_id);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("Insert a shape, a symbol or an SVG at the cursor of the text.").c_str());

    if (ImGui::BeginPopup(popup_id)) {
        m_open = true;
        if (ImGui::BeginTabBar("##emboss_insert_tabs")) {
            if (ImGui::BeginTabItem(_u8L("Shapes").c_str())) {
                changed |= draw_shapes_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(_u8L("Symbols").c_str())) {
                changed |= draw_symbols_tab(ctx);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem(_u8L("My SVGs").c_str())) {
                changed |= draw_svgs_tab(ctx);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!m_message.empty())
            ImGui::TextColored(ImGuiWrapper::COL_ORANGE_LIGHT, "%s", m_message.c_str());
        else
            ImGui::TextDisabled("%s", _u8L("Shift + click inserts more than one.").c_str());
        if (changed && !ImGui::GetIO().KeyShift)
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    } else if (m_open) {
        m_open = false;
        release_textures();
    }
    return changed;
}

namespace emboss_insert_detail {
// builds an atlas texture of the thumbnails (cells in rows of `cols`)
void build_atlas(GLuint &tex, int &cell_out, int &cols_out, int &rows_out, int &count_out, std::vector<bool> &drawn,
                 const std::vector<Thumb> &thumbs, int cell, int cols)
{
    const int count = static_cast<int>(thumbs.size());
    cols            = std::max(1, std::min(cols, count));
    const int rows  = std::max(1, (count + cols - 1) / cols);
    const int w = cols * cell, h = rows * cell;
    std::vector<unsigned char> rgba(size_t(w) * size_t(h) * 4, 0);
    drawn.assign(size_t(count), false);
    for (int i = 0; i < count; ++i) {
        const Thumb &t = thumbs[size_t(i)];
        if (t.shape.empty() || !(t.scale > 0.))
            continue;
        std::vector<uint8_t> alpha = rasterize_shape(t.shape, cell, cell, t.origin, t.scale);
        if (alpha.size() != size_t(cell) * size_t(cell))
            continue;
        drawn[size_t(i)] = true;
        const int x0 = (i % cols) * cell, y0 = (i / cols) * cell;
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x) {
                unsigned char *dst = &rgba[(size_t(y0 + y) * size_t(w) + size_t(x0 + x)) * 4];
                dst[0] = dst[1] = dst[2] = 255;
                dst[3] = alpha[size_t(y) * cell + x];
            }
    }
    tex       = upload_rgba(rgba, w, h);
    cell_out  = cell;
    cols_out  = cols;
    rows_out  = rows;
    count_out = count;
}

// a thumbnail button; true when clicked
bool thumb_button(GLuint tex, const ImVec2 &uv0, const ImVec2 &uv1, float size, int id, bool drawn)
{
    ImGui::PushID(id);
    const ImVec4 tint = drawn ? ImGui::GetStyleColorVec4(ImGuiCol_Text) : ImVec4(0.5f, 0.5f, 0.5f, 0.3f);
    const bool clicked = ImGui::ImageButton((ImTextureID) (intptr_t) tex, ImVec2(size, size), uv0, uv1, 2, ImVec4(0, 0, 0, 0), tint);
    ImGui::PopID();
    return clicked;
}
} // namespace emboss_insert_detail

bool EmbossInsert::draw_shapes_tab(Context &ctx)
{
    const BuiltinShapeLibrary &lib = BuiltinShapeLibrary::instance();
    if (!lib.loaded() || lib.shapes().empty()) {
        ImGui::TextColored(ImGuiWrapper::COL_ORANGE_LIGHT, "%s", _u8L("The shape library is missing from the installation.").c_str());
        return false;
    }
    const std::vector<BuiltinShapeInfo> &shapes = lib.shapes();
    const int cols = 8;
    if (m_shapes_atlas.tex == 0) {
        std::vector<Thumb> thumbs;
        thumbs.reserve(shapes.size());
        for (const BuiltinShapeInfo &info : shapes) {
            std::shared_ptr<const InlineUnitShape> unit = lib.unit_shape(info.id);
            thumbs.push_back(unit ? fit(unit->shape, m_cell_px) : Thumb{});
        }
        build_atlas(m_shapes_atlas.tex, m_shapes_atlas.cell, m_shapes_atlas.cols, m_shapes_atlas.rows, m_shapes_atlas.count,
                    m_shapes_atlas.drawn, thumbs, m_cell_px, cols);
    }

    bool        changed = false;
    std::string category;
    int         in_row  = 0;
    for (size_t i = 0; i < shapes.size(); ++i) {
        const BuiltinShapeInfo &info = shapes[i];
        if (i == 0 || info.category != category) {
            category = info.category;
            ImGui::TextDisabled("%s", shape_category_title(category).c_str());
            in_row = 0;
        }
        if (in_row > 0)
            ImGui::SameLine();
        in_row = (in_row + 1) % cols;
        const int idx = static_cast<int>(i);
        if (thumb_button(m_shapes_atlas.tex, m_shapes_atlas.uv0(idx), m_shapes_atlas.uv1(idx), float(m_cell_px), idx,
                         idx < int(m_shapes_atlas.drawn.size()) && m_shapes_atlas.drawn[size_t(idx)])) {
            if (std::optional<InlineShape> entry = lib.make_entry(info.id))
                changed |= insert_shape(ctx, std::move(*entry));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", info.name.c_str());
    }
    return changed;
}

bool EmbossInsert::draw_symbols_tab(Context &ctx)
{
    std::shared_ptr<const Slic3r::Emboss::FontFile> symbol_font = bundled_symbol_font();
    if (symbol_font == nullptr) {
        ImGui::TextColored(ImGuiWrapper::COL_ORANGE_LIGHT, "%s", _u8L("The symbol font is missing from the installation.").c_str());
        return false;
    }
    const std::vector<SymbolGroup> &groups = symbol_picker_groups();
    const int cols = 10;
    if (m_symbols_atlas.tex == 0) {
        std::vector<Thumb> thumbs;
        const Slic3r::Emboss::FontFile::Info &info = symbol_font->infos.front();
        // all symbols on one em scale: shape units of letter2glyph are font units / 0.001
        FontProp unit_prop;
        unit_prop.size_in_mm      = static_cast<float>(info.unit_per_em);
        const double shape_per_unit = 1. / Slic3r::Emboss::get_text_shape_scale(unit_prop, *symbol_font);
        const double em_shape       = double(info.ascent - info.descent) * shape_per_unit;
        const double max_scale      = em_shape > 0. ? m_cell_px * 0.95 / em_shape : 0.;
        for (const SymbolGroup &g : groups)
            for (uint32_t cp : g.code_points) {
                std::optional<Slic3r::Emboss::Glyph> glyph = Slic3r::Emboss::letter2glyph(*symbol_font, 0, static_cast<int>(cp), 0.5f);
                thumbs.push_back(glyph ? fit(glyph->shape, m_cell_px, max_scale) : Thumb{});
            }
        build_atlas(m_symbols_atlas.tex, m_symbols_atlas.cell, m_symbols_atlas.cols, m_symbols_atlas.rows, m_symbols_atlas.count,
                    m_symbols_atlas.drawn, thumbs, m_cell_px, cols);
    }

    GlyphCoverage primary;
    if (ctx.font != nullptr)
        primary = make_font_coverage(*ctx.font, ctx.font_index);
    const std::vector<std::string> titles = symbol_group_titles();
    bool changed = false;
    int  idx     = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi) {
        const SymbolGroup &g = groups[gi];
        ImGui::TextDisabled("%s", (gi < titles.size() && titles.size() == groups.size() ? titles[gi] : std::string(g.name)).c_str());
        int in_row = 0;
        for (uint32_t cp : g.code_points) {
            if (in_row > 0)
                ImGui::SameLine();
            in_row = (in_row + 1) % cols;
            if (thumb_button(m_symbols_atlas.tex, m_symbols_atlas.uv0(idx), m_symbols_atlas.uv1(idx), float(m_cell_px), 1000 + idx,
                             idx < int(m_symbols_atlas.drawn.size()) && m_symbols_atlas.drawn[size_t(idx)])) {
                // symbols are ordinary characters (drawn by the bundled font when the selected one lacks them)
                changed |= insert_text(ctx, utf8_of(cp));
                m_message.clear();
            }
            if (ImGui::IsItemHovered()) {
                std::string tip = hex_code(cp);
                if (primary && !primary(cp))
                    tip += "\n" + _u8L("The selected font lacks it: drawn with the bundled symbol font.");
                ImGui::SetTooltip("%s", tip.c_str());
            }
            ++idx;
        }
    }
    return changed;
}

void EmbossInsert::collect_user_svgs(const Context &ctx)
{
    std::vector<UserSvg>                       list;
    std::map<std::pair<size_t, std::string>, bool> seen; // (size, entry name from data) -> listed
    auto add = [&list, &seen](const std::string &name, const std::shared_ptr<const std::string> &data) {
        if (data == nullptr || data->empty())
            return;
        auto key = std::make_pair(data->size(), inline_svg_entry_name(*data));
        if (seen.count(key) != 0)
            return;
        seen[key] = true;
        list.push_back({name, data});
    };
    for (const InlineShape &s : ctx.table)
        if (s.source == InlineShapeSource::Svg)
            add(s.id, s.svg_data);
    if (ctx.model != nullptr)
        for (const ModelObject *object : ctx.model->objects)
            for (const ModelVolume *volume : object->volumes)
                if (volume->text_configuration.has_value())
                    for (const InlineShape &s : volume->text_configuration->inline_shapes)
                        if (s.source == InlineShapeSource::Svg)
                            add(s.id, s.svg_data);
    for (const UserSvg &s : m_session_svgs)
        add(s.name, s.data);
    m_svgs = std::move(list);
}

bool EmbossInsert::draw_svgs_tab(Context &ctx)
{
    bool changed = false;
    if (ImGui::Button((_u8L("From SVG file") + "...").c_str())) {
        ImGui::CloseCurrentPopup();
        const std::string path = choose_svg_file_for_text();
        if (!path.empty()) {
            std::string error;
            bool        too_large = false;
            std::optional<InlineShape> shape = load_user_inline_svg(path, &error, &too_large);
            const std::string file_name = boost::filesystem::path(path).filename().string();
            if (!shape.has_value()) {
                if (too_large) {
                    const double limit_mb = double(untrusted::SVG_SIZE_LIMIT) / (1024. * 1024.);
                    show_error(nullptr, GUI::format(_u8L("The SVG file is too large to be inserted into the text (limit %1% MB) (%2%)."),
                                                    limit_mb, file_name));
                } else {
                    show_error(nullptr, GUI::format(_u8L("The SVG file cannot be inserted into the text (%1%): %2%."), file_name, error));
                }
                BOOST_LOG_TRIVIAL(warning) << "Inline SVG refused: " << error;
            } else {
                m_session_svgs.push_back({shape->id, shape->svg_data});
                changed |= insert_shape(ctx, std::move(*shape));
            }
        }
        return changed;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", _u8L("The SVG is embedded into the project (not its location on disk). "
                                     "Colours are ignored: the drawing becomes one solid silhouette.").c_str());

    collect_user_svgs(ctx);
    if (m_svgs.empty()) {
        ImGui::TextDisabled("%s", _u8L("SVGs used in this project are listed here.").c_str());
        return changed;
    }
    std::string key;
    for (const UserSvg &s : m_svgs)
        key += inline_svg_entry_name(*s.data) + ";";
    if (key != m_svgs_key) {
        release(m_svgs_atlas);
        std::vector<Thumb> thumbs;
        for (const UserSvg &s : m_svgs) {
            std::optional<InlineUnitShape> unit = load_inline_svg(*s.data, InlineBoxMode::InkBox);
            thumbs.push_back(unit ? fit(unit->shape, m_cell_px) : Thumb{});
        }
        build_atlas(m_svgs_atlas.tex, m_svgs_atlas.cell, m_svgs_atlas.cols, m_svgs_atlas.rows, m_svgs_atlas.count, m_svgs_atlas.drawn,
                    thumbs, m_cell_px, 8);
        m_svgs_key = key;
    }
    ImGui::TextDisabled("%s", _u8L("Used in this project").c_str());
    const int cols = std::max(1, m_svgs_atlas.cols);
    for (size_t i = 0; i < m_svgs.size(); ++i) {
        const int idx = static_cast<int>(i);
        if (idx % cols != 0)
            ImGui::SameLine();
        const bool drawn = idx < int(m_svgs_atlas.drawn.size()) && m_svgs_atlas.drawn[size_t(idx)];
        if (thumb_button(m_svgs_atlas.tex, m_svgs_atlas.uv0(idx), m_svgs_atlas.uv1(idx), float(m_cell_px), 5000 + idx, drawn) && drawn) {
            InlineShape entry;
            entry.source      = InlineShapeSource::Svg;
            entry.id          = m_svgs[i].name;
            entry.svg_data    = m_svgs[i].data;
            entry.path_in_3mf = inline_svg_entry_name(*entry.svg_data);
            changed |= insert_shape(ctx, std::move(entry));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", m_svgs[i].name.c_str());
    }
    return changed;
}

} // namespace Slic3r::GUI
