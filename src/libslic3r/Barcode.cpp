#include "Barcode.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>

namespace Slic3r::Barcode {

namespace {

// ------------------------------------------------------------------
// QR code
// ------------------------------------------------------------------

// Index [ecc][version], version 0 is unused
constexpr int8_t QR_ECC_CODEWORDS_PER_BLOCK[4][41] = {
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}, // Low
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28}, // Medium
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}, // Quartile
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30}, // High
};

constexpr int8_t QR_NUM_ERROR_CORRECTION_BLOCKS[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2,  4,  4,  4,  4,  4,  6,  6,  6,  6,  7,  8,  8,  9,  9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25}, // Low
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5,  5,  5,  8,  9,  9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49}, // Medium
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8,  8,  8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68}, // Quartile
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8,  8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81}, // High
};

// bits stored in format information
int qr_ecc_format_bits(QrEcc ecc)
{
    switch (ecc) {
    case QrEcc::Low: return 1;
    case QrEcc::Medium: return 0;
    case QrEcc::Quartile: return 3;
    case QrEcc::High: return 2;
    }
    return 0;
}

int qr_size(int version) { return version * 4 + 17; }

// Count of modules usable for data and error correction codewords (include remainder bits)
int qr_num_raw_data_modules(int version)
{
    int result = (16 * version + 128) * version + 64;
    if (version >= 2) {
        int num_align = version / 7 + 2;
        result -= (25 * num_align - 10) * num_align - 55;
        if (version >= 7)
            result -= 36;
    }
    return result;
}

int qr_num_data_codewords(int version, QrEcc ecc)
{
    int e = static_cast<int>(ecc);
    return qr_num_raw_data_modules(version) / 8 - QR_ECC_CODEWORDS_PER_BLOCK[e][version] * QR_NUM_ERROR_CORRECTION_BLOCKS[e][version];
}

std::vector<int> qr_alignment_positions(int version)
{
    if (version == 1)
        return {};
    int num_align = version / 7 + 2;
    int step      = (version * 8 + num_align * 3 + 5) / (num_align * 4 - 4) * 2;
    std::vector<int> result;
    for (int i = 0, pos = qr_size(version) - 7; i < num_align - 1; ++i, pos -= step)
        result.insert(result.begin(), pos);
    result.insert(result.begin(), 6);
    return result;
}

// Multiplication in GF(2^8/0x11D)
uint8_t gf_multiply(uint8_t x, uint8_t y)
{
    int z = 0;
    for (int i = 7; i >= 0; --i) {
        z = (z << 1) ^ ((z >> 7) * 0x11D);
        z ^= ((y >> i) & 1) * x;
    }
    return static_cast<uint8_t>(z);
}

std::vector<uint8_t> reed_solomon_divisor(int degree)
{
    std::vector<uint8_t> result(size_t(degree), 0);
    result.back() = 1;
    uint8_t root  = 1;
    for (int i = 0; i < degree; ++i) {
        for (size_t j = 0; j < result.size(); ++j) {
            result[j] = gf_multiply(result[j], root);
            if (j + 1 < result.size())
                result[j] ^= result[j + 1];
        }
        root = gf_multiply(root, 0x02);
    }
    return result;
}

std::vector<uint8_t> reed_solomon_remainder(const std::vector<uint8_t> &data, const std::vector<uint8_t> &divisor)
{
    std::vector<uint8_t> result(divisor.size(), 0);
    for (uint8_t b : data) {
        uint8_t factor = b ^ result.front();
        result.erase(result.begin());
        result.push_back(0);
        for (size_t i = 0; i < result.size(); ++i)
            result[i] ^= gf_multiply(divisor[i], factor);
    }
    return result;
}

struct BitBuffer
{
    std::vector<bool> bits;
    void append(uint32_t value, int count)
    {
        for (int i = count - 1; i >= 0; --i)
            bits.push_back(((value >> i) & 1) != 0);
    }
};

enum class QrMode { Numeric, Alphanumeric, Byte };

