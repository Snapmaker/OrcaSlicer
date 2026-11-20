# widgets library: low-dependency custom widgets (migrated from
# slic3r/GUI/Widgets/) plus files newly created by the extraction
# (the UiAppearance look-and-feel seam, the DpiAware dialog template).
# Consumers: libslic3r_gui links this target; an out-of-tree widget lab can
# link it as well. The legacy include path slic3r/GUI/Widgets/<Name>.hpp is
# kept working by forwarding headers at the original locations.
# The PCH matches libslic3r_gui (FORCEINCLUDE) so migrated widget sources do
# not fail to compile over missing implicit includes.
set(WIDGETS_SOURCES
    DpiAware.hpp
    UiAppearance.cpp
    UiAppearance.hpp
    BitmapCache.cpp
    BitmapCache.hpp
    ScalableBitmap.cpp
    ScalableBitmap.hpp
    StateColor.cpp
    StateColor.hpp
    StateHandler.cpp
    StateHandler.hpp
    StaticBox.cpp
    StaticBox.hpp
    Label.cpp
    Label.hpp
    RoundedRectangle.cpp
    RoundedRectangle.hpp
    PopupWindow.cpp
    PopupWindow.hpp
    ErrorMsgStaticText.cpp
    ErrorMsgStaticText.hpp
    AxisCtrlButton.cpp
    AxisCtrlButton.hpp
    Button.cpp
    Button.hpp
    CheckBox.cpp
    CheckBox.hpp
    RadioBox.cpp
    RadioBox.hpp
    ProgressBar.cpp
    ProgressBar.hpp
    StepCtrl.cpp
    StepCtrl.hpp
    TextInput.cpp
    TextInput.hpp
    TextCtrl.h
    Scrollbar.cpp
    Scrollbar.hpp
    ScrolledWindow.cpp
    ScrolledWindow.hpp
    SideButton.cpp
    SideButton.hpp
    SegmentedToggle.cpp
    SegmentedToggle.hpp
    StaticLine.cpp
    StaticLine.hpp
    TempInput.cpp
    TempInput.hpp
    LabeledStaticBox.cpp
    LabeledStaticBox.hpp
    DialogButtons.cpp
    DialogButtons.hpp
    ImageSwitchButton.cpp
    ImageSwitchButton.hpp
    SwitchButton.cpp
    SwitchButton.hpp
    RadioGroup.cpp
    RadioGroup.hpp
    SpinInput.cpp
    SpinInput.hpp
    TabCtrl.cpp
    TabCtrl.hpp
    DropDown.cpp
    DropDown.hpp
    ComboBox.cpp
    ComboBox.hpp
    I18N.cpp
    I18N.hpp
)
if(APPLE)
    list(APPEND WIDGETS_SOURCES MacUtils.mm MacUtils.hpp)
endif()
add_library(libslic3r_widgets STATIC ${WIDGETS_SOURCES})
target_include_directories(libslic3r_widgets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(libslic3r_widgets libslic3r ${wxWidgets_LIBRARIES})
if (SLIC3R_PCH AND NOT SLIC3R_SYNTAXONLY)
    add_precompiled_header(libslic3r_widgets ../slic3r/pchheader.hpp FORCEINCLUDE)
endif ()
encoding_check(libslic3r_widgets)
if(UNIX AND NOT APPLE)
    # DropDown.cpp includes gtk/gtk.h under __WXGTK__. Mirror the GUI lib
    # GTK3 setup (src/slic3r/CMakeLists.txt); PRIVATE keeps gtk out of the
    # widgets public interface.
    find_package(GTK${SLIC3R_GTK} REQUIRED)
    target_include_directories(libslic3r_widgets SYSTEM PRIVATE ${GTK${SLIC3R_GTK}_INCLUDE_DIRS})
endif()
