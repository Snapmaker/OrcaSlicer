#!/usr/bin/env python3
"""End-to-end gate for the phone's Bambu reprint (RemoteSend::prepare_from_record for a Bambu
printer, /api/archive/{id}/preview and /send), against UltraNet's loopback fake printer.

No real printer is ever contacted: every printer in the data dir is a fake on 127.0.0.1 (the
network plug-in's LAN ports 8883 / 990 are fixed, so only one fake runs at a time), and the data
dir must hold nothing else. The run:

  1. writes three archive records (an H2D job on plate 2 of 3, an H2C job, an X1C job) from
     tests/data/bambu_reprint into the instance's G-code archive folder;
  2. H2D: the mapping preview (automatic mapping, sides, options defaults from the data dir's
     remembered send-dialog choices), a wrong-side override (refused), a dry run, a real send - the
     upload and the MQTT project_file the fake captured are checked field by field against what the
     desktop's send dialog composes for that plate on that AMS - and the record's reprint history;
     a busy printer and a job of another model are refused in words;
  3. H2C (nozzle rack): the get_auto_nozzle_mapping handshake goes out before project_file, and the
     printer's answer comes back in it as nozzle_mapping; the slice's timelapse warning turns
     timelapse off;
  4. X1 Carbon: a one-nozzle payload - no nozzleId, no nozzles_info, no nozzle offset calibration.

usage: test_bambu_reprint_gate.py --datadir DIR --fakeprinter <ultranet>/tools/fakeprinter.py
                                  --fixtures <repo>/tests/data/bambu_reprint --work DIR
The data dir's instance must be running (the gate script starts it) with phone access token
testtoken12345, and its conf must hold the three fake printers (SNFAKEH2D0001, SNFAKEH2C0001,
SNFAKEX1C0001 on 127.0.0.1) with access codes, and the archive switched on.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

TOKEN = "testtoken12345"
HERE = os.path.dirname(os.path.abspath(__file__))
FAILS = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what, flush=True)
    if not cond:
        FAILS.append(what)
    return cond


class Hub:
    def __init__(self, datadir):
        hub = json.load(open(os.path.join(datadir, "hub", "hub.json"), encoding="utf-8"))
        self.port = hub["port"]
        pids = [int(f.split(".")[0]) for f in os.listdir(os.path.join(datadir, "hub", "instances"))]
        self.pid = None
        for pid in pids:
            try:
                j = self.get("/i/%d/api/info" % pid)
                if j.get("title") is not None:
                    self.pid = pid
                    break
            except Exception:
                pass
        if not self.pid:
            raise SystemExit("no slicer instance answers")

    def url(self, path):
        return "http://127.0.0.1:%d/r/%s%s" % (self.port, TOKEN, path)

    def get(self, path, timeout=30):
        return json.load(urllib.request.urlopen(self.url(path), timeout=timeout))

    def post(self, path, form, timeout=60):
        body = urllib.parse.urlencode(form).encode()
        req = urllib.request.Request(self.url(path), data=body, method="POST",
                                     headers={"Content-Type": "application/x-www-form-urlencoded"})
        try:
            r = urllib.request.urlopen(req, timeout=timeout)
            return r.status, json.loads(r.read().decode() or "{}")
        except urllib.error.HTTPError as e:
            text = e.read().decode()
            try:
                return e.code, json.loads(text)
            except ValueError:
                return e.code, {"error": text}

    def api(self, path):
        return "/i/%d/api%s" % (self.pid, path)

    def follow(self, job, budget=180):
        deadline = time.time() + budget
        while time.time() < deadline:
            j = self.get(self.api("/jobs/%d" % job))
            if j.get("state") != "running":
                return j
            time.sleep(1)
        return {"state": "timeout"}


class Fake:
    """One fake printer on 127.0.0.1:8883/990, recording what it receives."""

    def __init__(self, args, name, profile, *extra):
        self.record = os.path.join(args.work, name + ".json")
        self.log = os.path.join(args.work, name + ".log")
        for p in (self.record, self.log):
            if os.path.exists(p):
                os.remove(p)
        cmd = [sys.executable, "-u", os.path.join(HERE, "fake_bambu_status.py"), "--fakeprinter", args.fakeprinter,
               "--profile", profile] + list(extra) + ["--", "--host", "127.0.0.1", "--out", self.record, "--seconds", "900",
                                                     "--status-interval", "2", "--serial", name]
        self.proc = subprocess.Popen(cmd, stdout=open(self.log, "w"), stderr=subprocess.STDOUT)
        deadline = time.time() + 30
        while time.time() < deadline:
            if "READY" in open(self.log).read():
                return
            time.sleep(0.2)
        raise SystemExit("fake printer %s did not come up: %s" % (name, open(self.log).read()))

    def rec(self):
        try:
            return json.load(open(self.record, encoding="utf-8"))
        except Exception:
            return {"publishes": [], "uploads": [], "connects": []}

    def payloads(self, command):
        out = []
        for p in self.rec().get("publishes", []):
            if '"%s"' % command in p.get("payload", ""):
                out.append(json.loads(p["payload"]))
        return out

    def stop(self):
        self.proc.terminate()
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def md5(path):
    return hashlib.md5(open(path, "rb").read()).hexdigest()


def write_record(archive, rid, fixture, serial, name, model, sent_name):
    os.makedirs(archive, exist_ok=True)
    dest = os.path.join(archive, rid + ".gcode.3mf")
    shutil.copyfile(fixture, dest)
    data = open(dest, "rb").read()
    side = {
        "id": rid, "time": int(time.time()) - 3600, "file": rid + ".gcode.3mf", "sent_name": sent_name,
        "size": len(data), "sha256": hashlib.sha256(data).hexdigest(),
        "printer": {"id": serial, "kind": "bambu", "name": name, "model": model},
        "plate": 0, "plate_name": "", "project_title": "reprint gate", "source": "desktop", "mode": "print",
        "filaments": [], "spoolman_deduct": False,
    }
    json.dump(side, open(os.path.join(archive, rid + ".json"), "w"), indent=2)
    return dest


def wait_connected(hub, rid, serial, budget=60):
    """Preview until the printer's report has arrived (the first one may still be connecting)."""
    deadline = time.time() + budget
    st, j = 0, {}
    while time.time() < deadline:
        st, j = hub.post(hub.api("/archive/%s/preview" % rid), {"printer": serial})
        if st == 200:
            return st, j
        if st == 409 and any(w in j.get("error", "") for w in ("storage", "SD card", "offline")):
            time.sleep(3)  # the report with the SD card state has not arrived yet
            continue
        return st, j
    return st, j