const char *QR_ALPHANUMERIC_CHARSET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

int qr_alphanumeric_value(char c)
{
    for (int i = 0; QR_ALPHANUMERIC_CHARSET[i] != '\0'; ++i)
        if (QR_ALPHANUMERIC_CHARSET[i] == c)
            return i;
    return -1;
}

QrMode qr_select_mode(const std::string &text)
{
    bool numeric = true, alphanumeric = true;
    for (char c : text) {
        if (c < '0' || c > '9')
            numeric = false;
        if (qr_alphanumeric_value(c) < 0)
            alphanumeric = false;
    }
    if (text.empty())
        return QrMode::Byte;
    if (numeric)
        return QrMode::Numeric;
    if (alphanumeric)
        return QrMode::Alphanumeric;
    return QrMode::Byte;
}

int qr_char_count_bits(QrMode mode, int version)
{
    int range = version <= 9 ? 0 : (version <= 26 ? 1 : 2);
    switch (mode) {
    case QrMode::Numeric: return std::array<int, 3>{10, 12, 14}[range];
    case QrMode::Alphanumeric: return std::array<int, 3>{9, 11, 13}[range];
    case QrMode::Byte: return std::array<int, 3>{8, 16, 16}[range];
    }
    return 0;
}

// count of bits of segment data without header
int qr_data_bits(QrMode mode, size_t count)
{
    int n = static_cast<int>(count);
    switch (mode) {
    case QrMode::Numeric: return n / 3 * 10 + (n % 3 == 0 ? 0 : (n % 3 == 1 ? 4 : 7));
    case QrMode::Alphanumeric: return n / 2 * 11 + (n % 2) * 6;
    case QrMode::Byte: return n * 8;
    }
    return 0;
}

void qr_append_data(BitBuffer &bb, QrMode mode, const std::string &text)
{
    switch (mode) {
    case QrMode::Numeric:
        for (size_t i = 0; i < text.size();) {
            size_t   n     = std::min<size_t>(3, text.size() - i);
            uint32_t value = 0;
            for (size_t j = 0; j < n; ++j)
                value = value * 10 + uint32_t(text[i + j] - '0');
            bb.append(value, int(n) * 3 + 1);
            i += n;
        }
        break;
    case QrMode::Alphanumeric:
        for (size_t i = 0; i < text.size(); i += 2) {
            if (i + 1 < text.size())
                bb.append(uint32_t(qr_alphanumeric_value(text[i]) * 45 + qr_alphanumeric_value(text[i + 1])), 11);
            else
                bb.append(uint32_t(qr_alphanumeric_value(text[i])), 6);
        }
        break;
    case QrMode::Byte:
        for (char c : text)
            bb.append(static_cast<uint8_t>(c), 8);
        break;
    }
}

uint32_t qr_mode_indicator(QrMode mode)
{
    switch (mode) {
    case QrMode::Numeric: return 0x1;
    case QrMode::Alphanumeric: return 0x2;
    case QrMode::Byte: return 0x4;
    }
    return 0;
}

class QrBuilder
{
public:
    QrBuilder(int version, QrEcc ecc) : m_version(version), m_size(qr_size(version)), m_ecc(ecc)
    {
        m_modules.assign(size_t(m_size * m_size), 0);
        m_function.assign(size_t(m_size * m_size), 0);
        draw_function_patterns();
    }

    void build(const std::vector<uint8_t> &data_codewords, int mask)
    {
        std::vector<uint8_t> all = add_ecc_and_interleave(data_codewords);
        draw_codewords(all);

        if (mask < 0) {
            long min_penalty = -1;
            for (int m = 0; m < 8; ++m) {
                apply_mask(m);
                draw_format_bits(m);
                long penalty = penalty_score();
                if (min_penalty < 0 || penalty < min_penalty) {
                    mask        = m;
                    min_penalty = penalty;
                }
                apply_mask(m); // XOR again to undo
            }
        }
        assert(0 <= mask && mask <= 7);
        apply_mask(mask);
        draw_format_bits(mask);
    }

