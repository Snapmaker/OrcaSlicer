#pragma once

class wxWindow;

namespace Slic3r::GUI {

// Separate from MixedFilamentBatchDialog, which is the Color Mixing Match tool.
void show_mixed_filament_manager(wxWindow* parent);

} // namespace Slic3r::GUI
