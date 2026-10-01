#include <exception>
#include <vector>

#include "miniz_extension.hpp"
#include "UntrustedInput.hpp"
#include "Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#if defined(_MSC_VER) || defined(__MINGW64__) || defined(_WIN32)
#include "boost/nowide/cstdio.hpp"
#include <boost/nowide/convert.hpp>
#endif

#include "I18N.hpp"

//! macro used to mark string used at localization,
//! return same string
#define L(s) Slic3r::I18N::translate(s)

namespace Slic3r {

namespace {
bool open_zip(mz_zip_archive *zip, const char *fname, bool isread)
{
    if (!zip) return false;
    const char *mode = isread ? "rb" : "wb";

    FILE *f = nullptr;
#if defined(_MSC_VER) || defined(__MINGW64__)
    f = boost::nowide::fopen(fname, mode);
#elif defined(__GNUC__) && defined(_LARGEFILE64_SOURCE)
    f = fopen64(fname, mode);
#else
    f = fopen(fname, mode);
#endif

    if (!f) {
        zip->m_last_error = MZ_ZIP_FILE_OPEN_FAILED;
        return false;
    }

    bool res = false;
    if (isread)
    {
        res = mz_zip_reader_init_cfile(zip, f, 0, 0);
        if (!res)
            // if we get here it means we tried to open a non-zip file
            // we need to close the file here because the call to mz_zip_get_cfile() made into close_zip() returns a null pointer
            // see: https://github.com/prusa3d/PrusaSlicer/issues/3536
            fclose(f);
    }
    else
        res = mz_zip_writer_init_cfile(zip, f, 0);

    return res;
}

bool close_zip(mz_zip_archive *zip, bool isread)
{
    bool ret = false;
    if (zip) {
        FILE *f = mz_zip_get_cfile(zip);
        ret     = bool(isread ? mz_zip_reader_end(zip)
                          : mz_zip_writer_end(zip));
        if (f) fclose(f);
    }
    return ret;
}
}

bool open_zip_reader(mz_zip_archive *zip, const std::string &fname)
{
    return open_zip(zip, fname.c_str(), true);
}

bool open_zip_writer(mz_zip_archive *zip, const std::string &fname)
{
    return open_zip(zip, fname.c_str(), false);
}

bool close_zip_reader(mz_zip_archive *zip) { return close_zip(zip, true); }
bool close_zip_writer(mz_zip_archive *zip) { return close_zip(zip, false); }

bool zip_entry_is_symlink(const mz_zip_archive_file_stat &stat)
{
    const mz_uint32 mode = stat.m_external_attr >> 16;
    return (mode & 0170000u) == 0120000u;
}

namespace {

std::string strip_trailing_separators(std::string name)
{
    while (!name.empty() && (name.back() == '/' || name.back() == '\\'))
        name.pop_back();
    return name;
}

bool extract_one_file(mz_zip_archive &archive, const mz_zip_archive_file_stat &stat, const boost::filesystem::path &full_dest, std::string &err)
{
    namespace fs = boost::filesystem;
    const fs::path parent = full_dest.parent_path();
    if (!parent.empty() && !fs::exists(parent))
        fs::create_directories(parent);
    if (fs::is_symlink(fs::symlink_status(full_dest)))
        fs::remove(full_dest);
    const std::string dest_encoded = encode_path(full_dest.string().c_str());
    mz_bool           res          = mz_zip_reader_extract_to_file(&archive, stat.m_file_index, dest_encoded.c_str(), 0);
#ifdef _WIN32
    if (!res) {
        const std::wstring dest_w = boost::nowide::widen(full_dest.generic_string());
        res                       = mz_zip_reader_extract_to_file_w(&archive, stat.m_file_index, dest_w.c_str(), 0);
    }
#endif
    if (!res) {
        const mz_zip_error zip_err = mz_zip_get_last_error(&archive);
        err = std::string("extract failed: ") + stat.m_filename +
              (zip_err != MZ_ZIP_NO_ERROR ? (std::string(" (") + mz_zip_get_error_string(zip_err) + ")") : std::string());
        return false;
    }
    return true;
}

} // namespace

bool extract_archive_confined(mz_zip_archive &archive, const boost::filesystem::path &dest, std::string &err)
{
    namespace fs = boost::filesystem;
    err.clear();
    try {
        if (!fs::exists(dest))
            fs::create_directories(dest);
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }

    const mz_uint            num_entries = mz_zip_reader_get_num_files(&archive);
    mz_zip_archive_file_stat stat;

    // Pass 1: validate every entry. Any bad entry rejects the whole archive (D3) so a hostile
    // bundle cannot leave a partial install behind.
    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            err = "failed to read archive entry";
            return false;
        }
        if (zip_entry_is_symlink(stat)) {
            err = std::string("symlink entry rejected: ") + stat.m_filename;
            BOOST_LOG_TRIVIAL(error) << "Unzip: " << err;
            return false;
        }
        const std::string name = strip_trailing_separators(stat.m_filename);
        if (name.empty() || !untrusted::is_safe_archive_relative_path(name) || !untrusted::is_path_within_root(dest, dest / name)) {
            err = std::string("entry resolves outside the extraction root: ") + stat.m_filename;
            BOOST_LOG_TRIVIAL(error) << "Unzip: rejecting archive, " << err;
            return false;
        }
    }

    // Pass 2: extract. A symlink already sitting at the destination is replaced, not followed.
    // A mid-extract I/O failure rolls back files written in this call; dest itself is left
    // (it may have pre-existed or hold other files).
    std::vector<fs::path> written;
    auto rollback_written = [&]() {
        boost::system::error_code ec;
        for (const fs::path &p : written)
            fs::remove(p, ec);
    };
    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            rollback_written();
            err = "failed to read archive entry";
            return false;
        }
        const std::string name      = strip_trailing_separators(stat.m_filename);
        const fs::path    full_dest = dest / name;
        try {
            if (stat.m_is_directory) {
                if (!fs::exists(full_dest))
                    fs::create_directories(full_dest);
                continue;
            }
            if (stat.m_uncomp_size == 0) {
                BOOST_LOG_TRIVIAL(warning) << "Unzip: invalid size for file " << stat.m_filename;
                continue;
            }
            if (!extract_one_file(archive, stat, full_dest, err)) {
                rollback_written();
                return false;
            }
            written.push_back(full_dest);
            BOOST_LOG_TRIVIAL(info) << "Unzip: successfully extract file " << stat.m_file_index << " to " << full_dest.string();
        } catch (const std::exception &e) {
            rollback_written();
            err = e.what();
            BOOST_LOG_TRIVIAL(error) << "Unzip: archive read exception: " << err;
            return false;
        }
    }
    return true;
}

