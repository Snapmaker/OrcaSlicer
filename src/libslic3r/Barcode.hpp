#ifndef slic3r_Barcode_hpp_
#define slic3r_Barcode_hpp_

// Self contained encoders for the symbologies that can be embossed as a code:
// QR code (ISO/IEC 18004), Code 128, EAN-13, UPC-A and Code 39.
// The result is only a grid of dark / light modules, geometry is made in CodeEmboss.

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r::Barcode {

enum class Symbology : int { QR = 0, Code128, EAN13, UPCA, Code39 };

// QR error correction level, recovers approx. 7%, 15%, 25% and 30% of codewords
enum class QrEcc : int { Low = 0, Medium, Quartile, High };

// Grid of modules, row 0 is the top row.
// Linear (1D) codes have height 1, every column is a bar or a gap.
struct Matrix
{
    int                  width  = 0;
    int                  height = 0;
    std::vector<uint8_t> dark; // row major, 1 .. dark module / bar

    bool is_2d() const { return height > 1; }
    bool at(int x, int y) const { return dark[size_t(y) * size_t(width) + size_t(x)] != 0; }
    void set(int x, int y, bool is_dark) { dark[size_t(y) * size_t(width) + size_t(x)] = is_dark ? 1 : 0; }
};

struct QrOptions
{
    QrEcc ecc = QrEcc::Medium;
    // Use higher error correction when it fits into the same version (size)
    bool boost_ecc = true;
    // 1 .. 40
    int min_version = 1;
    int max_version = 40;
    // -1 .. choose by penalty, otherwise 0 .. 7
    int mask = -1;
};

struct Result
{
    Matrix matrix;
    // Empty on success, otherwise reason of the failure
    std::string error;
    // Text really encoded (e.g. EAN with the check digit added)
    std::string encoded_text;
    // Only for QR
    int   qr_version = 0;
    QrEcc qr_ecc     = QrEcc::Low;

    bool is_valid() const { return error.empty() && matrix.width > 0; }
};

// Text is encoded as UTF-8 bytes unless it fits numeric or alphanumeric mode
Result encode_qr(const std::string &text, const QrOptions &options = {});
// Printable ASCII (32 .. 126); runs of digits are packed by code set C
Result encode_code128(const std::string &text);
// 12 digits (the check digit is appended) or 13 digits (the check digit is verified)
Result encode_ean13(const std::string &text);
// 11 digits (the check digit is appended) or 12 digits (the check digit is verified)
Result encode_upca(const std::string &text);
// 0-9 A-Z space - . $ / + %  (lower case letters are converted to upper case)
Result encode_code39(const std::string &text);

Result encode(Symbology symbology, const std::string &text, const QrOptions &qr_options = {});

// Minimal quiet zone (in modules) recommended by the specification of the symbology
int recommended_quiet_zone(Symbology symbology);

// Fraction of codewords which QR error correction level can restore
double qr_recovery_capacity(QrEcc ecc);

// Mark function patterns (finders, timing, alignment, format and version) of QR matrix,
// same size as matrix, 1 .. function module which must not be damaged by logo
std::vector<uint8_t> qr_function_modules(int version);

const char *to_string(Symbology symbology);
const char *to_string(QrEcc ecc);

} // namespace Slic3r::Barcode

#endif // slic3r_Barcode_hpp_