    Matrix matrix() const
    {
        Matrix result;
        result.width  = m_size;
        result.height = m_size;
        result.dark   = m_modules;
        return result;
    }

    const std::vector<uint8_t> &function_modules() const { return m_function; }

private:
    int                  m_version;
    int                  m_size;
    QrEcc                m_ecc;
    std::vector<uint8_t> m_modules;
    std::vector<uint8_t> m_function;

    bool module(int x, int y) const { return m_modules[size_t(y * m_size + x)] != 0; }
    void set_module(int x, int y, bool dark) { m_modules[size_t(y * m_size + x)] = dark ? 1 : 0; }
    bool is_function(int x, int y) const { return m_function[size_t(y * m_size + x)] != 0; }
    void set_function_module(int x, int y, bool dark)
    {
        set_module(x, y, dark);
        m_function[size_t(y * m_size + x)] = 1;
    }

    void draw_function_patterns()
    {
        // timing patterns
        for (int i = 0; i < m_size; ++i) {
            set_function_module(6, i, i % 2 == 0);
            set_function_module(i, 6, i % 2 == 0);
        }
        // finder patterns, overwrite some timing modules
        draw_finder_pattern(3, 3);
        draw_finder_pattern(m_size - 4, 3);
        draw_finder_pattern(3, m_size - 4);

        // alignment patterns
        std::vector<int> positions = qr_alignment_positions(m_version);
        int              num_align = static_cast<int>(positions.size());
        for (int i = 0; i < num_align; ++i)
            for (int j = 0; j < num_align; ++j) {
                // skip the three finder corners
                if ((i == 0 && j == 0) || (i == 0 && j == num_align - 1) || (i == num_align - 1 && j == 0))
                    continue;
                draw_alignment_pattern(positions[size_t(i)], positions[size_t(j)]);
            }

        // reserve format bits by dummy mask, it is overwritten later
        draw_format_bits(0);
        draw_version();
    }

    void draw_finder_pattern(int x, int y)
    {
        for (int dy = -4; dy <= 4; ++dy)
            for (int dx = -4; dx <= 4; ++dx) {
                int dist = std::max(std::abs(dx), std::abs(dy));
                int xx = x + dx, yy = y + dy;
                if (0 <= xx && xx < m_size && 0 <= yy && yy < m_size)
                    set_function_module(xx, yy, dist != 2 && dist != 4);
            }
    }

