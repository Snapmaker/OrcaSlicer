#ifndef GL_GIZMO_UTIL_HPP
#define GL_GIZMO_UTIL_HPP

// Shared ImGui helpers for gizmo panels. Backported subset of OrcaSlicer's
// src/slic3r/GUI/Gizmos/GLGizmoUtils (OrcaSlicer #16076, used by the texture displacement gizmo);
// only the accent-button style is taken. AGPL-3.0, as the rest of the tree.

namespace Slic3r::GUI::GLGizmoUtils {

// Pushes the filled accent ("Orca green") button style: COL_ORCA background, a lighter hover and a
// darker active variant, window-background text, no frame border. Pair with pop_orca_button_style().
void push_orca_button_style();

void pop_orca_button_style();

} // namespace Slic3r::GUI::GLGizmoUtils

#endif // GL_GIZMO_UTIL_HPP
