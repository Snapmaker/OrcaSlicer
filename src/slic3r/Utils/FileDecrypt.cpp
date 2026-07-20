#include "FileDecrypt.hpp"

#include <fstream>
#include <vector>
#include <cstring>

#include <openssl/evp.h>
#include <openssl/err.h>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

namespace Slic3r {

static const char* const PBKDF2_SALT = "kN2C6si0OK98p7IibGmJ1bQxY9PLWRQi94mPCxqUVgSr5w6YGGXGBaF5bKb";
static const int PBKDF2_KEY_LEN = 32;
static const int PBKDF2_IV_LEN  = 16;
static const int PBKDF2_OUT_LEN = PBKDF2_KEY_LEN + PBKDF2_IV_LEN; // 48

DecryptKeyIV derive_key_iv(const std::string& dev_id, int iterations)
{
    DecryptKeyIV result;
    result.key.resize(PBKDF2_KEY_LEN);
    result.iv.resize(PBKDF2_IV_LEN);

    std::vector<unsigned char> derived(PBKDF2_OUT_LEN);

    const int ret = PKCS5_PBKDF2_HMAC(
        dev_id.c_str(),
        static_cast<int>(dev_id.size()),
        reinterpret_cast<const unsigned char*>(PBKDF2_SALT),
        static_cast<int>(std::strlen(PBKDF2_SALT)),
        iterations,
        EVP_sha256(),
        PBKDF2_OUT_LEN,
        derived.data());

    if (ret != 1) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: PKCS5_PBKDF2_HMAC failed";
        result.key.clear();
        result.iv.clear();
        return result;
    }

    std::memcpy(result.key.data(), derived.data(), PBKDF2_KEY_LEN);
    std::memcpy(result.iv.data(), derived.data() + PBKDF2_KEY_LEN, PBKDF2_IV_LEN);

    return result;
}

bool decrypt_file_aes_cbc(const std::string& input_path,
                          const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& iv,
                          const std::string& output_path)
{
    if (key.size() != PBKDF2_KEY_LEN || iv.size() != PBKDF2_IV_LEN) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: invalid key/iv size";
        return false;
    }

    std::ifstream ifs(input_path, std::ios::binary | std::ios::ate);
    if (!ifs.is_open()) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: cannot open input file: " << input_path;
        return false;
    }

    const std::streamsize file_size = ifs.tellg();
    ifs.seekg(0, std::ios::beg);

    std::vector<unsigned char> cipher_data(static_cast<size_t>(file_size));
    if (!ifs.read(reinterpret_cast<char*>(cipher_data.data()), file_size)) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: failed to read input file: " << input_path;
        return false;
    }
    ifs.close();

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (ctx == nullptr) {
        BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_CIPHER_CTX_new failed";
        return false;
    }

    std::vector<unsigned char> plain_data(static_cast<size_t>(file_size) + EVP_MAX_BLOCK_LENGTH);
    int out_len = 0;
    int final_len = 0;

    bool success = false;
    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv.data()) != 1) {
            BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_DecryptInit_ex failed: "
                                     << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }

        if (EVP_DecryptUpdate(ctx, plain_data.data(), &out_len,
                              cipher_data.data(), static_cast<int>(cipher_data.size())) != 1) {
            BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_DecryptUpdate failed: "
                                     << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }

        if (EVP_DecryptFinal_ex(ctx, plain_data.data() + out_len, &final_len) != 1) {
            BOOST_LOG_TRIVIAL(error) << "FileDecrypt: EVP_DecryptFinal_ex failed: "
                                     << ERR_error_string(ERR_get_error(), nullptr);
            break;
        }

        const int total_len = out_len + final_len;

        std::ofstream ofs(output_path, std::ios::binary);
        if (!ofs.is_open()) {
            BOOST_LOG_TRIVIAL(error) << "FileDecrypt: cannot open output file: " << output_path;
            break;
        }

        ofs.write(reinterpret_cast<const char*>(plain_data.data()), total_len);
        if (ofs.fail()) {
            BOOST_LOG_TRIVIAL(error) << "FileDecrypt: failed to write output file: " << output_path;
            ofs.close();
            break;
        }
        ofs.close();

        success = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);
    return success;
}

} // namespace Slic3r