    void draw_alignment_pattern(int x, int y)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                set_function_module(x + dx, y + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    static bool get_bit(uint32_t x, int i) { return ((x >> i) & 1) != 0; }

    void draw_format_bits(int mask)
    {
        uint32_t data = uint32_t(qr_ecc_format_bits(m_ecc) << 3 | mask);
        uint32_t rem  = data;
        for (int i = 0; i < 10; ++i)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        uint32_t bits = (data << 10 | rem) ^ 0x5412;
        assert(bits >> 15 == 0);

        // first copy
        for (int i = 0; i <= 5; ++i)
            set_function_module(8, i, get_bit(bits, i));
        set_function_module(8, 7, get_bit(bits, 6));
        set_function_module(8, 8, get_bit(bits, 7));
        set_function_module(7, 8, get_bit(bits, 8));
        for (int i = 9; i < 15; ++i)
            set_function_module(14 - i, 8, get_bit(bits, i));

        // second copy
        for (int i = 0; i < 8; ++i)
            set_function_module(m_size - 1 - i, 8, get_bit(bits, i));
        for (int i = 8; i < 15; ++i)
            set_function_module(8, m_size - 15 + i, get_bit(bits, i));
        set_function_module(8, m_size - 8, true); // always dark
    }

    void draw_version()
    {
        if (m_version < 7)
            return;
        uint32_t rem = uint32_t(m_version);
        for (int i = 0; i < 12; ++i)
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        uint32_t bits = uint32_t(m_version) << 12 | rem;
        assert(bits >> 18 == 0);
        for (int i = 0; i < 18; ++i) {
            bool bit = get_bit(bits, i);
            int  a   = m_size - 11 + i % 3;
            int  b   = i / 3;
            set_function_module(a, b, bit);
            set_function_module(b, a, bit);
        }
    }

    std::vector<uint8_t> add_ecc_and_interleave(const std::vector<uint8_t> &data) const
    {
        int e                = static_cast<int>(m_ecc);
        int num_blocks       = QR_NUM_ERROR_CORRECTION_BLOCKS[e][m_version];
        int block_ecc_len    = QR_ECC_CODEWORDS_PER_BLOCK[e][m_version];
        int raw_codewords    = qr_num_raw_data_modules(m_version) / 8;
        int num_short_blocks = num_blocks - raw_codewords % num_blocks;
        int short_block_len  = raw_codewords / num_blocks;

        std::vector<std::vector<uint8_t>> blocks;
        std::vector<uint8_t>              divisor = reed_solomon_divisor(block_ecc_len);
        for (int i = 0, k = 0; i < num_blocks; ++i) {
            int                  dat_len = short_block_len - block_ecc_len + (i < num_short_blocks ? 0 : 1);
            std::vector<uint8_t> dat(data.begin() + k, data.begin() + k + dat_len);
            k += dat_len;
            std::vector<uint8_t> ecc = reed_solomon_remainder(dat, divisor);
            if (i < num_short_blocks)
                dat.push_back(0); // placeholder for interleaving
            dat.insert(dat.end(), ecc.begin(), ecc.end());
            blocks.emplace_back(std::move(dat));
        }

        std::vector<uint8_t> result;
        result.reserve(size_t(raw_codewords));
        for (size_t i = 0; i < blocks.front().size(); ++i)
            for (size_t j = 0; j < blocks.size(); ++j)
                // skip the padding placeholder of short blocks
                if (i != size_t(short_block_len - block_ecc_len) || j >= size_t(num_short_blocks))
                    result.push_back(blocks[j][i]);
        assert(result.size() == size_t(raw_codewords));
        return result;
    }

    void draw_codewords(const std::vector<uint8_t> &data)
    {
        size_t i = 0; // bit index
        for (int right = m_size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5; // skip vertical timing pattern
            for (int vert = 0; vert < m_size; ++vert)
                for (int j = 0; j < 2; ++j) {
                    int  x      = right - j;
                    bool upward = ((right + 1) & 2) == 0;
                    int  y      = upward ? m_size - 1 - vert : vert;
                    if (!is_function(x, y) && i < data.size() * 8) {
                        set_module(x, y, get_bit(data[i >> 3], 7 - int(i & 7)));
                        ++i;
                    }
                    // remainder bits stay light (0)
                }
        }
        assert(i == data.size() * 8);
    }

    void apply_mask(int mask)
    {
        for (int y = 0; y < m_size; ++y)
            for (int x = 0; x < m_size; ++x) {
                bool invert = false;
                switch (mask) {
                case 0: invert = (x + y) % 2 == 0; break;
                case 1: invert = y % 2 == 0; break;
                case 2: invert = x % 3 == 0; break;
                case 3: invert = (x + y) % 3 == 0; break;
                case 4: invert = (x / 3 + y / 2) % 2 == 0; break;
                case 5: invert = x * y % 2 + x * y % 3 == 0; break;
                case 6: invert = (x * y % 2 + x * y % 3) % 2 == 0; break;
                case 7: invert = ((x + y) % 2 + x * y % 3) % 2 == 0; break;
                default: assert(false);
                }
                if (invert && !is_function(x, y))
                    set_module(x, y, !module(x, y));
            }
    }

    // Penalty rules N1 .. N4 of ISO/IEC 18004 section 7.8.3
    long penalty_score() const
    {
        long result = 0;
        auto line_penalty = [this, &result](bool is_row) {
            for (int a = 0; a < m_size; ++a) {
                // N1 runs of same color
                int  run   = 0;
                bool color = false;
                for (int b = 0; b < m_size; ++b) {
                    bool c = is_row ? module(b, a) : module(a, b);
                    if (b == 0 || c != color) {
                        color = c;
                        run   = 1;
                    } else {
                        ++run;
                        if (run == 5)
                            result += 3;
                        else if (run > 5)
                            result += 1;
                    }
                }
                // N3 finder like pattern 1011101 with 4 light modules on one side
                for (int b = 0; b + 11 <= m_size; ++b) {
                    static const bool p1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
                    static const bool p2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
                    bool m1 = true, m2 = true;
                    for (int k = 0; k < 11 && (m1 || m2); ++k) {
                        bool c = is_row ? module(b + k, a) : module(a, b + k);
                        if (c != p1[k]) m1 = false;
                        if (c != p2[k]) m2 = false;
                    }
                    if (m1) result += 40;
                    if (m2) result += 40;
                }
            }
        };
        line_penalty(true);
        line_penalty(false);

        // N2 blocks 2x2 of same color
        for (int y = 0; y + 1 < m_size; ++y)
            for (int x = 0; x + 1 < m_size; ++x) {
                bool c = module(x, y);
                if (c == module(x + 1, y) && c == module(x, y + 1) && c == module(x + 1, y + 1))
                    result += 3;
            }

        // N4 balance of dark and light modules
        long dark = 0;
        for (uint8_t m : m_modules)
            dark += m;
        long total = long(m_size) * m_size;
        long k     = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
        result += std::max(0L, k) * 10;
        return result;
    }
};

Result error_result(const std::string &message)
{
    Result r;
    r.error = message;
    return r;
}

// widths of alternating bars and spaces (first is bar) into 1D matrix
void append_widths(std::vector<uint8_t> &modules, const std::vector<int> &widths)
{
    bool bar = true;
    for (int w : widths) {
        modules.insert(modules.end(), size_t(w), bar ? 1 : 0);
        bar = !bar;
    }
}

void append_bits(std::vector<uint8_t> &modules, const char *bits)
{
    for (const char *c = bits; *c != '\0'; ++c)
        modules.push_back(*c == '1' ? 1 : 0);
}

Result linear_result(std::vector<uint8_t> &&modules, const std::string &encoded_text)
{
    Result r;
    r.matrix.width  = static_cast<int>(modules.size());
    r.matrix.height = 1;
    r.matrix.dark   = std::move(modules);
    r.encoded_text  = encoded_text;
    return r;
}

// ------------------------------------------------------------------
// Code 128
// ------------------------------------------------------------------

// widths of bar, space, bar, space, bar, space
constexpr const char *CODE128_PATTERNS[107] = {
    "212222", "222122", "222221", "121223", "121322", "131222", "122213", "122312", "132212", "221213", // 0
    "221312", "231212", "112232", "122132", "122231", "113222", "123122", "123221", "223211", "221132", // 10
    "221231", "213212", "223112", "312131", "311222", "321122", "321221", "312212", "322112", "322211", // 20
    "212123", "212321", "232121", "111323", "131123", "131321", "112313", "132113", "132311", "211313", // 30
    "231113", "231311", "112133", "112331", "132131", "113123", "113321", "133121", "313121", "211331", // 40
    "231131", "213113", "213311", "213131", "311123", "311321", "331121", "312113", "312311", "332111", // 50
    "314111", "221411", "431111", "111224", "111422", "121124", "121421", "141122", "141221", "112214", // 60
    "112412", "122114", "122411", "142112", "142211", "241211", "221114", "413111", "241112", "134111", // 70
    "111242", "121142", "121241", "114212", "124112", "124211", "411212", "421112", "421211", "212141", // 80
    "214121", "412121", "111143", "111341", "131141", "114113", "114311", "411113", "411311", "113141", // 90
    "114131", "311141", "411131", "211412", "211214", "211232", "2331112",                             // 100
};
constexpr int CODE128_CODE_C  = 99;
constexpr int CODE128_CODE_B  = 100;
constexpr int CODE128_START_B = 104;
constexpr int CODE128_START_C = 105;
constexpr int CODE128_STOP    = 106;

size_t count_digits(const std::string &text, size_t from)
{
    size_t i = from;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9')
        ++i;
    return i - from;
}

// ------------------------------------------------------------------
// EAN-13
// ------------------------------------------------------------------
constexpr const char *EAN_L[10] = {"0001101", "0011001", "0010011", "0111101", "0100011",
                                   "0110001", "0101111", "0111011", "0110111", "0001011"};
constexpr const char *EAN_G[10] = {"0100111", "0110011", "0011011", "0100001", "0011101",
                                   "0111001", "0000101", "0010001", "0001001", "0010111"};
constexpr const char *EAN_R[10] = {"1110010", "1100110", "1101100", "1000010", "1011100",
                                   "1001110", "1010000", "1000100", "1001000", "1110100"};
// parity of digits 2..7 selected by the first digit, 0 .. L, 1 .. G
constexpr const char *EAN_PARITY[10] = {"000000", "001011", "001101", "001110", "010011",
                                        "011001", "011100", "010101", "010110", "011010"};

bool all_digits(const std::string &text)
{
    return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// check digit for EAN/UPC data digits (without check digit)
int gtin_check_digit(const std::string &digits)
{
    int sum = 0;
    // weight 3 is on the rightmost data digit
    for (size_t i = 0; i < digits.size(); ++i) {
        int d      = digits[digits.size() - 1 - i] - '0';
        int weight = (i % 2 == 0) ? 3 : 1;
        sum += d * weight;
    }
    return (10 - sum % 10) % 10;
}

// ------------------------------------------------------------------
// Code 39
// ------------------------------------------------------------------
constexpr const char *CODE39_ALPHABET = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-. $/+%";
// 9 elements (bar space bar ...), MSB first, 1 .. wide element
constexpr uint16_t CODE39_ENCODINGS[43] = {
    0x034, 0x121, 0x061, 0x160, 0x031, 0x130, 0x070, 0x025, 0x124, 0x064, // 0-9
    0x109, 0x049, 0x148, 0x019, 0x118, 0x058, 0x00D, 0x10C, 0x04C, 0x01C, // A-J
    0x103, 0x043, 0x142, 0x013, 0x112, 0x052, 0x007, 0x106, 0x046, 0x016, // K-T
    0x181, 0x0C1, 0x1C0, 0x091, 0x190, 0x0D0, 0x085, 0x184, 0x0C4, 0x0A8, // U-Z - . space $
    0x0A2, 0x08A, 0x02A,                                                  // / + %
};
constexpr uint16_t CODE39_ASTERISK = 0x094;
constexpr int      CODE39_WIDE     = 3;

void append_code39(std::vector<uint8_t> &modules, uint16_t encoding)
{
    std::vector<int> widths;
    for (int i = 8; i >= 0; --i)
        widths.push_back(((encoding >> i) & 1) ? CODE39_WIDE : 1);
    append_widths(modules, widths);
}

} // namespace

Result encode_qr(const std::string &text, const QrOptions &options)
{
    if (text.empty())
        return error_result("Text for QR code is empty.");
    int min_version = std::clamp(options.min_version, 1, 40);
    int max_version = std::clamp(options.max_version, min_version, 40);

    QrMode mode    = qr_select_mode(text);
    QrEcc  ecc     = options.ecc;
    int    version = 0;
    int    used_bits = 0;
    for (int v = min_version; v <= max_version; ++v) {
        int count_bits = qr_char_count_bits(mode, v);
        if (text.size() >= (size_t(1) << count_bits))
            continue;
        int bits = 4 + count_bits + qr_data_bits(mode, text.size());
        if (bits <= qr_num_data_codewords(v, ecc) * 8) {
            version   = v;
            used_bits = bits;
            break;
        }
    }
    if (version == 0)
        return error_result("Text is too long for QR code.");

    if (options.boost_ecc)
        for (int e = static_cast<int>(ecc) + 1; e <= static_cast<int>(QrEcc::High); ++e)
            if (used_bits <= qr_num_data_codewords(version, static_cast<QrEcc>(e)) * 8)
                ecc = static_cast<QrEcc>(e);

    BitBuffer bb;
    bb.append(qr_mode_indicator(mode), 4);
    bb.append(uint32_t(text.size()), qr_char_count_bits(mode, version));
    qr_append_data(bb, mode, text);
    assert(int(bb.bits.size()) == used_bits);

    size_t capacity = size_t(qr_num_data_codewords(version, ecc)) * 8;
    // terminator and padding to byte
    bb.append(0, int(std::min<size_t>(4, capacity - bb.bits.size())));
    bb.append(0, int((8 - bb.bits.size() % 8) % 8));
    for (uint8_t pad = 0xEC; bb.bits.size() < capacity; pad ^= 0xEC ^ 0x11)
        bb.append(pad, 8);

    std::vector<uint8_t> data_codewords(bb.bits.size() / 8, 0);
    for (size_t i = 0; i < bb.bits.size(); ++i)
        if (bb.bits[i])
            data_codewords[i >> 3] |= uint8_t(1 << (7 - (i & 7)));

    QrBuilder builder(version, ecc);
    builder.build(data_codewords, options.mask < 0 || options.mask > 7 ? -1 : options.mask);

    Result r;
    r.matrix       = builder.matrix();
    r.encoded_text = text;
    r.qr_version   = version;
    r.qr_ecc       = ecc;
    return r;
}

std::vector<uint8_t> qr_function_modules(int version)
{
    version = std::clamp(version, 1, 40);
    QrBuilder builder(version, QrEcc::Low);
    return builder.function_modules();
}

Result encode_code128(const std::string &text)
{
    if (text.empty())
        return error_result("Text for Code 128 is empty.");
    for (char c : text)
        if (c < 32 || c > 126)
            return error_result("Code 128 supports only printable ASCII characters.");

    std::vector<int> values;
    // Code set C packs pairs of digits, it is worth for runs of at least
    // 4 digits on the start / end of text and 6 digits in the middle.
    size_t leading = count_digits(text, 0);
    bool   is_c    = leading >= 4 || (leading == 2 && text.size() == 2);
    values.push_back(is_c ? CODE128_START_C : CODE128_START_B);
    size_t i = 0;
    while (i < text.size()) {
        size_t digits = count_digits(text, i);
        if (is_c) {
            if (digits >= 2) {
                values.push_back((text[i] - '0') * 10 + (text[i + 1] - '0'));
                i += 2;
            } else {
                values.push_back(CODE128_CODE_B);
                is_c = false;
            }
            continue;
        }
        bool worth_c = digits >= 6 || (digits >= 4 && i + digits == text.size());
        if (worth_c) {
            if (digits % 2 == 1) {
                // odd count of digits, first one is encoded by B
                values.push_back(text[i] - 32);
                ++i;
            }
            values.push_back(CODE128_CODE_C);
            is_c = true;
            continue;
        }
        values.push_back(text[i] - 32);
        ++i;
    }

    int checksum = values.front();
    for (size_t k = 1; k < values.size(); ++k)
        checksum += int(k) * values[k];
    values.push_back(checksum % 103);
    values.push_back(CODE128_STOP);

    std::vector<uint8_t> modules;
    for (int v : values) {
        std::vector<int> widths;
        for (const char *c = CODE128_PATTERNS[v]; *c != '\0'; ++c)
            widths.push_back(*c - '0');
        append_widths(modules, widths);
    }
    return linear_result(std::move(modules), text);
}

Result encode_ean13(const std::string &text)
{
    if (!all_digits(text) || (text.size() != 12 && text.size() != 13))
        return error_result("EAN-13 needs 12 or 13 digits.");
    std::string digits = text.substr(0, 12);
    int         check  = gtin_check_digit(digits);
    if (text.size() == 13 && text[12] - '0' != check)
        return error_result("Wrong check digit of EAN-13, expected " + std::to_string(check) + ".");
    digits += char('0' + check);

    std::vector<uint8_t> modules;
    append_bits(modules, "101");
    const char *parity = EAN_PARITY[digits[0] - '0'];
    for (int i = 1; i <= 6; ++i) {
        int d = digits[size_t(i)] - '0';
        append_bits(modules, parity[i - 1] == '0' ? EAN_L[d] : EAN_G[d]);
    }
    append_bits(modules, "01010");
    for (int i = 7; i <= 12; ++i)
        append_bits(modules, EAN_R[digits[size_t(i)] - '0']);
    append_bits(modules, "101");
    return linear_result(std::move(modules), digits);
}

Result encode_upca(const std::string &text)
{
    if (!all_digits(text) || (text.size() != 11 && text.size() != 12))
        return error_result("UPC-A needs 11 or 12 digits.");
    std::string digits = text.substr(0, 11);
    int         check  = gtin_check_digit(digits);
    if (text.size() == 12 && text[11] - '0' != check)
        return error_result("Wrong check digit of UPC-A, expected " + std::to_string(check) + ".");
    // UPC-A is EAN-13 with leading zero
    Result r = encode_ean13("0" + digits);
    if (r.is_valid())
        r.encoded_text = r.encoded_text.substr(1);
    return r;
}

Result encode_code39(const std::string &text)
{
    if (text.empty())
        return error_result("Text for Code 39 is empty.");
    std::string upper;
    for (char c : text) {
        char u = (c >= 'a' && c <= 'z') ? char(c - 'a' + 'A') : c;
        if (u == '*' || std::string(CODE39_ALPHABET).find(u) == std::string::npos)
            return error_result("Code 39 supports only 0-9, A-Z, space and - . $ / + %.");
        upper += u;
    }
    std::vector<uint8_t> modules;
    append_code39(modules, CODE39_ASTERISK);
    for (char c : upper) {
        modules.push_back(0); // inter character gap
        append_code39(modules, CODE39_ENCODINGS[std::string(CODE39_ALPHABET).find(c)]);
    }
    modules.push_back(0);
    append_code39(modules, CODE39_ASTERISK);
    return linear_result(std::move(modules), upper);
}

Result encode(Symbology symbology, const std::string &text, const QrOptions &qr_options)
{
    switch (symbology) {
    case Symbology::QR: return encode_qr(text, qr_options);
    case Symbology::Code128: return encode_code128(text);
    case Symbology::EAN13: return encode_ean13(text);
    case Symbology::UPCA: return encode_upca(text);
    case Symbology::Code39: return encode_code39(text);
    }
    return error_result("Unknown symbology.");
}

int recommended_quiet_zone(Symbology symbology)
{
    switch (symbology) {
    case Symbology::QR: return 4;
    case Symbology::Code128: return 10;
    case Symbology::EAN13: return 11;
    case Symbology::UPCA: return 9;
    case Symbology::Code39: return 10;
    }
    return 4;
}

double qr_recovery_capacity(QrEcc ecc)
{
    switch (ecc) {
    case QrEcc::Low: return 0.07;
    case QrEcc::Medium: return 0.15;
    case QrEcc::Quartile: return 0.25;
    case QrEcc::High: return 0.30;
    }
    return 0.;
}

const char *to_string(Symbology symbology)
{
    switch (symbology) {
    case Symbology::QR: return "QR";
    case Symbology::Code128: return "Code128";
    case Symbology::EAN13: return "EAN13";
    case Symbology::UPCA: return "UPCA";
    case Symbology::Code39: return "Code39";
    }
    return "";
}

const char *to_string(QrEcc ecc)
{
    switch (ecc) {
    case QrEcc::Low: return "L";
    case QrEcc::Medium: return "M";
    case QrEcc::Quartile: return "Q";
    case QrEcc::High: return "H";
    }
    return "";
}

} // namespace Slic3r::Barcode
