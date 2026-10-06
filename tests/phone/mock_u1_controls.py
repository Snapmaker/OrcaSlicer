"""A stand-in Snapmaker U1 for the phone's native printer controls (GET /api/printers `controls`,
POST /api/printers/{id}/control action=set_temp|set_speed|set_light|set_fan).

It answers what the hub's LAN list asks a U1 (SnapmakerLan::probe / identify_at):
  GET  /server/info, /access/info, /printer/info, /machine/system_info
  GET  /printer/objects/query   print_stats, display_status, heater_bed, extruder..extruder3,
                                print_task_config (four loaded toolheads) and the controls
                                objects: gcode_move (speed_factor), fan, "led cavity_led",
                                "fan_generic cavity_fan". Object names are URL-decoded, so
                                "led%20cavity_led" is the LED, as on the printer.
  POST /printer/gcode/script?script=   recorded, and the four commands the phone's controls send
                                are applied to what the query then reports, as Klipper would:
                                  SET_HEATER_TEMPERATURE HEATER=<heater_bed|extruder[N]> TARGET=<t>
                                  M220 S<percent>
                                  SET_LED LED=cavity_led WHITE=<0..1>
                                  M106 S<0..255>
                                  SET_FAN_SPEED FAN=cavity_fan SPEED=<0..1>
                                and the filament macros the phone's load / unload send (ASSUMED
                                names, see FilamentCommands.hpp): T<n> selects a toolhead,
                                INNER_FILAMENT_UNLOAD empties it, SM_PRINT_AUTO_FEED EXTRUDER=<n>
                                fills toolhead n - flipping print_task_config.filament_exist
  GET  /printer/gcode/help      the commands this "printer" knows, including those four macros
                                unless /mock/macros?on=0 took them away
and a control surface for the test:
  GET  /mock/state   {"scripts": [...], "heaters": {...}, "speed_factor", "led", "fan", "cavity_fan"}
  GET  /mock/reset   forget the scripts and put every value back
  GET  /mock/print?state=printing|standby   what print_stats.state reports

Binds 127.0.0.1 only. Nothing here talks to a real printer.

usage: mock_u1_controls.py <port>        (runs until killed; prints "READY <port>")
"""
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qsl, unquote, unquote_plus, urlparse

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18189
LOCK = threading.Lock()


def fresh():
    return {
        "scripts": [],
        "print_state": "standby",
        "heaters": {"heater_bed": 0.0, "extruder": 0.0, "extruder1": 0.0, "extruder2": 0.0, "extruder3": 0.0},
        "speed_factor": 1.0,
        "led": 1.0,
        "fan": 0.0,
        "cavity_fan": 0.0,
        "active": 0,
        "exist": [True, True, False, True],
        "macros": True,
    }


STATE = fresh()

TOOLHEADS = {
    "filament_vendor": ["Snapmaker"] * 4,
    "filament_type": ["PLA", "PLA", "PETG", "TPU"],
    "filament_sub_type": ["Matte", "Matte", "Basic", "Matte"],
    "filament_color_rgba": ["D93A2BFF", "8E8E93FF", "F5F04AFF", "000000FF"],
    "filament_official": [False] * 4,
    "filament_exist": [True, True, True, True],
    "extruder_map_table": [0, 1, 2, 3] + [0] * 28,
    "extruders_used": [False] * 4,
}


def apply(script):
    """What Klipper does with the commands the phone sends. Caller holds LOCK."""
    for line in script.splitlines():
        words = line.strip().split()
        if not words:
            continue
        cmd, args = words[0].upper(), {}
        for w in words[1:]:
            if "=" in w:
                k, v = w.split("=", 1)
                args[k.upper()] = v
            elif len(w) > 1:
                args[w[0].upper()] = w[1:]
        try:
            if cmd == "SET_HEATER_TEMPERATURE" and args.get("HEATER") in STATE["heaters"]:
                STATE["heaters"][args["HEATER"]] = float(args.get("TARGET", "0"))
            elif cmd == "M220":
                STATE["speed_factor"] = float(args.get("S", "100")) / 100.0
            elif cmd == "SET_LED" and args.get("LED") == "cavity_led":
                STATE["led"] = float(args.get("WHITE", "0"))
            elif cmd == "M106":
                STATE["fan"] = float(args.get("S", "0")) / 255.0
            elif cmd == "SET_FAN_SPEED" and args.get("FAN") == "cavity_fan":
                STATE["cavity_fan"] = float(args.get("SPEED", "0"))
            elif len(cmd) == 2 and cmd[0] == "T" and cmd[1].isdigit():
                STATE["active"] = int(cmd[1])
            elif cmd == "INNER_FILAMENT_UNLOAD" and STATE["macros"]:
                STATE["exist"][STATE["active"]] = False
            elif cmd == "SM_PRINT_AUTO_FEED" and STATE["macros"]:
                STATE["exist"][int(args.get("EXTRUDER", "0"))] = True
        except ValueError:
            pass


