#!/bin/sh
# Shared environment for packaged Linux launchers and source builds.
# Keep user-provided certificate settings; source builds may use an OpenSSL
# prefix that does not contain the distribution's CA bundle.
if [ -z "${SSL_CERT_FILE:-}" ] && [ -f /etc/ssl/cert.pem ]; then
    export SSL_CERT_FILE=/etc/ssl/cert.pem
fi

# Flutter expects a browser language tag rather than the POSIX C locale.
case "${LC_ALL:-${LC_MESSAGES:-${LANG:-C}}}" in
    C|C.*|POSIX) export LC_ALL=en_US.UTF-8 ;;
esac

# Explicit fallback for blank model canvases or NVIDIA EGL crashes. This is
# intentionally opt-in: the OpenGL 2.1 path disables newer rendering features.
if [ "${ORCA_LINUX_RENDER_COMPAT:-0}" = 1 ]; then
    export __GLX_VENDOR_LIBRARY_NAME=mesa
    export __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json
    export MESA_LOADER_DRIVER_OVERRIDE=zink
    export GALLIUM_DRIVER=zink
    export MESA_GL_VERSION_OVERRIDE=2.1
    export GDK_BACKEND=x11
    export WEBKIT_DISABLE_DMABUF_RENDERER=1
fi
