#ifndef slic3r_GUI_AO_Diagnostics_AOCaptureWriter_hpp_
#define slic3r_GUI_AO_Diagnostics_AOCaptureWriter_hpp_

#include "../AOTypes.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace AO {

// CPU-side encoders and file writers for capture products. Inputs are always owned CPU buffers
// produced by GLAOReadback; no GL handles ever reach this module.
namespace AOCaptureWriter {
// 8-bit grayscale (or RGB triplets with rgb=true) BMP, bottom-up rows.
bool write_gray_bmp(const std::string& path, const std::vector<unsigned char>& pixels, int w, int h, bool rgb = false);
// 1-component (Pf) or 3-component (PF) float raster, little-endian, bottom-to-top rows.
bool write_pfm(const std::string& path, const std::vector<float>& pixels, int w, int h, int channels);
// 8-bit grayscale PGM with top-down rows (255 maxval).
bool write_pgm(const std::string& path, const std::vector<unsigned char>& pixels, int w, int h);
// Text products; classic locale, full precision doubles.
bool write_text(const std::string& path, const std::string& contents);
// Atomic completion marker: temporary file + rename.
bool write_capture_complete(const std::string& directory, int width, int height);
} // namespace AOCaptureWriter

// One-shot capture sequencing: the AO products are written during the frame; the completion
// marker is deferred until the canvas presents the full frame successfully.
class AOCaptureSession
{
public:
    // Queues the next successful AO render to export one diagnostic frame.
    void request(const std::string& directory);
    void cancel();
    bool capture_requested() const { return !m_capture_directory.empty(); }
    // Claims the pending request at frame start; empty when no capture runs this frame.
    std::string begin_frame();
    // Records the AO products outcome; defers the marker when the presented frame is required.
    void products_finished(const std::string& directory, bool defer_completion, bool ok, int width, int height);
    // Completes the deferred capture after presentation. write_presented_products emits the
    // color-presented capture and presentation metadata; the completion marker is written
    // atomically afterwards. Returns false on any failure.
    bool               finalize(bool presented_ok, int width, int height, const std::function<bool()>& write_presented_products);
    bool               pending() const { return !m_pending_directory.empty(); }
    const std::string& pending_directory() const { return m_pending_directory; }
    // Directory finalize() would consume (deferred products first, unconsumed request second).
    std::string finalize_directory() const { return m_pending_directory.empty() ? m_capture_directory : m_pending_directory; }
    void        forget()
    {
        m_capture_directory.clear();
        m_pending_directory.clear();
    }

private:
    std::string m_capture_directory;
    std::string m_pending_directory;
};

}}} // namespace Slic3r::GUI::AO
#endif
