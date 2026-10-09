#ifndef slic3r_PrinterMetadataValidation_hpp_
#define slic3r_PrinterMetadataValidation_hpp_

#include <boost/filesystem.hpp>

namespace Slic3r {

// resources/printers contains flat model metadata, not preset bundles.
inline bool validate_printer_metadata_directory(const boost::filesystem::path& directory)
{
    namespace fs = boost::filesystem;
    if (!fs::is_directory(directory) || !fs::is_regular_file(directory / "version.txt"))
        return false;

    for (const auto& entry : fs::directory_iterator(directory)) {
        if (fs::is_regular_file(entry.path()) && entry.path().extension() == ".json")
            return true;
    }
    return false;
}

} // namespace Slic3r

#endif
