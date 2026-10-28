#pragma once

namespace Slic3r { namespace GUI {

// Copies UninstallWatcher out of the .app and registers one LaunchAgent.
// Calling this from every running instance updates that same job; it does not add another.
void ensure_macos_uninstall_watcher();

}} // namespace Slic3r::GUI