def phase_h2d(args, hub, archive):
    rid = "20260926-100000_FakeH2D_two_sided"
    path = write_record(archive, rid, os.path.join(args.fixtures, "h2d_two_sided.gcode.3mf"), "SNFAKEH2D0001",
                        "Fake H2D", "Bambu Lab H2D", "two_sided_plate_2.gcode.3mf")
    fake = Fake(args, "SNFAKEH2D0001", "h2d")
    try:
        st, pv = wait_connected(hub, rid, "SNFAKEH2D0001")
        print("preview:", st, json.dumps(pv)[:600])
        json.dump(pv, open(os.path.join(args.work, "h2d_preview.json"), "w"), indent=1)  # the app tests' fixture shape
        if not check(st == 200, "H2D preview answers 200"):
            return
        check(pv.get("can_send") is True, "H2D preview: the job can be sent")
        check(pv.get("mapping") == "0:128-0,1:0-1,3:0-2",
              "H2D preview: automatic mapping per side (left filament from the AMS HT, right ones from AMS A) - got %s" % pv.get("mapping"))
        fil = {f["index"]: f for f in pv.get("filaments", [])}
        check(fil.get(0, {}).get("side") == "L" and fil.get(1, {}).get("side") == "R" and fil.get(3, {}).get("side") == "R",
              "H2D preview: each filament's nozzle side as sliced")
        check(all(f.get("auto") for f in pv.get("filaments", [])), "H2D preview: every slot is the automatic one")
        trays = {t["id"]: t for t in pv.get("trays", [])}
        check(trays.get("128-0", {}).get("side") == "L" and trays.get("0-1", {}).get("side") == "R",
              "H2D preview: trays carry their side")
        opts = pv.get("options", {})
        check(opts.get("bed_leveling", {}).get("value") is True and opts.get("flow_cali", {}).get("value") is False
              and opts.get("timelapse", {}).get("value") is False and opts.get("nozzle_offset_cali", {}).get("value") is True
              and opts.get("use_ams", {}).get("value") is True,
              "H2D preview: options default to the remembered send-dialog choices (bed on, flow off, timelapse off, offset on)")
        check(pv.get("job", {}).get("plate") == 2, "H2D preview: plate 2 of the file")
        check(pv.get("nozzle_mapping", {}).get("applies") is False, "H2D preview: no nozzle-mapping handshake on an H2D")

        # A left filament on a right-side slot: shown in the preview, refused by the send.
        st, bad = hub.post(hub.api("/archive/%s/preview" % rid), {"printer": "SNFAKEH2D0001", "ams_mapping": "0:0-0"})
        check(st == 200 and bad.get("can_send") is False and any(p.get("code") == "side" for p in bad.get("problems", [])),
              "H2D preview: a left filament on a right-side slot is flagged (side)")
        st, j = hub.post(hub.api("/archive/%s/send" % rid),
                         {"printer": "SNFAKEH2D0001", "mode": "print", "confirm": "1", "ams_mapping": "0:0-0"})
        check(st == 409 and "left" in j.get("error", ""), "H2D send: the wrong side is refused in words (%s)" % j.get("error"))
        st, j = hub.post(hub.api("/archive/%s/send" % rid),
                         {"printer": "SNFAKEH2D0001", "mode": "print", "confirm": "1", "ams_mapping": "1:0-3"})
        check(st == 400 and ("empty" in j.get("error", "") or "no AMS slot" in j.get("error", "")),
              "H2D send: a slot with nothing in it is refused (%s)" % j.get("error"))
        st, j = hub.post(hub.api("/archive/%s/send" % rid), {"printer": "SNFAKEH2D0001", "mode": "print"})
        check(st == 400, "H2D send without confirm=1 is refused")

        # Dry run: the parameters, never a path on the PC.
        st, j = hub.post(hub.api("/archive/%s/send" % rid), {"printer": "SNFAKEH2D0001", "mode": "print", "confirm": "1",
                                                              "dry_run": "1", "ams_mapping": pv.get("mapping", "")})
        check(st == 200 and j.get("kind") == "bambu", "H2D dry run starts (%s)" % j)
        if st == 200:
            done = hub.follow(j["job"])
            res = done.get("result", {})
            params = res.get("params", {})
            check(done.get("state") == "done" and res.get("dry_run") is True, "H2D dry run finishes")
            check(params.get("filename") == "archive:" + rid, "H2D dry run: the archive path never reaches the phone")
            check(params.get("plate_index") == 2 and res.get("call") == "start_local_print",
                  "H2D dry run: plate 2, the LAN print call (%s, %s)" % (params.get("plate_index"), res.get("call")))

        uploads_before = len(fake.rec().get("uploads", []))
        st, j = hub.post(hub.api("/archive/%s/send" % rid), {"printer": "SNFAKEH2D0001", "mode": "print", "confirm": "1",
                                                              "ams_mapping": pv.get("mapping", "")})
        check(st == 200, "H2D send starts (%s %s)" % (st, j))
        if st != 200:
            return
        done = hub.follow(j["job"])
        print("send result:", json.dumps(done)[:800])
        check(done.get("state") == "done", "H2D send finishes (%s: %s)" % (done.get("state"), done.get("error")))
        time.sleep(1)
        rec = fake.rec()
        ups = rec.get("uploads", [])[uploads_before:]
        real = [u for u in ups if "verify_job" not in u["name"]]
        check(any("verify_job" in u["name"] for u in ups), "H2D send: the access-code probe upload went first (as PrintJob)")
        check(len(real) == 1 and real[0]["md5"] == md5(path) and real[0]["size"] == os.path.getsize(path),
              "H2D send: the archived file was uploaded byte for byte (%s)" % real)
        pf = fake.payloads("project_file")
        if not check(len(pf) == 1, "H2D send: one project_file reached the printer"):
            return
        p = pf[0]["print"]
        up_name = real[0]["name"] if real else ""
        check(p.get("param") == "Metadata/plate_2.gcode", "project_file param names plate 2 (%s)" % p.get("param"))
        check(p.get("url") == "ftp:///" + up_name, "project_file url is the uploaded file (%s)" % p.get("url"))
        check(p.get("md5") == md5(path), "project_file md5 is the archived file's")
        check(p.get("subtask_name") == "two_sided_plate_2", "project_file subtask_name is the job's name (%s)" % p.get("subtask_name"))
        check(p.get("use_ams") is True, "project_file use_ams")
        check(p.get("ams_mapping") == [512, 1, -1, 2], "project_file ams_mapping (%s)" % p.get("ams_mapping"))
        check(p.get("ams_mapping2") == [{"ams_id": 128, "slot_id": 0}, {"ams_id": 0, "slot_id": 1},
                                        {"ams_id": 255, "slot_id": 255}, {"ams_id": 0, "slot_id": 2}],
              "project_file ams_mapping2 (%s)" % p.get("ams_mapping2"))
        info = p.get("ams_mapping_info", [])
        check(len(info) == 4 and info[0].get("nozzleId") == 1 and info[1].get("nozzleId") == 0 and info[3].get("nozzleId") == 0
              and "nozzleId" not in info[2], "project_file ams_mapping_info nozzleId: left 1, right 0, unused none (%s)" % info)
        check(info and info[1].get("filamentType") == "PETG" and info[1].get("filamentId") == "GFG00"
              and info[1].get("sourceColor") == "#000000FF" and info[1].get("targetColor") == "000000FF",
              "project_file ams_mapping_info carries type, preset id and both colours (%s)" % (info[1] if len(info) > 1 else None))
        noz = {n["id"]: n for n in p.get("nozzles_info", [])}
        check(noz.get(1, {}).get("flowSize") == "standard_flow" and noz.get(0, {}).get("flowSize") == "high_flow"
              and abs(noz.get(1, {}).get("diameter", 0) - 0.4) < 1e-6,
              "project_file nozzles_info: left standard, right high flow, 0.4 (%s)" % p.get("nozzles_info"))
        check(p.get("extruder_cali_manual_mode") == 1, "project_file extruder_cali_manual_mode 1")
        check("nozzle_mapping" not in p and not fake.payloads("get_auto_nozzle_mapping"),
              "no nozzle-mapping handshake for an H2D")
        check(p.get("bed_type") == "textured_plate", "project_file bed_type (%s)" % p.get("bed_type"))
        check(p.get("bed_leveling") is True and p.get("flow_cali") is False and p.get("timelapse") is False
              and p.get("vibration_cali") is False and p.get("layer_inspect") is True,
              "project_file options as defaulted")
        check(p.get("auto_offset_cali") == 2 and p.get("nozzle_offset_cali") == 2, "project_file nozzle offset calibration: auto")
        side = json.load(open(os.path.join(archive, rid + ".json"), encoding="utf-8"))
        hist = side.get("reprints", [])
        check(len(hist) == 1 and hist[0].get("printer", {}).get("id") == "SNFAKEH2D0001" and hist[0].get("mode") == "print"
              and hist[0].get("source") == "phone", "the reprint is in the record's history (%s)" % hist)
        check(done.get("result", {}).get("history") is True, "the send result says the history was written")

        # Upload only: the file goes to the printer's storage, nothing starts.
        pf_before = len(fake.payloads("project_file"))
        st, j = hub.post(hub.api("/archive/%s/send" % rid), {"printer": "SNFAKEH2D0001", "mode": "upload", "confirm": "1"})
        done = hub.follow(j["job"]) if st == 200 else {}
        check(st == 200 and done.get("state") == "done" and len(fake.payloads("project_file")) == pf_before,
              "H2D upload only: uploaded, no project_file (%s)" % done.get("error"))

        # Another model: an X1C job sent to the H2D.
        xid = "20260926-100100_FakeH2D_x1c_job"
        write_record(archive, xid, os.path.join(args.fixtures, "x1c_two_colour.gcode.3mf"), "SNFAKEH2D0001",
                     "Fake H2D", "Bambu Lab H2D", "x1c_job.gcode.3mf")
        st, j = hub.post(hub.api("/archive/%s/preview" % xid), {"printer": "SNFAKEH2D0001"})
        check(st == 409 and "sliced for" in j.get("error", ""), "a job of another model is refused in words (%s)" % j.get("error"))
    finally:
        fake.stop()

    # A busy printer.
    fake = Fake(args, "SNFAKEH2D0001", "h2d", "--busy")
    try:
        deadline = time.time() + 60
        st, j = 0, {}
        while time.time() < deadline:
            st, j = hub.post(hub.api("/archive/%s/preview" % rid), {"printer": "SNFAKEH2D0001"})
            if st == 409 and "busy" in j.get("error", ""):
                break
            time.sleep(3)
        check(st == 409 and "busy printing" in j.get("error", ""), "a busy printer is refused in words (%s)" % j.get("error"))
    finally:
        fake.stop()


