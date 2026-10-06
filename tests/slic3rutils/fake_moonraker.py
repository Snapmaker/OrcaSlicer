"""A fake Snapmaker U1 (Moonraker over HTTP) on 127.0.0.1, for testing the hub's LAN probe
(SnapmakerLan) end to end without a printer.

It answers what SnapmakerLan asks a U1:
  GET /server/info            {"result": {"klippy_state": "ready", ...}}
  GET /access/info            {"result": {"login_required": false, "trusted": true}}
  GET /printer/objects/query  print_stats, display_status, heater_bed, extruder..extruder3 and
                              print_task_config, shaped like firmware 1.5.2's answer
  GET /machine/system_info    model, serial number and device name (what identify() reads)
  GET /printer/info           hostname
  POST /server/files/upload   multipart (root=gcodes, file=...): kept in memory, name -> bytes
  GET /server/files/metadata  ?filename= -> {size, modified} of an uploaded file, 404 otherwise
  GET /server/files/list      ?root=gcodes -> every uploaded file
  POST /printer/print/start   ?filename= -> starts that file (404 when it was never uploaded)
  POST /printer/gcode/script  ?script= -> accepted (the mapped start's macros)

and has a control port of its own, so a test can change how the "printer" behaves:
  GET /fake/mode?m=normal           answer at once
  GET /fake/mode?m=slow&delay=6     answer every printer request after `delay` seconds
  GET /fake/mode?m=down             stop listening on the printer port (connection refused)
  GET /fake/mode?m=up               listen again and answer at once (same as normal)
  GET /fake/state?state=printing    what print_stats.state says (standby, printing, paused, ...)
  GET /fake/stats                   {"mode":..., "requests": {path: count}}
  GET /fake/files                   {"files": {name: size}, "started": [names], "uploads": n}
  GET /fake/reset                   forget every file and start, state standby

Nothing here ever talks to a real printer: both ports bind to 127.0.0.1 only.

usage: fake_moonraker.py --port 18201 --control 18202 [--name U1-fake] [--sn FAKE0001]
       (runs until killed; prints "READY <port> <control>" once both ports listen)
"""
import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

LOCK = threading.Lock()
STATE = {"mode": "normal", "delay": 0.0, "state": "printing", "requests": {},
         "files": {}, "started": [], "uploads": 0, "current": ""}
ARGS = None


def reply(handler, code, obj):
    body = json.dumps(obj).encode()
    try:
        handler.send_response(code)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)
    except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
        pass  # the client gave up waiting (a timeout test): nothing to do


def query_status(state):
    temps = [(215.0, 220.0), (30.5, 0.0), (29.8, 0.0), (30.1, 0.0)]
    status = {
        "print_stats": {
            "state": state,
            "filename": (STATE["current"] or "fake_job.gcode") if state in ("printing", "paused") else "",
            "message": "",
            "print_duration": 600.0,
            "total_duration": 640.0,
            "info": {"current_layer": 12, "total_layer": 80},
        },
        "display_status": {"progress": 0.25 if state in ("printing", "paused") else 0.0},
        "heater_bed": {"temperature": 60.2, "target": 60.0},
        "print_task_config": {
            "filament_type": ["PLA", "PETG", "PLA", "TPU"],
            "filament_color_rgba": ["FF0000FF", "00FF00FF", "0000FFFF", "FFFFFFFF"],
            "filament_exist": [True, True, False, True],
        },
    }
    for i, (t, target) in enumerate(temps):
        status["extruder" if i == 0 else "extruder%d" % i] = {
            "temperature": t, "target": target, "nozzle_diameter": 0.4}
    return {"result": {"eventtime": time.time(), "status": status}}


