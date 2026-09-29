#!/usr/bin/env python3
"""A loopback fake Bambu printer with a chosen model, AMS and state, for the Bambu reprint gate.

It is UltraNet's own tools/fakeprinter.py (MQTT over TLS on 8883, FTPS on 990, both on 127.0.0.1
only; it refuses anything else), with its push_status report replaced by one that describes the
printer the gate needs: an idle H2D with an AMS on each extruder, an idle X1 Carbon, an H2C with a
nozzle rack, or a busy printer. Nothing else of the fake is changed - the uploads it stores and the
MQTT publishes it records (--out) are what the gate checks.

  python fake_bambu_status.py --fakeprinter <ultranet>/tools/fakeprinter.py --profile h2d \\
         [--busy] [--no-sdcard] -- <fakeprinter.py arguments: --out rec.json --seconds 600 ...>

UltraNet is private; the gate passes the path of a checkout (ULTRANET_TOOLS).
"""
import argparse
import importlib.util
import json
import sys
import threading

# Trays: (ams_id, info, [(slot, type, tray_info_idx, colour RRGGBBAA)]). "info" carries the AMS type
# in bits 0-3 and the PHYSICAL extruder it feeds in bits 8-11 (0 = main = right on the H2 series).
H2_TRAYS = [
    ("0", "1", [("0", "PLA", "GFA00", "FFFFFFFF"), ("1", "PETG", "GFG00", "000000FF"), ("2", "PLA", "GFA00", "C12E1FFF")]),
    ("128", "104", [("0", "PLA", "GFA00", "FFFFFFFF")]),
]
H2C_TRAYS = [
    ("0", "1", [("0", "PLA", "GFA00", "042F56FF"), ("1", "PLA", "GFA00", "5D989EFF")]),
    ("128", "104", [("0", "PLA", "GFA00", "F7D959FF")]),
]
X1_TRAYS = [
    ("0", "1", [("0", "PLA", "GFA00", "FFFFFFFF"), ("1", "PLA", "GFA00", "000000FF")]),
]

PROFILES = {
    # model code, extruders, rack, trays
    "h2d": ("O1D", 2, False, H2_TRAYS),
    "h2c": ("O1C2", 2, True, H2C_TRAYS),
    "x1c": ("BL-P001", 1, False, X1_TRAYS),
}


def ams_block(trays):
    ams, ams_bits, tray_bits = [], 0, 0
    for ams_id, info, slots in trays:
        n = int(ams_id)
        if n >= 128:
            ams_bits |= 1 << (4 + n - 128)
        else:
            ams_bits |= 1 << n
        tlist = []
        for slot, typ, idx, colour in slots:
            s = int(slot)
            if n >= 128:
                tray_bits |= 1 << (16 + n - 128)
            else:
                tray_bits |= 1 << (n * 4 + s)
            tlist.append({"id": slot, "tray_type": typ, "tray_info_idx": idx, "tray_color": colour,
                          "tag_uid": "0000000000000000", "remain": 80})
        ams.append({"id": ams_id, "info": info, "humidity": "4", "temp": "25.0", "tray": tlist})
    return {"ams": ams, "ams_exist_bits": "%x" % ams_bits, "tray_exist_bits": "%x" % tray_bits,
            "tray_is_bbl_bits": "%x" % tray_bits, "tray_read_done_bits": "%x" % tray_bits,
            "tray_now": "255", "tray_tar": "255", "version": 3}


def make_report(profile, busy, sdcard):
    model, extruders, rack, trays = PROFILES[profile]
    p = {
        "command": "push_status", "msg": 0,
        "nozzle_temper": 25.0, "nozzle_target_temper": 0, "bed_temper": 25.0, "bed_target_temper": 0,
        "gcode_state": "RUNNING" if busy else "IDLE",
        "mc_percent": 40 if busy else 0, "mc_remaining_time": 30 if busy else 0,
        "subtask_name": "someone else's job" if busy else "",
        "print_type": "local", "print_error": 0,
        "sdcard": sdcard, "home_flag": (1 << 8) if sdcard else 0,
        "nozzle_diameter": "0.4", "nozzle_type": "stainless_steel",
        "ams": ams_block(trays),
    }
    if extruders == 2:
        # The new-protocol ("np") blocks the H2 series reports: aux bits 12-13 are the SD card state,
        # fun bit 60 says the printer has a nozzle rack (H2C).
        p.update({"cfg": "0", "fun": ("1" + "0" * 15) if rack else "0", "aux": "1000" if sdcard else "0", "stat": "0"})
        nozzles = [{"id": 0, "type": "HS00", "diameter": 0.4, "stat": 0, "wear": 0},
                   {"id": 1, "type": "HS00", "diameter": 0.4, "stat": 0, "wear": 0}]
        if rack:
            nozzles += [{"id": 0x10 + n, "type": "HS00", "diameter": 0.4, "stat": 0, "wear": 0} for n in range(2)]
        p["device"] = {
            "nozzle": {"exist": 3, "state": 0, "info": nozzles},
            "extruder": {"state": 2, "info": [
                {"id": 0, "info": 8, "temp": 25, "snow": 0xFFFF, "spre": 0xFFFF, "star": 0xFFFF, "hnow": 0, "htar": 0, "stat": 0},
                {"id": 1, "info": 8, "temp": 25, "snow": 0xFFFF, "spre": 0xFFFF, "star": 0xFFFF, "hnow": 1, "htar": 1, "stat": 0}]},
        }
    return p


def main():
    argv = sys.argv[1:]
    fake_args = []
    if "--" in argv:
        i = argv.index("--")
        argv, fake_args = argv[:i], argv[i + 1:]
    ap = argparse.ArgumentParser()
    ap.add_argument("--fakeprinter", required=True)
    ap.add_argument("--profile", choices=sorted(PROFILES), default="h2d")
    ap.add_argument("--busy", action="store_true")
    ap.add_argument("--no-sdcard", action="store_true")
    a = ap.parse_args(argv)

    spec = importlib.util.spec_from_file_location("fakeprinter", a.fakeprinter)
    fake = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fake)

    seq = {"n": 0}
    lock = threading.Lock()
    full = make_report(a.profile, a.busy, not a.no_sdcard)

    def status_report(is_full):
        with lock:
            seq["n"] += 1
            n = seq["n"]
        if is_full:
            p = dict(full)
        else:
            p = {"command": "push_status", "msg": 1, "gcode_state": full["gcode_state"]}
        p["sequence_id"] = str(n)
        return json.dumps({"print": p}, separators=(",", ":"))

    fake.status_report = status_report
    sys.argv = [a.fakeprinter, "--model", PROFILES[a.profile][0]] + fake_args
    fake.main()


if __name__ == "__main__":
    main()
