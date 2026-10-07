#ifndef slic3r_TextureDisplacementGuards_hpp_
#define slic3r_TextureDisplacementGuards_hpp_

// EdgeSlicer: guards around the texture displacement port (OrcaSlicer #14662).
//
// Texture displacement replaces a part's mesh (subdivide, smooth, remesh, bake). Upstream only keeps
// the paint across that; EdgeSlicer parts can carry more than paint that is tied to the old mesh:
//  - editable text (text_configuration) and editable SVG (emboss_shape): the next text/SVG edit
//    rebuilds the mesh from the glyphs/paths and would silently throw the relief away;
//  - an exact CAD body (cad_body, CAD fillet / STEP): it no longer matches the displaced mesh;
//  - a re-editable cut (ModelObject::cut_recipe): re-cutting starts again from the pre-cut mesh.
// The first three are detached when texture displacement changes the mesh (the part becomes a plain
// mesh part, as Color Split and the SVG tool's own "Bake" do); the cut recipe belongs to the object
// and is only warned about.
//
// Texture colour (upstream's colour mode, off by default) writes the part's colour paint with its own
// filament mixing; it does not know EdgeSlicer's mixed (virtual) filaments, so it is not offered
// while any are enabled.

#include <cstddef>

namespace Slic3r {

class ModelVolume;

struct TextureDisplacementMeshRecipes
{
    bool text       = false; // editable text
    bool svg        = false; // editable SVG
    bool cad_body   = false; // exact CAD body that still matches the mesh
    bool cut_recipe = false; // the object can be re-cut from its pre-cut mesh

    // Something a mesh change detaches (and the user should agree to first).
    bool detaches() const { return text || svg || cad_body; }
    bool any() const { return detaches() || cut_recipe; }
};

// What changing `volume`'s mesh would affect.
TextureDisplacementMeshRecipes texture_displacement_mesh_recipes(const ModelVolume &volume);

// Call where texture displacement has replaced (or is about to replace) `volume`'s mesh: clears the
// text configuration and emboss shape (the part becomes a plain, non-regenerable mesh part) and drops
// the CAD body. Paint, settings, name, type and the cut recipe are untouched. Returns what it found.
TextureDisplacementMeshRecipes detach_mesh_recipes_for_texture_displacement(ModelVolume &volume);

// Texture colour is not offered while mixed (virtual) filaments are enabled in the project.
inline bool texture_displacement_colors_allowed(size_t enabled_mixed_filaments) { return enabled_mixed_filaments == 0; }

} // namespace Slic3r

#endif // slic3r_TextureDisplacementGuards_hpp_
