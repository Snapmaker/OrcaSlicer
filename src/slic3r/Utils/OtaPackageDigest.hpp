#pragma once

// Snapmaker Orca: checks a downloaded OTA package against "file_sha256" / "file_md5" from
// version.json before install. GUI-free (OpenSSL EVP + boost::filesystem) for the unit tests.

#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {
namespace OtaPackageDigest {

enum class Algorithm { MD5, SHA256 };

// Lower-case hexadecimal digest of a regular file. Empty when the file cannot be read or the
// digest cannot be computed.
std::string file_hex_digest(const boost::filesystem::path& file, Algorithm algorithm);

// True when `expected` (as sent by the server: any letter case, surrounding white space allowed)
// names the same digest as `actual`. An empty `actual` never matches.
bool digests_equal(const std::string& expected, const std::string& actual);

struct Verdict
{
    bool        ok = true;
    // Empty when ok. Otherwise one line for the log, naming the algorithm and both values.
    std::string reason;
};

// Checks `file` against the digests the server announced. An empty (or blank) expected value is
// not checked, so a server that sends neither field keeps the behaviour without the check. Every
// non-empty value has to match; SHA-256 is checked first.
Verdict verify_file(const boost::filesystem::path& file, const std::string& expected_sha256, const std::string& expected_md5);

} // namespace OtaPackageDigest
} // namespace Slic3r
