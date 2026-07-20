#ifndef __FileDecrypt_hpp__
#define __FileDecrypt_hpp__

#include <string>
#include <vector>

namespace Slic3r {

struct DecryptKeyIV
{
    std::vector<unsigned char> key;
    std::vector<unsigned char> iv;
};

DecryptKeyIV derive_key_iv(const std::string& dev_id, int iterations);

bool decrypt_file_aes_cbc(const std::string& input_path,
                          const std::vector<unsigned char>& key,
                          const std::vector<unsigned char>& iv,
                          const std::string& output_path);

} // namespace Slic3r

#endif // __FileDecrypt_hpp__
