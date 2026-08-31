#ifndef _common_func_hppp_
#define _common_func_hppp_
#include <iostream>


// App identity and version macros (SLIC3R_APP_NAME/KEY, SLIC3R_VERSION,
// SoftFever_VERSION, SLIC3R_BUILD_ID, BBL_INTERNAL_TESTING, ...) come from the
// header generated out of version.inc — the single source of truth.
#include "libslic3r_version.h"
#define Snapmaker_VERSION "2.5.0"
#define MIN_FIRM_VER "1.6.0"
#define BBL_RELEASE_TO_PUBLIC 1

namespace common 
{
	std::string get_pc_name();

	std::string get_flutter_version();

	std::string get_profile_version();

	std::string getMachineId();

	std::string getLocalArea();

	std::string getLanguage();

    } // namespace common

#endif