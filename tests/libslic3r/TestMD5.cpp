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

namespace {
std::string to_hex(const std::array<unsigned char, 32>& digest)
{
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned char byte : digest) {
        out.push_back(hex[byte >> 4]);
        out.push_back(hex[byte & 0x0F]);
    }
    return out;
}
} // namespace

TEST_CASE("calc_file_sha256 produces the FIPS 180-2 vector for abc", "[SHA256]")
{
    TemporaryFile tmpfile("abc");

    std::array<unsigned char, 32> digest;
    REQUIRE(calc_file_sha256(tmpfile.path().string(), digest));
    REQUIRE(to_hex(digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("calc_file_sha256 hashes across buffer boundaries", "[SHA256]")
{
    TemporaryFile exact_buffer(std::string(64 * 1024, 'a'));
    TemporaryFile one_past_buffer(std::string(64 * 1024 + 1, 'a'));

    std::array<unsigned char, 32> digest;
    REQUIRE(calc_file_sha256(exact_buffer.path().string(), digest));
    REQUIRE(to_hex(digest) == "bf718b6f653bebc184e1479f1935b8da974d701b893afcf49e701f3e2f9f9c5a");
    REQUIRE(calc_file_sha256(one_past_buffer.path().string(), digest));
    REQUIRE(to_hex(digest) == "008ffc88d3c96a9f307524eb361e47c5222a887fc45fa0c1fb8d429c5c23b430");
}

TEST_CASE("calc_file_sha256 handles empty file", "[SHA256]")
{
    TemporaryFile tmpfile("");

    std::array<unsigned char, 32> digest;
    REQUIRE(calc_file_sha256(tmpfile.path().string(), digest));
    REQUIRE(to_hex(digest) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("calc_file_sha256 rejects a directory and a missing file and zeroes the output", "[SHA256]")
{
    const boost::filesystem::path directory_path = boost::filesystem::temp_directory_path() /
                                                   boost::filesystem::unique_path("orca_sha256_directory_%%%%-%%%%-%%%%");
    REQUIRE(boost::filesystem::create_directory(directory_path));
    TemporaryDirectoryGuard directory_guard{directory_path};

    std::array<unsigned char, 32> digest;
    digest.fill(0xAB);
    REQUIRE_FALSE(calc_file_sha256(directory_path.string(), digest));
    REQUIRE(to_hex(digest) == std::string(64, '0'));

    digest.fill(0xAB);
    REQUIRE_FALSE(calc_file_sha256(std::string("/nonexistent/file/path"), digest));
    REQUIRE(to_hex(digest) == std::string(64, '0'));
}
