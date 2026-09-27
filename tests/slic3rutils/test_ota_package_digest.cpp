#include <catch2/catch_test_macros.hpp>

// Snapmaker Orca: the digest check a downloaded Snapmaker OTA profile package has to pass before
// PresetUpdater renames, extracts and installs it.
#include "slic3r/Utils/OtaPackageDigest.hpp"

#include <string>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

using namespace Slic3r;
using OtaPackageDigest::Algorithm;

namespace {

// RFC 1321 / FIPS 180-2 test vectors for the three bytes "abc" and for no bytes at all.
const char* const ABC_SHA256   = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
const char* const ABC_MD5      = "900150983cd24fb0d6963f7d28e17f72";
const char* const EMPTY_SHA256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";
const char* const EMPTY_MD5    = "d41d8cd98f00b204e9800998ecf8427e";

struct TempFile
{
    boost::filesystem::path path;

    explicit TempFile(const std::string& content)
        : path(boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("ota_digest_%%%%-%%%%-%%%%.zip.tmp"))
    {
        boost::nowide::ofstream out(path.string(), std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    ~TempFile()
    {
        boost::system::error_code ec;
        boost::filesystem::remove(path, ec);
    }
};

} // namespace

TEST_CASE("OTA package digest: known vectors", "[OtaPackageDigest]")
{
    const TempFile abc("abc");
    CHECK(OtaPackageDigest::file_hex_digest(abc.path, Algorithm::SHA256) == ABC_SHA256);
    CHECK(OtaPackageDigest::file_hex_digest(abc.path, Algorithm::MD5) == ABC_MD5);

    const TempFile empty("");
    CHECK(OtaPackageDigest::file_hex_digest(empty.path, Algorithm::SHA256) == EMPTY_SHA256);
    CHECK(OtaPackageDigest::file_hex_digest(empty.path, Algorithm::MD5) == EMPTY_MD5);
}

TEST_CASE("OTA package digest: a file larger than the read buffer", "[OtaPackageDigest]")
{
    // FIPS 180-2: one million times 'a'.
    const TempFile million(std::string(1000000, 'a'));
    CHECK(OtaPackageDigest::file_hex_digest(million.path, Algorithm::SHA256) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    CHECK(OtaPackageDigest::file_hex_digest(million.path, Algorithm::MD5) == "7707d6ae4e027c70eea2a935c2296f21");
}

TEST_CASE("OTA package digest: a missing file has no digest", "[OtaPackageDigest]")
{
    const boost::filesystem::path missing =
        boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("ota_digest_missing_%%%%-%%%%");
    CHECK(OtaPackageDigest::file_hex_digest(missing, Algorithm::SHA256).empty());
    CHECK(OtaPackageDigest::file_hex_digest(missing, Algorithm::MD5).empty());

    // A digest was announced, so the package that cannot be read is rejected.
    const OtaPackageDigest::Verdict verdict = OtaPackageDigest::verify_file(missing, ABC_SHA256, "");
    CHECK_FALSE(verdict.ok);
    CHECK(verdict.reason.find("SHA-256") != std::string::npos);

    // Nothing announced, nothing checked.
    CHECK(OtaPackageDigest::verify_file(missing, "", "").ok);
}

TEST_CASE("OTA package digest: comparison ignores letter case and surrounding white space", "[OtaPackageDigest]")
{
    CHECK(OtaPackageDigest::digests_equal("900150983CD24FB0D6963F7D28E17F72", ABC_MD5));
    CHECK(OtaPackageDigest::digests_equal("  900150983cd24fb0d6963f7d28e17f72\n", ABC_MD5));
    CHECK_FALSE(OtaPackageDigest::digests_equal("900150983cd24fb0d6963f7d28e17f73", ABC_MD5));
    CHECK_FALSE(OtaPackageDigest::digests_equal("", ""));
    CHECK_FALSE(OtaPackageDigest::digests_equal(ABC_MD5, ""));
}

TEST_CASE("OTA package digest: verify_file", "[OtaPackageDigest]")
{
    const TempFile abc("abc");

    SECTION("empty fields keep the unchecked behaviour") {
        const OtaPackageDigest::Verdict verdict = OtaPackageDigest::verify_file(abc.path, "", "  ");
        CHECK(verdict.ok);
        CHECK(verdict.reason.empty());
    }
    SECTION("both digests match, upper case from the server") {
        CHECK(OtaPackageDigest::verify_file(abc.path, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", ABC_MD5).ok);
    }
    SECTION("one field alone is enough") {
        CHECK(OtaPackageDigest::verify_file(abc.path, ABC_SHA256, "").ok);
        CHECK(OtaPackageDigest::verify_file(abc.path, "", ABC_MD5).ok);
    }
    SECTION("a wrong SHA-256 rejects the package and names both values") {
        const OtaPackageDigest::Verdict verdict = OtaPackageDigest::verify_file(abc.path, EMPTY_SHA256, ABC_MD5);
        CHECK_FALSE(verdict.ok);
        CHECK(verdict.reason.find("SHA-256 mismatch") != std::string::npos);
        CHECK(verdict.reason.find(EMPTY_SHA256) != std::string::npos);
        CHECK(verdict.reason.find(ABC_SHA256) != std::string::npos);
    }
    SECTION("a wrong MD5 rejects the package although the SHA-256 matches") {
        const OtaPackageDigest::Verdict verdict = OtaPackageDigest::verify_file(abc.path, ABC_SHA256, EMPTY_MD5);
        CHECK_FALSE(verdict.ok);
        CHECK(verdict.reason.find("MD5 mismatch") != std::string::npos);
    }
    SECTION("a changed byte is noticed") {
        const TempFile changed("abd");
        CHECK_FALSE(OtaPackageDigest::verify_file(changed.path, ABC_SHA256, "").ok);
        CHECK_FALSE(OtaPackageDigest::verify_file(changed.path, "", ABC_MD5).ok);
    }
}
