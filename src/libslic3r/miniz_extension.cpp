#include <algorithm>
#include <exception>
#include <set>
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
// Decodes the Info-ZIP Unicode Path extra field (0x7075) of a non-UTF-8-flagged entry. The field is
// honoured only when its CRC matches the legacy name; otherwise the legacy name is decoded with the
// system code page, as before.
std::string decode_zip_unicode_path_extra_field(const std::string& extra, const std::string& path)
{
    size_t offset = 0;
    const mz_uint32 path_crc = mz_crc32(0, reinterpret_cast<const unsigned char*>(path.data()), path.size());

    while (offset + 4 <= extra.size()) {
        const unsigned char* field = reinterpret_cast<const unsigned char*>(extra.data() + offset);
        const std::uint16_t len = field[2] | (static_cast<std::uint16_t>(field[3]) << 8);
        if (offset + 4 + len > extra.size())
            break;

        if (field[0] == 0x75 && field[1] == 0x70 && len >= 5 && field[4] == 0x01) {
            const mz_uint32 stored_crc =
                static_cast<mz_uint32>(field[5]) |
                (static_cast<mz_uint32>(field[6]) << 8) |
                (static_cast<mz_uint32>(field[7]) << 16) |
                (static_cast<mz_uint32>(field[8]) << 24);
            if (stored_crc == path_crc)
                return std::string(extra.data() + offset + 9, extra.data() + offset + 4 + len);
        }

        offset += 4 + len;
    }

    return Slic3r::decode_path(path.c_str());
}

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

std::string decode_archive_entry_path(mz_zip_archive *zip, const mz_zip_archive_file_stat &stat)
{
    if (stat.m_is_utf8)
        return stat.m_filename;

    std::string extra(1024, 0);
    const size_t extra_size = mz_zip_reader_get_extra(zip, stat.m_file_index, extra.data(), extra.size());
    return decode_zip_unicode_path_extra_field(extra.substr(0, extra_size > 0 ? extra_size - 1 : 0), stat.m_filename);
}

bool zip_entry_is_symlink(const mz_zip_archive_file_stat &stat)
{
    const mz_uint32 mode = stat.m_external_attr >> 16;
    return (mode & 0170000u) == 0120000u;
}

bool extract_entry_to_file(mz_zip_archive &archive, mz_uint file_index, const boost::filesystem::path &dest_path)
{
#ifdef _WIN32
    // path::c_str() is a wchar_t* here. No narrowing: the ANSI code page maps U+FF0E / U+FF0F
    // (and other look-alikes) to '.' and '/', which would defeat every check done on the UTF-8 name.
    return mz_zip_reader_extract_to_file_w(&archive, file_index, dest_path.c_str(), 0) != MZ_FALSE;
#else
    return mz_zip_reader_extract_to_file(&archive, file_index, dest_path.string().c_str(), 0) != MZ_FALSE;
#endif
}

bool extract_entry_to_file(mz_zip_archive &archive, mz_uint file_index, const std::string &dest_path_utf8)
{
#ifdef _WIN32
    const std::wstring dest_w = boost::nowide::widen(dest_path_utf8);
    return mz_zip_reader_extract_to_file_w(&archive, file_index, dest_w.c_str(), 0) != MZ_FALSE;
#else
    return mz_zip_reader_extract_to_file(&archive, file_index, dest_path_utf8.c_str(), 0) != MZ_FALSE;
#endif
}

namespace {
struct CappedSink
{
    std::string  *out;
    std::uint64_t cap;
    bool          overflow = false;
};

size_t capped_sink_write(void *opaque, mz_uint64 file_ofs, const void *data, size_t n)
{
    CappedSink *sink = static_cast<CappedSink *>(opaque);
    // Entries are written sequentially; anything else is a corrupt stream.
    if (file_ofs != sink->out->size() || n > sink->cap - std::min<std::uint64_t>(sink->cap, sink->out->size())) {
        sink->overflow = true;
        return 0; // a short write makes miniz abort the extraction
    }
    sink->out->append(static_cast<const char *>(data), n);
    return n;
}
} // namespace

