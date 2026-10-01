#ifndef MINIZ_EXTENSION_HPP
#define MINIZ_EXTENSION_HPP

#include <string>
#include <miniz.h>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

bool open_zip_reader(mz_zip_archive *zip, const std::string &fname_utf8);
bool open_zip_writer(mz_zip_archive *zip, const std::string &fname_utf8);
bool close_zip_reader(mz_zip_archive *zip);
bool close_zip_writer(mz_zip_archive *zip);

// Unix symlink bit in the zip central-directory external attributes (high 16 bits).
bool zip_entry_is_symlink(const mz_zip_archive_file_stat &stat);

// Extracts every entry of an already-open reader under dest. Validates every entry first
// (is_safe_archive_relative_path + is_path_within_root, no symlink entries). Any bad entry
// rejects the whole archive and writes nothing (Orca #15957 / D3). Files are staged to a
// sibling .part and renamed over the dest only after every entry has been staged, so a
// pass-2 failure never deletes or truncates a pre-existing dest. A dest-file symlink is
// replaced by that rename rather than written through.
bool extract_archive_confined(mz_zip_archive &archive, const boost::filesystem::path &dest, std::string &err);
bool extract_archive_confined(const boost::filesystem::path &zip_path, const boost::filesystem::path &dest, std::string &err);

class MZ_Archive {
public:
    mz_zip_archive arch;
    
    MZ_Archive();
    
    static std::string get_errorstr(mz_zip_error mz_err);
    
    std::string get_errorstr() const
    {
        return get_errorstr(arch.m_last_error) + "!";
    }

    bool is_alive() const
    {
        return arch.m_zip_mode != MZ_ZIP_MODE_WRITING_HAS_BEEN_FINALIZED;
    }
};

} // namespace Slic3r

#endif // MINIZ_EXTENSION_HPP
