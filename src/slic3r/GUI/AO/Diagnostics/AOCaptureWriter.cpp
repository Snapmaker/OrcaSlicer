#include "AOCaptureWriter.hpp"

#include <boost/log/trivial.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <locale>
#include <sstream>

namespace Slic3r { namespace GUI { namespace AO {

namespace AOCaptureWriter {

bool write_gray_bmp(const std::string& name, const std::vector<unsigned char>& pixels, int w, int h, bool rgb)
{
    const std::filesystem::path path(name);
    std::ofstream               out(path, std::ios::binary);
    if (!out)
        return false;
    const unsigned int stride = (static_cast<unsigned int>(w * 3) + 3u) & ~3u;
    const unsigned int size   = 54u + stride * static_cast<unsigned int>(h);
    auto               put16  = [&out](unsigned int v) {
        out.put(char(v));
        out.put(char(v >> 8));
    };
    auto put32 = [&put16](unsigned int v) {
        put16(v);
        put16(v >> 16);
    };
    out.write("BM", 2);
    put32(size);
    put32(0);
    put32(54);
    put32(40);
    put32(static_cast<unsigned int>(w));
    put32(static_cast<unsigned int>(h));
    put16(1);
    put16(24);
    put32(0);
    put32(stride * static_cast<unsigned int>(h));
    put32(0);
    put32(0);
    put32(0);
    put32(0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c)
                out.put(char(pixels[rgb ? (size_t(y) * w + x) * 3 + 2 - c : size_t(y) * w + x]));
        }
        for (unsigned int p = static_cast<unsigned int>(w * 3); p < stride; ++p)
            out.put(0);
    }
    out.close();
    return bool(out);
}

bool write_pfm(const std::string& name, const std::vector<float>& pixels, int w, int h, int channels)
{
    std::ofstream out(name, std::ios::binary);
    if (!out)
        return false;
    out << (channels == 1 ? "Pf\n" : "PF\n") << w << ' ' << h << "\n-1.0\n";
    out.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size() * sizeof(float)));
    out.close();
    return bool(out);
}

bool write_pgm(const std::string& name, const std::vector<unsigned char>& pixels, int w, int h)
{
    std::ofstream out(name, std::ios::binary);
    if (!out)
        return false;
    out << "P5\n" << w << ' ' << h << "\n255\n";
    for (int y = h - 1; y >= 0; --y)
        out.write(reinterpret_cast<const char*>(pixels.data() + size_t(y) * w), w);
    out.close();
    return bool(out);
}

bool write_text(const std::string& name, const std::string& contents)
{
    std::ofstream out(name);
    out.imbue(std::locale::classic());
    out << std::setprecision(17) << contents;
    out.close();
    return bool(out);
}

bool write_capture_complete(const std::string& directory, int width, int height)
{
    const auto      path = std::filesystem::path(directory);
    std::error_code error;
    const auto      temporary = path / "capture-complete.tmp";
    std::ofstream   complete(temporary);
    complete << "AO capture complete\n" << width << ' ' << height << '\n';
    complete.close();
    if (!complete) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    std::filesystem::rename(temporary, path / "capture-complete.txt", error);
    if (error) {
        std::error_code cleanup_error;
        std::filesystem::remove(temporary, cleanup_error);
        return false;
    }
    return true;
}

} // namespace AOCaptureWriter

void AOCaptureSession::request(const std::string& directory)
{
    m_capture_directory = directory;
    m_pending_directory.clear();
}

void AOCaptureSession::cancel()
{
    m_capture_directory.clear();
    m_pending_directory.clear();
}

std::string AOCaptureSession::begin_frame()
{
    const std::string directory = std::move(m_capture_directory);
    m_capture_directory.clear();
    return directory;
}

void AOCaptureSession::products_finished(const std::string& directory, bool defer_completion, bool ok, int width, int height)
{
    if (!ok || directory.empty()) {
        if (!ok)
            BOOST_LOG_TRIVIAL(warning) << "AO capture could not write all diagnostic files";
        return;
    }
    if (defer_completion)
        m_pending_directory = directory;
    else
        AOCaptureWriter::write_capture_complete(directory, width, height);
}

bool AOCaptureSession::finalize(bool presented_ok, int width, int height, const std::function<bool()>& write_presented_products)
{
    const std::string directory   = m_pending_directory.empty() ? m_capture_directory : m_pending_directory;
    const bool        was_pending = !m_pending_directory.empty();
    m_pending_directory.clear();
    m_capture_directory.clear();
    if (directory.empty())
        return true;
    std::error_code error;
    std::filesystem::remove(std::filesystem::path(directory) / "capture-complete.txt", error);
    if (!presented_ok || !was_pending || error)
        return false;
    if (!write_presented_products()) {
        BOOST_LOG_TRIVIAL(warning) << "AO final frame capture failed";
        return false;
    }
    return AOCaptureWriter::write_capture_complete(directory, width, height);
}

}}} // namespace Slic3r::GUI::AO
