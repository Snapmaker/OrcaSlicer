cmake_minimum_required(VERSION 3.0)
project(GLEW)

find_package(OpenGL REQUIRED)

# GLEW has to load its functions through the same backend wxWidgets creates the GL context with
# (deps/wxWidgets builds wxGLCanvas for GLX, see wxUSE_GLCANVAS_EGL there): the app forces
# GDK_BACKEND=x11, and NVIDIA's EGL cannot present on the X11 windows of wxGLCanvasEGL.
option(GLEW_USE_EGL "Load OpenGL functions through EGL instead of GLX" OFF)

if(GLEW_USE_EGL)
    message(STATUS "building GLEW for EGL (wxWidgets must be built with wxUSE_GLCANVAS_EGL=ON)")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DGLEW_EGL")
else()
    message(STATUS "building GLEW for GLX")
endif()

add_library(GLEW src/glew.c)
target_include_directories(GLEW PRIVATE include/)
target_link_libraries(GLEW PUBLIC OpenGL::GL)

if (NOT BUILD_SHARED_LIBS)
    target_compile_definitions(GLEW PUBLIC GLEW_STATIC)
endif ()

include(GNUInstallDirs)

install( 
    FILES
        ${PROJECT_SOURCE_DIR}/include/GL/glew.h
        ${PROJECT_SOURCE_DIR}/include/GL/wglew.h
        ${PROJECT_SOURCE_DIR}/include/GL/glxew.h
        ${PROJECT_SOURCE_DIR}/include/GL/eglew.h
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/GL
)

install(TARGETS GLEW GLEW
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} 
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)