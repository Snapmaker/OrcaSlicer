#include "OtaPackageDigest.hpp"

#include <cstddef>
#include <ios>
#include <memory>
#include <utility>
#include <vector>

#include <boost/algorithm/string/case_conv.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>

#include <openssl/evp.h>

namespace Slic3r {
namespace OtaPackageDigest {

namespace {

std::string normalized(const std::string& digest)
{
    std::string out = boost::algorithm::trim_copy(digest);
    boost::algorithm::to_lower(out);
    return out;
}

const char* algorithm_name(Algorithm algorithm)
{
    return algorithm == Algorithm::SHA256 ? "SHA-256" : "MD5";
}

} // namespace

std::string file_hex_digest(const boost::filesystem::path& file, Algorithm algorithm)
{
    boost::system::error_code ec;
    if (!boost::filesystem::is_regular_file(file, ec) || ec)
        return {};

    // The EVP interface: the one-shot MD5_* / SHA256_* functions are deprecated since OpenSSL 3.0.
    const EVP_MD* md = algorithm == Algorithm::SHA256 ? EVP_sha256() : EVP_md5();
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (md == nullptr || !ctx || EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1)
        return {};

    boost::nowide::ifstream ifs(file.string(), std::ios::binary);
    if (!ifs)
        return {};

    std::vector<char> buffer(64 * 1024);
    while (ifs) {
        ifs.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize read_bytes = ifs.gcount();
        if (read_bytes > 0 && EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(read_bytes)) != 1)
            return {};
    }
    if (!ifs.eof())
        return {};

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_size = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &digest_size) != 1 || digest_size == 0)
        return {};

    static const char hex[] = "0123456789abcdef";
    std::string       out;
    out.reserve(static_cast<std::size_t>(digest_size) * 2);
    for (unsigned int i = 0; i < digest_size; ++i) {
        out.push_back(hex[digest[i] >> 4]);
        out.push_back(hex[digest[i] & 0x0F]);
    }
    return out;
}

bool digests_equal(const std::string& expected, const std::string& actual)
{
    const std::string actual_normalized = normalized(actual);
    return !actual_normalized.empty() && normalized(expected) == actual_normalized;
}

Verdict verify_file(const boost::filesystem::path& file, const std::string& expected_sha256, const std::string& expected_md5)
{
    Verdict verdict;

    const std::pair<Algorithm, std::string> checks[] = {
        {Algorithm::SHA256, normalized(expected_sha256)},
        {Algorithm::MD5, normalized(expected_md5)},
    };
    for (const auto& check : checks) {
        if (check.second.empty())
            continue;
        const std::string actual = file_hex_digest(file, check.first);
        if (!digests_equal(check.second, actual)) {
            verdict.ok     = false;
            verdict.reason = std::string(algorithm_name(check.first)) + " mismatch: expected " + check.second + ", got " +
                             (actual.empty() ? std::string("none (file not readable)") : actual);
            return verdict;
        }
    }
    return verdict;
}

} // namespace OtaPackageDigest
} // namespace Slic3r