def status():
    """The full status object. Caller holds LOCK."""
    h = STATE["heaters"]
    st = {
        "print_stats": {"filename": "", "state": STATE["print_state"], "print_duration": 0.0,
                        "total_duration": 0.0, "message": "", "info": {"total_layer": 0, "current_layer": 0}},
        "display_status": {"progress": 0.0, "message": None},
        "heater_bed": {"temperature": 24.0, "target": h["heater_bed"]},
        "print_task_config": dict(TOOLHEADS, filament_exist=list(STATE["exist"])),
        "gcode_move": {"speed_factor": STATE["speed_factor"], "speed": 1500.0, "extrude_factor": 1.0},
        "fan": {"speed": STATE["fan"]},
        "led cavity_led": {"color_data": [[0.0, 0.0, 0.0, STATE["led"]]]},
        "fan_generic cavity_fan": {"speed": STATE["cavity_fan"]},
    }
    for i, name in enumerate(["extruder", "extruder1", "extruder2", "extruder3"]):
        st[name] = {"temperature": 25.0, "target": h[name], "nozzle_diameter": 0.4, "extruder_index": i}
    return st


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

    def do_GET(self):
        u = urlparse(self.path)
        if u.path == "/server/info":
            return self._json(200, {"result": {"klippy_connected": True, "klippy_state": "ready"}})
        if u.path == "/access/info":
            return self._json(200, {"result": {"login_required": False, "trusted": True}})
        if u.path == "/printer/info":
            return self._json(200, {"result": {"state": "ready", "hostname": "mock-u1-controls"}})
        if u.path == "/machine/system_info":
            return self._json(200, {"result": {"system_info": {"product_info": {
                "machine_type": "Snapmaker U1", "nozzle_diameter": [0.4] * 4,
                "serial_number": "MOCKU1CONTROLS00001", "device_name": "Mock U1 controls",
                "firmware_version": "1.5.2", "software_version": "1.5.2"}}}})
        if u.path == "/printer/objects/query":
            wanted = [unquote(part.split("=")[0]) for part in u.query.split("&") if part]
            with LOCK:
                st = status()
            if wanted:
                st = {k: v for k, v in st.items() if k in wanted}
            return self._json(200, {"result": {"eventtime": time.time(), "status": st}})
        if u.path == "/printer/gcode/help":
            with LOCK:
                macros = STATE["macros"]
            cmds = {"G28": "Home", "M106": "Set fan", "SET_HEATER_TEMPERATURE": "Sets a heater temperature"}
            if macros:
                cmds.update({"INNER_FILAMENT_UNLOAD": "G-Code macro", "PARK_EXTRUDER": "G-Code macro",
                             "SM_PRINT_EXTRUDER_PREHEAT": "G-Code macro", "SM_PRINT_AUTO_FEED": "G-Code macro"})
            return self._json(200, {"result": cmds})
        if u.path == "/mock/macros":
            q = dict(parse_qsl(u.query))
            with LOCK:
                STATE["macros"] = q.get("on", "1") == "1"
            return self._json(200, {"ok": True})
        if u.path == "/mock/state":
            with LOCK:
                return self._json(200, {k: v for k, v in STATE.items()})
        if u.path == "/mock/reset":
            with LOCK:
                STATE.clear()
                STATE.update(fresh())
            return self._json(200, {"ok": True})
        if u.path == "/mock/print":
            q = dict(parse_qsl(u.query))
            with LOCK:
                STATE["print_state"] = q.get("state", "standby")
            return self._json(200, {"ok": True})
        return self._json(404, {"error": {"code": 404, "message": "Not Found"}})

    def do_POST(self):
        u = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0") or "0")
        if length:
            self.rfile.read(length)
        if u.path == "/printer/gcode/script":
            script = ""
            for part in u.query.split("&"):
                if part.startswith("script="):
                    script = unquote_plus(part[len("script="):])
            with LOCK:
                STATE["scripts"].append({"script": script, "t": time.time()})
                apply(script)
            return self._json(200, {"result": "ok"})
        return self._json(404, {"error": {"code": 404, "message": "Not Found"}})


if __name__ == "__main__":
    ThreadingHTTPServer.allow_reuse_address = True
    ThreadingHTTPServer.daemon_threads = True
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
    print("READY %d" % PORT, flush=True)
    srv.serve_forever()
