#ifndef MINIZ_EXTENSION_HPP
#define MINIZ_EXTENSION_HPP

#include <cstdint>
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
// unique sibling (dest + ".part." + random) and renamed over the dest only after every entry
// has been staged, so a pass-2 failure never deletes or truncates a pre-existing dest. A
// dest-file symlink is replaced by that rename rather than written through. A failure partway
// through the commit (rename) phase can leave a mix of old and new files: nothing is truncated
// or deleted, but the result is not atomic across files.
bool extract_archive_confined(mz_zip_archive &archive, const boost::filesystem::path &dest, std::string &err);
bool extract_archive_confined(const boost::filesystem::path &zip_path, const boost::filesystem::path &dest, std::string &err);

// Extracts one archive entry to dest_path. On Windows the file is opened through the wide API
// (_wfopen): the path never goes through the ANSI code page, whose best-fit mapping turns
// fullwidth look-alikes (U+FF0E U+FF0E U+FF0F) into "../". Every extraction of an untrusted
// entry name must use this rather than encode_path() + mz_zip_reader_extract_to_file().
// The std::string overload takes a UTF-8 path.
bool extract_entry_to_file(mz_zip_archive &archive, mz_uint file_index, const boost::filesystem::path &dest_path);
bool extract_entry_to_file(mz_zip_archive &archive, mz_uint file_index, const std::string &dest_path_utf8);

// Reads one archive entry of a file we did not write into `out`, never trusting its header: the
// declared uncompressed size is compared with `cap` before anything is allocated, and the data
// is inflated through a sink that refuses to grow past `cap`, so an entry whose header lies
// about its size, or a zip bomb, cannot take more than `cap` bytes. Returns false (and `out`
// empty) when the entry is too large, cannot be inflated, or its size or CRC disagree with its
// header. `*why` gets a short reason. A zero-length entry yields true and an empty `out`.
bool read_zip_entry_capped(mz_zip_archive &archive, mz_uint file_index, std::uint64_t cap, std::string &out, std::string *why = nullptr);

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
