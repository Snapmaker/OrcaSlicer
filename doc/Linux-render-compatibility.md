# Linux startup and rendering compatibility

The packaged Linux launchers and `scripts/run-local-linux.sh` share
`resources/linux-launch-env.sh`. It supplies the distribution CA bundle when
available and not already configured, and replaces the POSIX C locale with
`en_US.UTF-8` because Flutter requires a browser language tag.

## Blank model canvas or NVIDIA EGL crashes

An optional compatibility mode uses Mesa/Zink over Vulkan, the OpenGL 2.1
rendering path, X11 (XWayland in a Wayland session), and disables WebKit's
DMA-BUF renderer. Install Mesa, its Zink driver, a working Vulkan driver, and
XWayland before enabling it. On Arch Linux the Mesa EGL vendor descriptor is
`/usr/share/glvnd/egl_vendor.d/50_mesa.json`; other distributions must provide
that descriptor as well for this mode.

For a packaged build:

```sh
ORCA_LINUX_RENDER_COMPAT=1 ./Snapmaker_Orca.AppImage
```

For a source build in the usual `build/` directory:

```sh
ORCA_LINUX_RENDER_COMPAT=1 ./scripts/run-local-linux.sh project.3mf
```

Leave the variable unset to retain the normal rendering path and the existing
automatic NVIDIA/Wayland Zink detection in the packaged launchers. Compatibility
mode is a workaround, not a root-cause fix for the modern OpenGL path. It may
reduce performance or disable features requiring newer OpenGL versions.

## Validation and limits

Observed with Snapmaker Orca 2.4.1 (`c0e6987`), Arch Linux, wxGTK3,
WebKitGTK 4.1, NVIDIA RTX 3090 / driver 610.57.04, and Mesa 26.2.2:

- WebKitWebProcess crashed with SIGSEGV inside `libnvidia-eglcore`.
- Default OpenGL contexts produced a blank model canvas with both Zink and
  software llvmpipe, while the native toolbar remained visible.
- OpenGL 2.1 displayed model geometry and all seven plates of the same 3MF with
  both llvmpipe and Zink. The Zink configuration was then checked using the normal
  user profile.
- Model visibility was checked; sliced toolpaths, printing, other GPUs, and
  other distributions were not validated.

The portable launcher changes accompany GTK popup lifetime fixes, no-result
WebKit script execution, correct validation of flat printer metadata, and
materialized Eigen casts in integer-coordinate line-radius queries.

## Focused regression tests

```sh
cmake -S tests/crash_regressions -B regression-build \
  -DCMAKE_PREFIX_PATH=/path/to/dependency/prefix
cmake --build regression-build
ctest --test-dir regression-build --output-on-failure
```

This suite contains three metadata-validation cases and two line-radius cases.
The metadata cases are also registered with the main `tests/slic3rutils` suite.
Catch2 is fetched by the repository's existing CMake helper; a local Catch2
checkout may be passed as `FETCHCONTENT_SOURCE_DIR_CATCH2` for offline builds.

Two standalone GUI smoke programs in `tests/crash_regressions` exercise popup
destruction and WebKit native-object completion. They require a graphical
session and can be built with the dependency prefix's `wx-config` and system
WebKitGTK:

```sh
c++ -std=c++17 -Isrc $(wx-config --cxxflags) \
  tests/crash_regressions/PopupLifecycle_smoke.cpp \
  src/slic3r/GUI/Widgets/PopupWindow.cpp $(wx-config --libs) -o popup-smoke
./popup-smoke
c++ tests/crash_regressions/WebKitCompletion_smoke.cpp \
  $(pkg-config --cflags --libs webkit2gtk-4.1) -o webkit-smoke
ORCA_LINUX_RENDER_COMPAT=1 sh -c \
  '. ./resources/linux-launch-env.sh; exec ./webkit-smoke'
```

The WebKit smoke compares the unsupported native-object completion with a
discarded completion value. It exercises the API behavior, not the complete
application bridge or the driver's crash path.
