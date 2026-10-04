#ifndef slic3r_EmbossShape_hpp_
#define slic3r_EmbossShape_hpp_

#include <string>
#include <optional>
#include <memory> // unique_ptr
#include <cereal/cereal.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/archives/binary.hpp>
#include "Point.hpp" // Transform3d
#include "ExPolygon.hpp"
#include "ExPolygonSerialize.hpp"
#include "nanosvg/nanosvg.h" // NSVGimage

namespace Slic3r {

// Curved (arc) text: an in-plane bend of the 2D shape around the emboss direction.
// Applied to the 2D outlines before extrusion, so every volume type and the
// surface-less flat emboss stay unchanged. Kept per volume (3MF + undo), never in style presets.
struct EmbossBend
{
    enum class Mode : unsigned char {
        off    = 0, // straight text (identical to text without bend)
        angle  = 1, // span of the longest line is given by `angle`
        radius = 2  // radius of the reference circle is given by `radius`
    };
    Mode mode = Mode::off;

    // [deg] span of the longest text line, used by Mode::angle; 0 = straight, max 359
    float angle = 0.f;

    // [mm] radius of the circle through the text reference line, used by Mode::radius
    float radius = 0.f;

    // false = arch (text outside the circle, reading clockwise over the top)
    // true  = smile (text inside the circle, reading along the bottom)
    bool inside = false;

    // false = bent: every outline point follows the arc (letters become slight wedges)
    // true  = rigid: each glyph keeps its shape, it is only moved and rotated onto the arc
    bool rigid = false;

    bool is_active() const {
        return (mode == Mode::angle && angle > 0.f) ||
               (mode == Mode::radius && radius > 0.f);
    }

    bool operator==(const EmbossBend &other) const {
        return mode == other.mode && angle == other.angle && radius == other.radius &&
               inside == other.inside && rigid == other.rigid;
    }
    bool operator!=(const EmbossBend &other) const { return !(*this == other); }

    // undo / redo stack recovery
    template<class Archive> void save(Archive &ar) const {
        unsigned char m = static_cast<unsigned char>(mode);
        ar(m, angle, radius, inside, rigid);
    }
    template<class Archive> void load(Archive &ar) {
        unsigned char m = 0;
        ar(m, angle, radius, inside, rigid);
        mode = (m <= static_cast<unsigned char>(Mode::radius)) ? static_cast<Mode>(m) : Mode::off;
    }
};

struct EmbossProjection{
    // Emboss depth, Size in local Z direction
    double depth = 1.; // [in loacal mm]
    // NOTE: User should see and modify mainly world size not local

    // Flag that result volume use surface cutted from source objects
    bool use_surface = false;

    // Curved text (in-plane arc). Default = off = straight text.
    EmbossBend bend;

    bool operator==(const EmbossProjection &other) const {
        return depth == other.depth && use_surface == other.use_surface && bend == other.bend;
    }

    // undo / redo stack recovery
    template<class Archive> void serialize(Archive &ar) { ar(depth, use_surface, bend); }
};

// Extend expolygons with information whether it was successfull healed
struct HealedExPolygons{
    ExPolygons expolygons;
    bool is_healed;
    operator ExPolygons&() { return expolygons; }
};

// Help structure to identify expolygons grups
// e.g. emboss -> per glyph -> identify character
struct ExPolygonsWithId
{ 
    // Identificator for shape
    // In text it separate letters and the name is unicode value of letter
    // Is svg it is id of path
    unsigned id;

    // shape defined by integer point contain only lines
    // Curves are converted to sequence of lines
    ExPolygons expoly;

    // flag whether expolygons are fully healed(without duplication)
    bool is_healed = true;
};
using ExPolygonsWithIds = std::vector<ExPolygonsWithId>;

/// <summary>
/// Contain plane shape information to be able emboss it and edit it
/// </summary>
struct EmbossShape 
{
    // shapes to to emboss separately over surface
    ExPolygonsWithIds shapes_with_ids;

    // Only cache for final shape
    // It is calculated from ExPolygonsWithIds
    // Flag is_healed --> whether union of shapes is healed
    // Healed mean without selfintersection and point duplication
    HealedExPolygons final_shape;

    // scale of shape, multiplier to get 3d point in mm from integer shape
    double scale = SCALING_FACTOR;

    // Define how to emboss shape
    EmbossProjection projection;

    // !!! Volume stored in .3mf has transformed vertices.
    // (baked transformation into vertices position)
    // Only place for fill this is when load from .3mf
    // This is correction for volume transformation
    // Stored_Transform3d * fix_3mf_tr = Transform3d_before_store_to_3mf
    std::optional<Slic3r::Transform3d> fix_3mf_tr;

    struct SvgFile {
        // File(.svg) path on local computer 
        // When empty can't reload from disk
        std::string path;

        // File path into .3mf(.zip)
        // When empty svg is not stored into .3mf file yet.
        // and will create dialog to delete private data on save.
        std::string path_in_3mf;

        // Loaded svg file data.
        // !!! It is not serialized on undo/redo stack 
        std::shared_ptr<NSVGimage> image = nullptr;

        // Loaded string data from file
        std::shared_ptr<std::string> file_data = nullptr;

        template<class Archive> void save(Archive &ar) const {
            // Note: image is only cache it is not neccessary to store

            // Store file data as plain string
            // For Embossed text file_data are nullptr
            ar(path, path_in_3mf, (file_data != nullptr) ? *file_data : std::string(""));
        }
        template<class Archive> void load(Archive &ar) {
            // for restore shared pointer on file data
            std::string file_data_str;
            ar(path, path_in_3mf, file_data_str);
            if (!file_data_str.empty())
                file_data = std::make_unique<std::string>(file_data_str);
        }
    };
    // When embossing shape is made by svg file this is source data
    std::optional<SvgFile> svg_file;

    // undo / redo stack recovery
    template<class Archive> void save(Archive &ar) const
    {
        // final_shape is not neccessary to store - it is only cache
        ar(shapes_with_ids, final_shape, scale, projection, svg_file);
        cereal::save(ar, fix_3mf_tr);
    }
    template<class Archive> void load(Archive &ar)
    {
        ar(shapes_with_ids, final_shape, scale, projection, svg_file);
        cereal::load(ar, fix_3mf_tr);
    }
};
} // namespace Slic3r

// Serialization through the Cereal library
namespace cereal {
template<class Archive> void serialize(Archive &ar, Slic3r::ExPolygonsWithId &o) { ar(o.id, o.expoly, o.is_healed); }
template<class Archive> void serialize(Archive &ar, Slic3r::HealedExPolygons &o) { ar(o.expolygons, o.is_healed); }
}; // namespace cereal

#endif // slic3r_EmbossShape_hpp_