def phase_h2c(args, hub, archive):
    rid = "20260926-100200_FakeH2C_rack"
    path = write_record(archive, rid, os.path.join(args.fixtures, "h2c_rack.gcode.3mf"), "SNFAKEH2C0001",
                        "Fake H2C", "Bambu Lab H2C", "rack_job.gcode.3mf")
    fake = Fake(args, "SNFAKEH2C0001", "h2c")
    try:
        st, pv = wait_connected(hub, rid, "SNFAKEH2C0001")
        print("H2C preview:", st, json.dumps(pv)[:600])
        if not check(st == 200, "H2C preview answers 200"):
            return
        check(pv.get("mapping") == "0:128-0,1:0-0,2:0-1", "H2C preview: automatic mapping (%s)" % pv.get("mapping"))
        check(pv.get("nozzle_mapping", {}).get("applies") is True and pv.get("nozzle_mapping", {}).get("state") == "accepted",
              "H2C preview: the printer answered the nozzle-mapping query (%s)" % pv.get("nozzle_mapping"))
        check(pv.get("options", {}).get("timelapse", {}).get("value") is False
              and pv.get("options", {}).get("timelapse", {}).get("enabled") is False,
              "H2C preview: the slice's timelapse warning turns timelapse off")
        before = len(fake.payloads("get_auto_nozzle_mapping"))
        st, j = hub.post(hub.api("/archive/%s/send" % rid), {"printer": "SNFAKEH2C0001", "mode": "print", "confirm": "1",
                                                              "ams_mapping": pv.get("mapping", ""), "timelapse": "1"})
        done = hub.follow(j["job"]) if st == 200 else {}
        check(st == 200 and done.get("state") == "done", "H2C send finishes (%s %s)" % (st, done.get("error") or j))
        queries = fake.payloads("get_auto_nozzle_mapping")[before:]
        pf = fake.payloads("project_file")
        check(len(queries) >= 1, "H2C send: the nozzle-mapping handshake went out (%d)" % len(queries))
        if queries:
            q = queries[-1]["print"]
            check(q.get("ams_mapping", [None])[1] == (128 << 8) and q.get("ams_mapping", [None, None])[2] == 0,
                  "H2C handshake: the request names the mapped slots ((ams << 8) | slot at the 1-based filament)")
            check(len(q.get("fila_info", [])) == 3, "H2C handshake: one fila_info per mapped filament (%s)" % q.get("fila_info"))
        if check(len(pf) == 1, "H2C send: one project_file"):
            p = pf[0]["print"]
            check(p.get("nozzle_mapping") == [1, 0], "H2C project_file carries the printer's nozzle_mapping (%s)" % p.get("nozzle_mapping"))
            check(p.get("timelapse") is False, "H2C project_file: no timelapse for a slice that cannot record one")
            check(p.get("ams_mapping") == [512, 0, 1], "H2C project_file ams_mapping (%s)" % p.get("ams_mapping"))
            check(p.get("md5") == md5(path), "H2C project_file md5")
    finally:
        fake.stop()


