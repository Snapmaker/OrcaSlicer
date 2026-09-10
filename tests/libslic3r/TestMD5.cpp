#include <catch2/catch_test_macros.hpp>
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <wchar.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <stdexcept>

using namespace Slic3r;

namespace {
class TemporaryFile
{
public:
    explicit TemporaryFile(const std::string& content)
        : m_path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("orca_md5_test_%%%%-%%%%-%%%%"))
    {
        create_exclusively(content);
    }

    ~TemporaryFile()
    {
        boost::system::error_code error_code;
        boost::filesystem::remove(m_path, error_code);
    }

    const boost::filesystem::path& path() const { return m_path; }

private:
    void create_exclusively(const std::string& content)
    {
#if defined(_WIN32)
        const int descriptor = _wopen(m_path.wstring().c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        const int descriptor = open(m_path.string().c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, S_IRUSR | S_IWUSR);
#endif
        if (descriptor < 0)
            throw std::runtime_error("Unable to create an exclusive temporary file");

        std::size_t written = 0;
        while (written < content.size()) {
#if defined(_WIN32)
            const int count = _write(descriptor, content.data() + written, static_cast<unsigned int>(content.size() - written));
#else
            const ssize_t count = write(descriptor, content.data() + written, content.size() - written);
#endif
            if (count <= 0) {
                close_file(descriptor);
                throw std::runtime_error("Unable to write an exclusive temporary file");
            }
            written += static_cast<std::size_t>(count);
        }

        close_file(descriptor);
    }

    static void close_file(const int descriptor)
    {
#if defined(_WIN32)
        _close(descriptor);
#else
        close(descriptor);
#endif
    }

    boost::filesystem::path m_path;
};

struct TemporaryDirectoryGuard
{
    boost::filesystem::path path;
    ~TemporaryDirectoryGuard()
    {
        boost::system::error_code error_code;
        boost::filesystem::remove(path, error_code);
    }
};
} // namespace

TEST_CASE("bbl_calc_md5 produces correct hash for known content", "[MD5]")
{
    TemporaryFile tmpfile("Hello, OpenSSL 3.x!");

    std::string md5_out;
    REQUIRE(bbl_calc_md5(tmpfile.path().string(), md5_out));
    REQUIRE(md5_out == "5712B8DE6F872E19818AE5032B73D0A3");
}

TEST_CASE("bbl_calc_md5 hashes across buffer boundaries", "[MD5]")
{
    TemporaryFile exact_buffer(std::string(64 * 1024, 'a'));
    TemporaryFile one_past_buffer(std::string(64 * 1024 + 1, 'a'));

    std::string exact_buffer_md5;
    std::string one_past_buffer_md5;
    REQUIRE(bbl_calc_md5(exact_buffer.path().string(), exact_buffer_md5));
    REQUIRE(bbl_calc_md5(one_past_buffer.path().string(), one_past_buffer_md5));
    REQUIRE(exact_buffer_md5 == "2D61AA54B58C2E94403FB092C3DBC027");
    REQUIRE(one_past_buffer_md5 == "B3C6FC238E908636E53AABD5AD830CF7");
}

TEST_CASE("bbl_calc_md5 handles empty file", "[MD5]")
{
    TemporaryFile tmpfile("");

    std::string md5_out;
    REQUIRE(bbl_calc_md5(tmpfile.path().string(), md5_out));
    REQUIRE(md5_out == "D41D8CD98F00B204E9800998ECF8427E");
}

TEST_CASE("bbl_calc_md5 rejects an unreadable file", "[MD5]")
{
    const boost::filesystem::path directory_path = boost::filesystem::temp_directory_path() /
                                                   boost::filesystem::unique_path("orca_md5_directory_%%%%-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directory(directory_path));
    TemporaryDirectoryGuard directory_guard{directory_path};

    std::string md5_out;
    md5_out = "STALE_MD5_VALUE";
    REQUIRE_FALSE(bbl_calc_md5(directory_path.string(), md5_out));
    REQUIRE(md5_out.empty());
}

TEST_CASE("bbl_calc_md5 returns false for nonexistent file", "[MD5]")
{
    std::string md5_out;
    REQUIRE_FALSE(bbl_calc_md5(std::string("/nonexistent/file/path"), md5_out));
}
