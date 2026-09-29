#ifndef slic3r_CodeEmboss_hpp_
#define slic3r_CodeEmboss_hpp_

// Emboss QR codes and barcodes as SVG volumes.
//
// A code is split into parts, every part is an independent embossed SVG volume:
//  - Dark  .. dark modules / bars
//  - Light .. background (quiet zone rectangle without dark modules and logo)
//  - Logo  .. optional logo in the middle of QR code
// Parts can be printed by different filaments (multi color) or by different depth (relief).
//
// All parts of one code share identical bounding box center, so volumes with
// the same transformation are aligned (SVG shapes are centered when loaded).
// Parameters of the code are stored inside of the generated SVG data (<metadata>),
// so the code stays editable after the project is saved into .3mf.

#include <optional>
#include <string>
#include <vector>

#include "Barcode.hpp"
#include "ExPolygon.hpp"

namespace Slic3r {

class ModelVolume;
class ModelObject;

enum class CodePartRole : int { Dark = 0, Light, Logo };

// Shape of the area cleared of dark modules around the logo
enum class CodeLogoClear : int { Outline = 0, Square, Circle };

// Outline of the light part (quiet zone around the code)
enum class CodeSurround : int { Square = 0, Rounded, Circle };

struct CodeEmbossParams
{
    Barcode::Symbology symbology = Barcode::Symbology::QR;
    std::string        text;
    Barcode::QrEcc     ecc = Barcode::QrEcc::Medium;

    // Width of one module (narrowest bar) [in mm]
    double module_size = 1.;
    // Height of bars of linear code [in mm]
    double bar_height = 15.;
    // Border around code [in modules], it is part of the light part
    int quiet_zone = 4;
    // Outline of the code with its quiet zone,
    // circle is circumscribed to the rectangle of the code with its quiet zone
    CodeSurround surround = CodeSurround::Square;
    // Only QR with circle surround: fill the space between the quiet zone and
    // the circle by random modules, scanners ignore them (circular QR code look)
    bool decorate = false;

    // Create part with the background (light modules)
    bool   light_part  = true;
    double dark_depth  = 1.; // [in mm]
    double light_depth = 1.; // [in mm]
    double logo_depth  = 1.; // [in mm]

    // Only for QR
    bool          has_logo    = false;
    double        logo_size   = 0.22; // ratio to size of code without quiet zone
    int           logo_margin = 1;    // [in modules] cleared around the logo
    CodeLogoClear logo_clear  = CodeLogoClear::Outline;

    // Identify parts of the same code, unique per code
    std::string group_id;

    bool operator==(const CodeEmbossParams &o) const;
    bool operator!=(const CodeEmbossParams &o) const { return !(*this == o); }
};

struct CodeEmbossPart
{
    CodePartRole role = CodePartRole::Dark;
    // Shape in SVG coordinates (Y down) scaled by SCALING_FACTOR
    ExPolygons shape;
    // Content of SVG file (include metadata)
    std::string svg;
    double      depth = 1.;
};

struct CodeEmbossResult
{
    std::vector<CodeEmbossPart> parts;
    Barcode::Result             code;
    // Empty on success
    std::string error;
    // Non fatal issues e.g. logo is too big for error correction
    std::vector<std::string> warnings;
    // Size of whole code include quiet zone [in mm]
    double width  = 0.;
    double height = 0.;
    // QR modules cleared by logo
    int cleared_modules = 0;
    // Used logo size ratio (can be smaller than wanted)
    double logo_size = 0.;

    bool is_valid() const { return error.empty() && !parts.empty(); }
    const CodeEmbossPart *part(CodePartRole role) const;
};

/// <summary>
/// Create geometry of the code parts
/// </summary>
/// <param name="params">Definition of code</param>
/// <param name="logo">Shape of logo in any scale and position, Y up (as loaded from SVG), could be nullptr</param>
CodeEmbossResult create_code_emboss(const CodeEmbossParams &params, const ExPolygons *logo = nullptr);

/// <summary>
/// Load filled shapes of svg (union of all), usable as logo
/// </summary>
/// <param name="svg_data">Content of svg file</param>
/// <returns>Empty when svg does not contain any shape</returns>
ExPolygons load_code_logo(const std::string &svg_data);

// Metadata stored in SVG of code part
struct CodeEmbossMeta
{
    CodeEmbossParams params;
    CodePartRole     role = CodePartRole::Dark;
};

std::string write_code_emboss_meta(const CodeEmbossMeta &meta);
std::optional<CodeEmbossMeta> read_code_emboss_meta(const std::string &svg_data);
// Read metadata from SVG volume, nullopt when volume is not a code part
std::optional<CodeEmbossMeta> read_code_emboss_meta(const ModelVolume &volume);

// Create new unique identifier of code
std::string create_code_group_id();

// Volumes of object, which are parts of the code with group_id
std::vector<ModelVolume *> get_code_volumes(const ModelObject &object, const std::string &group_id);

// Path of SVG in .3mf archive for code part, unique for each call
std::string code_part_path_in_3mf(const std::string &group_id, CodePartRole role);

const char *to_string(CodePartRole role);
// Translatable name of part(en)
std::string code_part_name(const CodeEmbossParams &params, CodePartRole role);

} // namespace Slic3r

#endif // slic3r_CodeEmboss_hpp_
