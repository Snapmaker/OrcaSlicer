"""A stand-in Snapmaker U1 for the phone's timelapse routes (GET /api/printers/{id}/timelapses,
.../timelapses/thumbnail, .../timelapses/video).

It answers what the hub's LAN list asks a U1 (SnapmakerLan::probe / identify_at):
  GET  /server/info, /access/info, /printer/info, /machine/system_info, /printer/objects/query
and Moonraker's file API for the `timelapse` root, as moonraker-timelapse leaves it:
  GET  /server/files/list?root=timelapse   videos with a preview picture each (same stem), a video
                                           without one, a frame in a sub-folder and a log file
                                           (neither of the last two may be listed by the hub)
  GET  /server/files/timelapse/<name>      the file; a single Range ("bytes=a-b", "a-", "-n") is
                                           answered 206 with Content-Range, a range past the end 416
The videos' bytes are generated, never stored: byte i of a file is (i * 7 + seed) % 251, so a test
can check any slice without downloading the rest. "big.mp4" is BIG_MB megabytes, for the
streaming-memory check.

Control surface for the test:
  GET  /mock/state              {"requests": [{"path", "range"}...], "ranges", "root"}
  GET  /mock/reset              forget the requests, ranges on, root on
  GET  /mock/ranges?on=0|1      0 = ignore Range and always send the whole file (some printers do)
  GET  /mock/root?on=0|1        0 = no timelapse root registered (Moonraker answers 400)

Binds 127.0.0.1 only. Nothing here talks to a real printer.

usage: mock_u1_timelapse.py <port> [BIG_MB]     (runs until killed; prints "READY <port>")
"""
import json
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, unquote, urlparse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18190
BIG_MB = int(sys.argv[2]) if len(sys.argv) > 2 else 64
LOCK = threading.Lock()
SERIAL = "MOCKU1TIMELAPSE0001"

# name -> (size, modified, seed)
VIDEOS = {
    "timelapse_benchy_20261001_1200.mp4": (3 * 1024 * 1024 + 123, 1759320000.25, 11),
    "timelapse_cube_20260930_1000.mp4": (200 * 1024 + 7, 1759226400.0, 23),
    "timelapse_nopic_20260901_0800.mkv": (4096, 1756713600.0, 37),
    "big.mp4": (BIG_MB * 1024 * 1024, 1700000000.0, 41),
}
PICTURES = {
    "timelapse_benchy_20261001_1200.jpg": b"\xff\xd8\xff\xe0" + b"benchy-preview" * 50 + b"\xff\xd9",
    "timelapse_cube_20260930_1000.jpg": b"\xff\xd8\xff\xe0" + b"cube-preview" * 40 + b"\xff\xd9",
    "big.png": b"\x89PNG\r\n\x1a\n" + b"big-preview" * 10,
}
OTHERS = [
    {"path": "frames/frame000001.jpg", "modified": 1759320000.0, "size": 100},
    {"path": "timelapse.log", "modified": 1759320000.0, "size": 10},
]


def fresh():
    return {"requests": [], "ranges": True, "root": True}


STATE = fresh()


def video_bytes(name, start, end):
    """Bytes start..end (inclusive) of a generated video: byte i is (i * 7 + seed) % 251."""
    seed = VIDEOS[name][2]
    cycle = bytes(((i * 7 + seed) % 251) for i in range(251))  # the pattern repeats every 251 bytes
    n = end + 1 - start
    off = start % 251
    reps = (off + n) // 251 + 1
    return (cycle * reps)[off:off + n]


def parse_range(header, total):
    """(start, end) | "unsatisfiable" | None (no / unusable header)."""
    if not header:
        return None
    m = re.fullmatch(r"\s*bytes=(\d*)-(\d*)\s*", header)
    if not m or (m.group(1) == "" and m.group(2) == ""):
        return None
    a, b = m.group(1), m.group(2)
    if a == "":
        n = int(b)
        if n == 0:
            return "unsatisfiable"
        return (max(0, total - n), total - 1)
    a = int(a)
    if a >= total:
        return "unsatisfiable"
    b = total - 1 if b == "" else min(int(b), total - 1)
    if b < a:
        return None
    return (a, b)