bool read_zip_entry_capped(mz_zip_archive &archive, mz_uint file_index, std::uint64_t cap, std::string &out, std::string *why)
{
    out.clear();
    auto fail = [&](const char *reason) {
        out.clear();
        out.shrink_to_fit();
        if (why != nullptr)
            *why = reason;
        return false;
    };
    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(&archive, file_index, &stat))
        return fail("unreadable entry header");
    if (stat.m_is_directory || stat.m_uncomp_size == 0)
        return true;
    if (stat.m_uncomp_size > cap)
        return fail("entry is larger than the allowed size");
    CappedSink sink{&out, stat.m_uncomp_size}; // stop at the declared size (already within cap)
    out.reserve(static_cast<size_t>(stat.m_uncomp_size)); // the header was just checked against cap
    if (!mz_zip_reader_extract_to_callback(&archive, file_index, capped_sink_write, &sink, 0))
        return fail(sink.overflow ? "entry inflates beyond its declared size" : "entry cannot be inflated");
    if (out.size() != stat.m_uncomp_size)
        return fail("entry size does not match its header");
    return true;
}

namespace {

// An archive entry name is UTF-8. On Windows it is widened explicitly, so the path does not
// depend on whether boost::filesystem has the nowide locale installed (the app installs it,
// a unit test may not).
boost::filesystem::path entry_path(const std::string &name_utf8)
{
#ifdef _WIN32
    return boost::filesystem::path(boost::nowide::widen(name_utf8));
#else
    return boost::filesystem::path(name_utf8);
#endif
}

// UTF-8 text of a path for logs and error messages (path::string() would go through the
// narrow locale, which can throw for characters the code page does not have).
std::string path_utf8(const boost::filesystem::path &p)
{
#ifdef _WIN32
    return boost::nowide::narrow(p.native());
#else
    return p.string();
#endif
}

boost::filesystem::path normalize_dir_path(boost::filesystem::path p)
{
    // Native path only: a generic_string() round-trip is a narrow conversion on Windows.
    // lexically_normal first so "cache/./" becomes a trailing-separator form, then strip
    // that empty (or ".") filename. Do not strip the root ("/", "C:\\"): "C:\\" -> "C:"
    // is a different path (the current directory on that drive).
    p = p.lexically_normal();
    while (!p.empty() && p != p.root_path() && (p.filename().empty() || p.filename() == ".")) {
        const boost::filesystem::path parent = p.parent_path();
        if (parent.empty() || parent == p)
            break;
        p = parent;
    }
    return p;
}

bool leaf_exists(const boost::filesystem::path &p)
{
    boost::system::error_code ec;
    const auto                st = boost::filesystem::symlink_status(p, ec);
    return !ec && boost::filesystem::exists(st);
}

bool is_dir_or_dir_symlink(const boost::filesystem::path &p)
{
    boost::system::error_code ec;
    const auto                st = boost::filesystem::symlink_status(p, ec);
    if (ec || !boost::filesystem::exists(st))
        return false;
    if (boost::filesystem::is_directory(st))
        return true;
    if (boost::filesystem::is_symlink(st))
        return boost::filesystem::is_directory(p, ec);
    return false;
}

// Create missing directories from root toward dir. A regular file already sitting
// on that path (the "blocker" case) is a failure, not an overwrite.
bool ensure_dirs(const boost::filesystem::path &root,
                 const boost::filesystem::path &dir,
                 std::vector<boost::filesystem::path> &created,
                 std::string                         &err)
{
    namespace fs = boost::filesystem;
    const fs::path root_n = normalize_dir_path(root);
    const fs::path dir_n  = normalize_dir_path(dir);
    if (dir_n.empty())
        return true;
    std::vector<fs::path> chain;
    for (fs::path p = dir_n;; p = p.parent_path()) {
        chain.push_back(p);
        if (p == root_n || p.parent_path() == p || p.empty())
            break;
    }
    std::reverse(chain.begin(), chain.end());
    for (const fs::path &p : chain) {
        if (is_dir_or_dir_symlink(p))
            continue;
        if (leaf_exists(p)) {
            err = path_utf8(p) + " is not a directory";
            return false;
        }
        boost::system::error_code ec;
        fs::create_directory(p, ec);
        if (ec) {
            err = "create directory failed: " + path_utf8(p) + " (" + ec.message() + ")";
            return false;
        }
        created.push_back(p);
    }
    return true;
}

bool extract_to_path(mz_zip_archive &archive, const mz_zip_archive_file_stat &stat, const boost::filesystem::path &path, std::string &err)
{
    // Wide API only on Windows (see extract_entry_to_file): no narrow attempt first, no fallback.
    const bool res = extract_entry_to_file(archive, stat.m_file_index, path);
    if (!res) {
        const mz_zip_error zip_err = mz_zip_get_last_error(&archive);
        err = std::string("extract failed: ") + stat.m_filename +
              (zip_err != MZ_ZIP_NO_ERROR ? (std::string(" (") + mz_zip_get_error_string(zip_err) + ")") : std::string());
        return false;
    }
    return true;
}

boost::filesystem::path make_part_path(const boost::filesystem::path              &full_dest,
                                       const std::set<boost::filesystem::path>    &reserved)
{
    namespace fs = boost::filesystem;
    // Always a random suffix so an archive that itself contains "x.json.part" cannot steal
    // the staging name of "x.json" (plain dest+".part" collided with that dest).
    for (int attempt = 0; attempt < 64; ++attempt) {
        fs::path unique = full_dest;
        unique += ".part.%%%%%%%%";
        unique = fs::unique_path(unique);
        if (!leaf_exists(unique) && reserved.find(unique) == reserved.end())
            return unique;
    }
    fs::path unique = full_dest;
    unique += ".part.%%%%%%%%";
    return fs::unique_path(unique);
}

bool commit_part(const boost::filesystem::path &part, const boost::filesystem::path &dest, std::string &err)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    // boost::filesystem::rename replaces an existing file (Windows: MoveFileEx REPLACE).
    // A dest-file symlink is replaced rather than followed.
    fs::rename(part, dest, ec);
    if (ec) {
        err = "rename failed: " + path_utf8(part) + " -> " + path_utf8(dest) + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

struct StagedFile
{
    boost::filesystem::path dest;
    boost::filesystem::path part;
    bool                    dest_existed = false;
};

void rollback_extract(std::vector<StagedFile>                 &staged,
                      std::vector<boost::filesystem::path>    &created_dirs)
{
    namespace fs = boost::filesystem;
    boost::system::error_code ec;
    for (const StagedFile &s : staged) {
        if (!s.part.empty())
            fs::remove(s.part, ec);
        // A dest that did not exist before this call and was already committed (rename
        // succeeded) is ours to remove. A pre-existing dest is never deleted.
        if (!s.dest_existed)
            fs::remove(s.dest, ec);
    }
    std::sort(created_dirs.begin(), created_dirs.end(), [](const fs::path &a, const fs::path &b) {
        return a.generic_string().size() > b.generic_string().size();
    });
    for (const fs::path &d : created_dirs)
        fs::remove(d, ec);
}

} // namespace

bool extract_archive_confined(mz_zip_archive &archive, const boost::filesystem::path &dest, std::string &err)
{
    namespace fs = boost::filesystem;
    err.clear();
    fs::path              dest_root;
    std::vector<fs::path> created_dirs;
    try {
        dest_root = normalize_dir_path(dest);
        if (!is_dir_or_dir_symlink(dest_root)) {
            if (leaf_exists(dest_root)) {
                err = path_utf8(dest_root) + " is not a directory";
                return false;
            }
            boost::system::error_code ec;
            fs::create_directories(dest_root, ec);
            if (ec) {
                err = "create directory failed: " + path_utf8(dest_root) + " (" + ec.message() + ")";
                return false;
            }
            created_dirs.push_back(dest_root);
        }
    } catch (const std::exception &e) {
        err = e.what();
        BOOST_LOG_TRIVIAL(error) << "Unzip: dest path: " << err;
        return false;
    }

    const mz_uint            num_entries = mz_zip_reader_get_num_files(&archive);
    mz_zip_archive_file_stat stat;
    std::vector<StagedFile>  staged;
    std::set<fs::path>       reserved;
    auto                     fail = [&]() {
        rollback_extract(staged, created_dirs);
        return false;
    };

    // Pass 1: validate every entry. Any bad entry rejects the whole archive (D3) so a hostile
    // bundle cannot leave a partial install behind. Dest names are reserved so a staging
    // filename cannot collide with another entry (x.json.part vs x.json).
    // Duplicate names (two entries, or "a\\b" and "a/b", that end up as the same path) are not
    // an error: each is staged to its own .part file and committed in archive order, so the
    // LAST entry in the archive wins, deterministically.
    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            err = "failed to read archive entry";
            return fail();
        }
        if (zip_entry_is_symlink(stat)) {
            err = std::string("symlink entry rejected: ") + stat.m_filename;
            BOOST_LOG_TRIVIAL(error) << "Unzip: " << err;
            return fail();
        }
        // Backslash separators, "./" prefixes, "a//b" and a trailing separator are normalised
        // first; the result is then judged as strictly as ever. A bare "./" entry has nothing to
        // extract. Two entries that normalise to the same name follow the duplicate rule below.
        std::string name;
        const untrusted::ArchiveEntryName verdict = untrusted::normalize_archive_entry_path(stat.m_filename, name);
        if (verdict == untrusted::ArchiveEntryName::Skip)
            continue;
        if (verdict == untrusted::ArchiveEntryName::Reject ||
            !untrusted::is_path_within_root(dest_root, dest_root / entry_path(name))) {
            err = std::string("entry resolves outside the extraction root: ") + stat.m_filename;
            BOOST_LOG_TRIVIAL(error) << "Unzip: rejecting archive, " << err;
            return fail();
        }
        reserved.insert(dest_root / entry_path(name));
    }

    // Pass 2: stage every file to a unique sibling .part.%%%%%%%%. Dest files are not opened,
    // truncated, or replaced until every entry has been staged. A dest-file symlink is left
    // alone here.
    for (mz_uint i = 0; i < num_entries; ++i) {
        if (!mz_zip_reader_file_stat(&archive, i, &stat)) {
            err = "failed to read archive entry";
            return fail();
        }
        std::string name;
        if (untrusted::normalize_archive_entry_path(stat.m_filename, name) != untrusted::ArchiveEntryName::Ok)
            continue; // Skip: validated as such in pass 1 (a Reject never gets here)
        const fs::path full_dest = dest_root / entry_path(name);
        try {
            if (stat.m_is_directory) {
                if (!ensure_dirs(dest_root, full_dest, created_dirs, err))
                    return fail();
                continue;
            }
            if (stat.m_uncomp_size == 0) {
                BOOST_LOG_TRIVIAL(warning) << "Unzip: invalid size for file " << stat.m_filename;
                continue;
            }
            const fs::path parent = full_dest.parent_path();
            if (!parent.empty() && !ensure_dirs(dest_root, parent, created_dirs, err))
                return fail();
            const fs::path part = make_part_path(full_dest, reserved);
            if (!untrusted::is_path_within_root(dest_root, part)) {
                err = "part path resolves outside the extraction root: " + path_utf8(part);
                return fail();
            }
            reserved.insert(part);
            staged.push_back({full_dest, part, leaf_exists(full_dest)});
            if (!extract_to_path(archive, stat, part, err))
                return fail();
            BOOST_LOG_TRIVIAL(info) << "Unzip: staged file " << stat.m_file_index << " to " << path_utf8(part);
        } catch (const std::exception &e) {
            err = e.what();
            BOOST_LOG_TRIVIAL(error) << "Unzip: archive read exception: " << err;
            return fail();
        }
    }

    // Pass 3: rename each part over its dest. Only now is a pre-existing dest replaced.
    // A failure here can leave a mix of old and new files (already-renamed dests stay;
    // not-yet-renamed dests keep their previous content). Nothing is truncated or deleted,
    // but the result is not atomic across files.
    for (StagedFile &s : staged) {
        if (!commit_part(s.part, s.dest, err))
            return fail();
        s.part.clear();
        BOOST_LOG_TRIVIAL(info) << "Unzip: committed " << path_utf8(s.dest);
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
