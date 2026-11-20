#include "MacUtils.hpp"

#if __APPLE__
#import <AppKit/AppKit.h>
#include <wx/statbox.h>

#include <algorithm>

namespace Slic3r {
namespace GUI {

void staticbox_remove_margin(wxStaticBox* sb)
{
    NSBox* nativeBox = static_cast<NSBox*>(sb->GetHandle());
    [nativeBox setBoxType:NSBoxCustom];
    [nativeBox setBorderWidth:0];
}

double mac_max_scaling_factor()
{
    double scaling = 1.;
    if ([NSScreen screens] == nil) {
        scaling = [[NSScreen mainScreen] backingScaleFactor];
    } else {
	    for (int i = 0; i < [[NSScreen screens] count]; ++ i)
	    	scaling = std::max<double>(scaling, [[[NSScreen screens] objectAtIndex:i] backingScaleFactor]);
	}
    return scaling;
}

}
}
#endif // __APPLE__
