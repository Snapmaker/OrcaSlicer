// Mac-specific helpers shared by the widgets library. Lives here (not in
// slic3r/Utils) so libslic3r_widgets stays free of GUI-library dependencies
// - widget_lab links only widgets + libslic3r.
#ifndef slic3r_widgets_MacUtils_hpp_
#define slic3r_widgets_MacUtils_hpp_

namespace Slic3r {
namespace GUI {

#if __APPLE__
// Highest backing scale factor across the attached screens.
extern double mac_max_scaling_factor();
#endif

}
}

#endif // slic3r_widgets_MacUtils_hpp_