def phase_x1c(args, hub, archive):
    rid = "20260926-100300_FakeX1C_two_colour"
    path = write_record(archive, rid, os.path.join(args.fixtures, "x1c_two_colour.gcode.3mf"), "SNFAKEX1C0001",
                        "Fake X1C", "Bambu Lab X1 Carbon", "two_colour.gcode.3mf")
    fake = Fake(args, "SNFAKEX1C0001", "x1c")
    try:
        st, pv = wait_connected(hub, rid, "SNFAKEX1C0001")
        print("X1C preview:", st, json.dumps(pv)[:400])
        if not check(st == 200, "X1C preview answers 200"):
            return
        check(pv.get("mapping") == "0:0-0,1:0-1", "X1C preview: automatic mapping by colour (%s)" % pv.get("mapping"))
        check(pv.get("options", {}).get("nozzle_offset_cali", {}).get("shown") is False, "X1C preview: no nozzle offset option")
        check(all(f.get("side") == "" for f in pv.get("filaments", [])), "X1C preview: no sides on a one-nozzle printer")
        st, j = hub.post(hub.api("/archive/%s/send" % rid), {"printer": "SNFAKEX1C0001", "mode": "print", "confirm": "1",
                                                              "ams_mapping": pv.get("mapping", "")})
        done = hub.follow(j["job"]) if st == 200 else {}
        check(st == 200 and done.get("state") == "done", "X1C send finishes (%s %s)" % (st, done.get("error") or j))
        pf = fake.payloads("project_file")
        if check(len(pf) == 1, "X1C send: one project_file"):
            p = pf[0]["print"]
            check(p.get("param") == "Metadata/plate_1.gcode", "X1C project_file param plate 1")
            check(p.get("ams_mapping") == [0, 1], "X1C project_file ams_mapping (%s)" % p.get("ams_mapping"))
            check(all("nozzleId" not in e for e in p.get("ams_mapping_info", [])), "X1C project_file: no nozzleId")
            check("nozzles_info" not in p and "extruder_cali_manual_mode" not in p and "auto_offset_cali" not in p,
                  "X1C project_file: the one-nozzle payload (no nozzles_info, no offset calibration)")
            check(p.get("md5") == md5(path), "X1C project_file md5")
    finally:
        fake.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--datadir", required=True)
    ap.add_argument("--fakeprinter", required=True)
    ap.add_argument("--fixtures", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--phases", default="h2d,h2c,x1c")
    args = ap.parse_args()
    os.makedirs(args.work, exist_ok=True)
    archive = os.path.join(args.datadir, "gcode_archive")
    hub = Hub(args.datadir)
    print("hub", hub.port, "instance", hub.pid, flush=True)
    st, j = hub.post(hub.api("/archive/nope/preview"), {"printer": "SNFAKEH2D0001"})
    check(st == 404, "an unknown record is a 404 (%s)" % st)
    phases = args.phases.split(",")
    if "h2d" in phases:
        phase_h2d(args, hub, archive)
    if "h2c" in phases:
        phase_h2c(args, hub, archive)
    if "x1c" in phases:
        phase_x1c(args, hub, archive)
    print("\n%d failure(s)" % len(FAILS))
    for f in FAILS:
        print("  FAIL", f)
    print("GATE_BAMBU_REPRINT=" + ("PASS" if not FAILS else "FAIL"))
    return 0 if not FAILS else 1


if __name__ == "__main__":
    sys.exit(main())