class Printer(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        pass

    def do_GET(self):
        u = urlparse(self.path)
        with LOCK:
            STATE["requests"][u.path] = STATE["requests"].get(u.path, 0) + 1
            mode, delay, state = STATE["mode"], STATE["delay"], STATE["state"]
        if mode == "slow" and delay > 0:
            time.sleep(delay)
        if u.path == "/server/info":
            return reply(self, 200, {"result": {"klippy_connected": True, "klippy_state": "ready",
                                                "moonraker_version": "fake"}})
        if u.path == "/access/info":
            return reply(self, 200, {"result": {"login_required": False, "trusted": True}})
        if u.path == "/printer/objects/query":
            return reply(self, 200, query_status(state))
        if u.path == "/machine/system_info":
            return reply(self, 200, {"result": {"system_info": {"product_info": {
                "machine_type": "Snapmaker U1", "serial_number": ARGS.sn, "device_name": ARGS.name}}}})
        if u.path == "/printer/info":
            return reply(self, 200, {"result": {"hostname": ARGS.name, "state": "ready"}})
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/server/files/metadata":
            with LOCK:
                size = STATE["files"].get(q.get("filename", ""))
            if size is None:
                return reply(self, 404, {"error": {"code": 404, "message": "Metadata not available for " + q.get("filename", "")}})
            return reply(self, 200, {"result": {"filename": q["filename"], "size": size, "modified": time.time()}})
        if u.path == "/server/files/list":
            with LOCK:
                files = [{"path": n, "size": sz, "modified": time.time(), "permissions": "rw"} for n, sz in STATE["files"].items()]
            return reply(self, 200, {"result": files})
        return reply(self, 404, {"error": {"code": 404, "message": "Not Found"}})

    def do_POST(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        with LOCK:
            STATE["requests"][u.path] = STATE["requests"].get(u.path, 0) + 1
        if u.path == "/server/files/upload":
            name, data = multipart_file(self.headers.get("Content-Type", ""), body)
            if not name:
                return reply(self, 400, {"error": {"code": 400, "message": "no file in the upload"}})
            with LOCK:
                STATE["files"][name] = len(data)
                STATE["uploads"] += 1
            return reply(self, 201, {"result": {"item": {"path": name, "root": "gcodes"}, "action": "create_file"}})
        if u.path == "/printer/print/start":
            name = q.get("filename", "")
            with LOCK:
                if name not in STATE["files"]:
                    return reply(self, 404, {"error": {"code": 404, "message": "File not found: " + name}})
                STATE["started"].append(name)
                STATE["current"] = name
                STATE["state"] = "printing"
            return reply(self, 200, {"result": "ok"})
        if u.path == "/printer/gcode/script":
            return reply(self, 200, {"result": "ok"})
        return reply(self, 404, {"error": {"code": 404, "message": "Not Found"}})


def multipart_file(content_type, body):
    """The one file part of a multipart/form-data body: (filename, bytes)."""
    import email.parser, email.policy
    msg = email.parser.BytesParser(policy=email.policy.HTTP).parsebytes(
        b"Content-Type: " + content_type.encode() + b"\r\n\r\n" + body)
    for part in msg.iter_parts():
        name = part.get_filename()
        if name:
            return name, part.get_payload(decode=True) or b""
    return "", b""


class PrinterPort:
    """The printer's listening socket, which `down` really closes and `up` opens again."""

    def __init__(self, port):
        self.port = port
        self.server = None
        self.thread = None

    def up(self):
        if self.server:
            return
        ThreadingHTTPServer.allow_reuse_address = True
        ThreadingHTTPServer.daemon_threads = True
        self.server = ThreadingHTTPServer(("127.0.0.1", self.port), Printer)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def down(self):
        if not self.server:
            return
        self.server.shutdown()
        self.server.server_close()
        self.server = None


PORT = None


class Control(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *a):
        pass

    def do_GET(self):
        u = urlparse(self.path)
        q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path == "/fake/mode":
            m = q.get("m", "normal")
            if m not in ("normal", "slow", "down", "up"):
                return reply(self, 400, {"error": "unknown mode " + m})
            with LOCK:
                STATE["mode"] = "normal" if m == "up" else m
                STATE["delay"] = float(q.get("delay", "6")) if m == "slow" else 0.0
            if m == "down":
                PORT.down()
            else:
                PORT.up()
            return reply(self, 200, {"mode": STATE["mode"], "delay": STATE["delay"]})
        if u.path == "/fake/state":
            with LOCK:
                STATE["state"] = q.get("state", "standby")
            return reply(self, 200, {"state": STATE["state"]})
        if u.path == "/fake/files":
            with LOCK:
                return reply(self, 200, {"files": dict(STATE["files"]), "started": list(STATE["started"]),
                                         "uploads": STATE["uploads"]})
        if u.path == "/fake/reset":
            with LOCK:
                STATE.update({"files": {}, "started": [], "uploads": 0, "current": "", "state": "standby"})
            return reply(self, 200, {"ok": True})
        if u.path == "/fake/stats":
            with LOCK:
                return reply(self, 200, {"mode": STATE["mode"], "delay": STATE["delay"],
                                         "requests": dict(STATE["requests"])})
        return reply(self, 404, {"error": "unknown"})


def main():
    global ARGS, PORT
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--control", type=int, required=True)
    ap.add_argument("--name", default="U1-fake")
    ap.add_argument("--sn", default="FAKE0001")
    ARGS = ap.parse_args()
    PORT = PrinterPort(ARGS.port)
    PORT.up()
    ctl = ThreadingHTTPServer(("127.0.0.1", ARGS.control), Control)
    print("READY %d %d" % (ARGS.port, ARGS.control), flush=True)
    try:
        ctl.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
