#ifndef slic3r_GUI_RemoteTimelapse_hpp_
#define slic3r_GUI_RemoteTimelapse_hpp_

// The phone's timelapse routes on a slicer instance (RemoteAccess), proxied by the hub as
// /r/<token>/i/<pid>/api/printers/{id}/timelapses...:
//
//   GET .../timelapses                         the list (JSON)
//   GET .../timelapses/thumbnail?name={file}   one video's preview picture
//   GET .../timelapses/video?name={file}[&download=1]
//                                              the video, with a single-range Range header honoured
//                                              (206 / 416), Accept-Ranges: bytes, so a phone can
//                                              stream, seek and resume
//
// A Snapmaker U1 (sm:{id}) or any Moonraker print host ("host", "ph:{id}", "connect") is read
// through Moonraker's `timelapse` file root and its video is proxied byte for byte with the Range
// header passed on. A Bambu printer is read through the storage tunnel the Device tab uses
// (PrinterTimelapse.hpp); that protocol has no ranged download, so the first request for a video
// copies it to a per-process folder in the system temp directory while the phone is already being
// served from the part that has arrived, and later requests (a seek, a resume, a replay) are
// answered from that copy. Nothing is ever held in memory beyond one read buffer.
//
// Every failure is {"error": text, "code": code}: no_printer 404, no_file 404, no_thumbnail 404,
// offline 409, no_storage 409, no_lan 409, busy 409, unsupported 501, no_tunnel 501,
// printer_error 502/504.

#include <boost/asio/ip/tcp.hpp>

#include <string>

namespace Slic3r { namespace GUI { namespace RemoteTimelapse {

struct Answer
{
    int         status { 200 };
    std::string type { "application/json" };
    std::string body;
    std::string headers; // extra "Name: value\r\n" lines
};

// Request threads only (they ask the GUI thread for what they need and wait for it).
Answer list(const std::string& printer);
Answer thumbnail(const std::string& printer, const std::string& name);
// Writes the whole HTTP answer to `client` itself, streaming. `range` is the request's Range
// header value ("" when it had none).
void   video(boost::asio::ip::tcp::socket& client, const std::string& printer, const std::string& name,
             const std::string& range, bool download);

// Forget the cached Bambu copies of this process (best effort; files still being read stay).
void   shutdown();

}}} // namespace Slic3r::GUI::RemoteTimelapse

#endif // slic3r_GUI_RemoteTimelapse_hpp_