bool extract_archive_confined(const boost::filesystem::path &zip_path, const boost::filesystem::path &dest, std::string &err)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, zip_path.string())) {
        err = "unable to open zip reader for " + zip_path.string();
        BOOST_LOG_TRIVIAL(error) << err;
        return false;
    }
    const bool ok = extract_archive_confined(archive, dest, err);
    close_zip_reader(&archive);
    return ok;
}

MZ_Archive::MZ_Archive()
{
    mz_zip_zero_struct(&arch);
}

std::string MZ_Archive::get_errorstr(mz_zip_error mz_err)
{
    switch (mz_err)
    {
    case MZ_ZIP_NO_ERROR:
        return "no error";
    case MZ_ZIP_UNDEFINED_ERROR:
        return L("undefined error");
    case MZ_ZIP_TOO_MANY_FILES:
        return L("too many files");
    case MZ_ZIP_FILE_TOO_LARGE:
        return L("file too large");
    case MZ_ZIP_UNSUPPORTED_METHOD:
        return L("unsupported method");
    case MZ_ZIP_UNSUPPORTED_ENCRYPTION:
        return L("unsupported encryption");
    case MZ_ZIP_UNSUPPORTED_FEATURE:
        return L("unsupported feature");
    case MZ_ZIP_FAILED_FINDING_CENTRAL_DIR:
        return L("failed finding central directory");
    case MZ_ZIP_NOT_AN_ARCHIVE:
        return L("not a ZIP archive");
    case MZ_ZIP_INVALID_HEADER_OR_CORRUPTED:
        return L("invalid header or corrupted");
    case MZ_ZIP_UNSUPPORTED_MULTIDISK:
        return L("unsupported multidisk");
    case MZ_ZIP_DECOMPRESSION_FAILED:
        return L("decompression failed");
    case MZ_ZIP_COMPRESSION_FAILED:
        return L("compression failed");
    case MZ_ZIP_UNEXPECTED_DECOMPRESSED_SIZE:
        return L("unexpected decompressed size");
    case MZ_ZIP_CRC_CHECK_FAILED:
        return L("CRC check failed");
    case MZ_ZIP_UNSUPPORTED_CDIR_SIZE:
        return L("unsupported central directory size");
    case MZ_ZIP_ALLOC_FAILED:
        return L("allocation failed");
    case MZ_ZIP_FILE_OPEN_FAILED:
        return L("file open failed");
    case MZ_ZIP_FILE_CREATE_FAILED:
        return L("file create failed");
    case MZ_ZIP_FILE_WRITE_FAILED:
        return L("file write failed");
    case MZ_ZIP_FILE_READ_FAILED:
        return L("file read failed");
    case MZ_ZIP_FILE_CLOSE_FAILED:
        return L("file close failed");
    case MZ_ZIP_FILE_SEEK_FAILED:
        return L("file seek failed");
    case MZ_ZIP_FILE_STAT_FAILED:
        return L("file stat failed");
    case MZ_ZIP_INVALID_PARAMETER:
        return L("invalid parameter");
    case MZ_ZIP_INVALID_FILENAME:
        return L("invalid filename");
    case MZ_ZIP_BUF_TOO_SMALL:
        return L("buffer too small");
    case MZ_ZIP_INTERNAL_ERROR:
        return L("internal error");
    case MZ_ZIP_FILE_NOT_FOUND:
        return L("file not found");
    case MZ_ZIP_ARCHIVE_TOO_LARGE:
        return L("archive too large");
    case MZ_ZIP_VALIDATION_FAILED:
        return L("validation failed");
    case MZ_ZIP_WRITE_CALLBACK_FAILED:
        return L("write callback failed");
    default:
        break;
    }

    return "unknown error";
}

} // namespace Slic3r