class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _file(self, name):
        rng_header = self.headers.get("Range")
        with LOCK:
            STATE["requests"].append({"path": name, "range": rng_header})
            honour = STATE["ranges"]
        if name in PICTURES:
            data = PICTURES[name]
            self.send_response(200)
            self.send_header("Content-Type", "image/png" if name.endswith(".png") else "image/jpeg")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        if name not in VIDEOS:
            return self._json(404, {"error": {"code": 404, "message": "File does not exist"}})
        total = VIDEOS[name][0]
        r = parse_range(rng_header, total) if honour else None
        if r == "unsatisfiable":
            self.send_response(416)
            self.send_header("Content-Range", "bytes */%d" % total)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        start, end = r if r else (0, total - 1)
        self.send_response(206 if r else 200)
        self.send_header("Content-Type", "video/mp4")
        self.send_header("Accept-Ranges", "bytes" if honour else "none")
        self.send_header("Content-Length", str(end - start + 1))
        if r:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, total))
        self.end_headers()
        pos = start
        try:
            while pos <= end:
                stop = min(end, pos + 256 * 1024 - 1)
                self.wfile.write(video_bytes(name, pos, stop))
                pos = stop + 1
        except (ConnectionError, OSError):
            pass  # the client went away mid-file (a player seeking does that)

    def do_GET(self):
        u = urlparse(self.path)
        q = dict(parse_qsl(u.query))
        if u.path == "/server/info":
            return self._json(200, {"result": {"klippy_connected": True, "klippy_state": "ready"}})
        if u.path == "/access/info":
            return self._json(200, {"result": {"login_required": False, "trusted": True}})
        if u.path == "/printer/info":
            return self._json(200, {"result": {"state": "ready", "hostname": "mock-u1-timelapse"}})
        if u.path == "/machine/system_info":
            return self._json(200, {"result": {"system_info": {"product_info": {
                "machine_type": "Snapmaker U1", "nozzle_diameter": [0.4] * 4,
                "serial_number": SERIAL, "device_name": "Mock U1 timelapse",
                "firmware_version": "1.5.2", "software_version": "1.5.2"}}}})
        if u.path == "/printer/objects/query":
            return self._json(200, {"result": {"eventtime": time.time(), "status": {
                "print_stats": {"filename": "", "state": "standby", "print_duration": 0.0, "total_duration": 0.0,
                                "message": "", "info": {"total_layer": 0, "current_layer": 0}},
                "display_status": {"progress": 0.0, "message": None},
                "heater_bed": {"temperature": 24.0, "target": 0.0}}}})
        if u.path == "/printer/gcode/help":
            return self._json(200, {"result": {"G28": "Home"}})
        if u.path == "/server/files/list":
            with LOCK:
                root_on = STATE["root"]
            if q.get("root") != "timelapse" or not root_on:
                return self._json(400, {"error": {"code": 400, "message": "Invalid root path (%s)" % q.get("root")}})
            out = [{"path": n, "modified": v[1], "size": v[0], "permissions": "rw"} for n, v in VIDEOS.items()]
            out += [{"path": n, "modified": 1759320001.0, "size": len(b), "permissions": "rw"} for n, b in PICTURES.items()]
            out += OTHERS
            return self._json(200, {"result": out})
        if u.path.startswith("/server/files/timelapse/"):
            return self._file(unquote(u.path[len("/server/files/timelapse/"):]))
        if u.path == "/mock/state":
            with LOCK:
                return self._json(200, dict(STATE))
        if u.path == "/mock/reset":
            with LOCK:
                STATE.clear()
                STATE.update(fresh())
            return self._json(200, {"ok": True})
        if u.path == "/mock/ranges":
            with LOCK:
                STATE["ranges"] = q.get("on", "1") == "1"
            return self._json(200, {"ok": True})
        if u.path == "/mock/root":
            with LOCK:
                STATE["root"] = q.get("on", "1") == "1"
            return self._json(200, {"ok": True})
        return self._json(404, {"error": {"code": 404, "message": "Not Found"}})


if __name__ == "__main__":
    ThreadingHTTPServer.allow_reuse_address = True
    ThreadingHTTPServer.daemon_threads = True
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print("READY %d" % PORT, flush=True)
    srv.serve_forever()
