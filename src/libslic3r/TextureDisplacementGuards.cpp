#include "TextureDisplacementGuards.hpp"

#include "Model.hpp"
#include "BRep/CadEdit.hpp"

namespace Slic3r {

TextureDisplacementMeshRecipes texture_displacement_mesh_recipes(const ModelVolume &volume)
{
    TextureDisplacementMeshRecipes out;
    out.text     = volume.is_text();
    out.svg      = volume.is_svg();
    out.cad_body = BRep::attached_cad_body(volume) != nullptr;
    if (const ModelObject *object = volume.get_object(); object != nullptr)
        out.cut_recipe = object->has_cut_recipe();
    return out;
}

TextureDisplacementMeshRecipes detach_mesh_recipes_for_texture_displacement(ModelVolume &volume)
{
    TextureDisplacementMeshRecipes found = texture_displacement_mesh_recipes(volume);
    // Raw members, not the is_*() views: a stale body (fingerprint mismatch) or an emboss shape left
    // behind on a text volume must not ride along into the 3MF either.
    volume.text_configuration.reset();
    volume.emboss_shape.reset();
    volume.cad_body.reset();
    return found;
}

} // namespace Slic3r
