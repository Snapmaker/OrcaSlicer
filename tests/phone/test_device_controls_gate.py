"""Gate for the phone's native printer controls, against a running hub and mock_u1_controls.py.

Covers, on a Snapmaker over the LAN (sm:<id>) played by the mock:
  * GET /api/printers carries `controls` (bed + four toolheads with their limits, the speed factor,
    the cavity light, the part and cavity fans) and the four `toolheads`;
  * every refusal the hub makes before anything is sent: an unknown heater (404), a target past the
    limit or not a whole number (400), heating an idle printer above 50 C without confirm=1 (400), a
    speed or fan value out of range (400), a fan or printer the hub does not know (404);
  * a dry run composes the G-code the Snapmaker app itself sends and sends nothing;
  * real sends - to the MOCK - land as SET_HEATER_TEMPERATURE / M220 / SET_LED / M106 /
    SET_FAN_SPEED, and the next /api/printers reports the new values;
  * /summary carries `controls` and `toolheads` through to the app;
  * the settings go through the same authenticated route as pause: a wrong token is refused;
  * filament: controls.filament marks the U1 as offering load / unload (assumed macros, found in
    its G-code help), each toolhead carries can_load / can_unload, a print or a flexible filament
    refuses, and a load / unload sends the macro script to the mock and the toolhead reads back.

Nothing is ever sent to a real printer: the only printer this adds is the mock on 127.0.0.1, and it
is removed again at the end.

usage: test_device_controls_gate.py --datadir=<the test instance's data dir> [--mock=127.0.0.1:18189]
                                    [--token=testtoken12345]
"""
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
DATADIR = opts.get("--datadir")
MOCK = opts.get("--mock", "127.0.0.1:18189")
TOKEN = opts.get("--token", "testtoken12345")
if not DATADIR:
    raise SystemExit(__doc__)
fails = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)


def http(url, method="GET", body=None, timeout=60, headers=None):
    req = urllib.request.Request(url, method=method, data=body.encode() if isinstance(body, str) else body,
                                 headers=headers or {})
    if body is not None:
        req.add_header("Content-Type", "application/x-www-form-urlencoded")
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        raw, st = r.read(), r.status
    except urllib.error.HTTPError as e:
        raw, st = e.read(), e.code
    except Exception as e:  # noqa: BLE001
        return 0, {"error": str(e)}
    try:
        return st, json.loads(raw or b"{}")
    except Exception:
        return st, {"raw": raw[:400].decode("utf-8", "replace")}


hubj = json.load(open(os.path.join(DATADIR, "hub", "hub.json"), encoding="utf-8"))
HUB = "http://127.0.0.1:%d" % hubj["port"]
ADMIN = "http://127.0.0.1:%d" % (hubj.get("admin_port") or hubj["port"])
st, inst = http(ADMIN + "/hub/instances", headers={"X-Hub-Secret": hubj.get("secret", "")})
live = [i for i in (inst.get("instances") or []) if i.get("title")]
check(bool(live), "hub %s lists a live instance" % HUB)
if not live:
    sys.exit(1)
PID = live[0]["pid"]
API = "%s/r/%s/i/%d/api" % (HUB, TOKEN, PID)
MOCK_URL = "http://" + MOCK


def api(path, method="GET", body=None, timeout=120):
    return http(API + path, method, body, timeout=timeout)


def wait_job(job, timeout=60):
    t0 = time.time()
    while time.time() - t0 < timeout:
        st, j = api("/jobs/%d" % job)
        if st == 200 and j.get("state") != "running":
            return j
        time.sleep(0.5)
    return {"state": "timeout"}


def row():
    st, pl = api("/printers")
    for p in pl.get("printers", []):
        if p.get("id") == "sm:" + MOCK_ID:
            return p
    return {}


def control(**form):
    return api("/printers/%s/control" % urllib.parse.quote("sm:" + MOCK_ID, safe=""), "POST",
               urllib.parse.urlencode(form))


def heater(p, hid):
    for h in (p.get("controls") or {}).get("heaters", []):
        if h.get("id") == hid:
            return h
    return {}


def fan(p, fid):
    for f in (p.get("controls") or {}).get("fans", []):
        if f.get("id") == fid:
            return f
    return {}


def until(pred, timeout=20):
    """The LAN list re-reads a printer every 4 s: wait (bounded) for a value to show up."""
    p = {}
    t0 = time.time()
    while time.time() - t0 < timeout:
        p = row()
        if pred(p):
            return p
        time.sleep(1)
    return p


def scripts():
    return [s["script"] for s in http(MOCK_URL + "/mock/state", timeout=15)[1].get("scripts", [])]


print("-- the mock as a LAN Snapmaker --", flush=True)
st, _ = http(MOCK_URL + "/mock/reset", timeout=15)
check(st == 200, "the mock at %s answers (%d)" % (MOCK, st))
st, j = api("/snapmaker/add?ip=" + urllib.parse.quote(MOCK), "POST", timeout=60)
check(st == 200 and j.get("id"), "POST /api/snapmaker/add?ip=%s -> %s %s" % (MOCK, st, j.get("error") or j.get("id")))
MOCK_ID = j.get("id", "")
if not MOCK_ID:
    sys.exit(1)

p = until(lambda p: bool(p.get("controls")))
c = p.get("controls") or {}
check([h.get("id") for h in c.get("heaters", [])] == ["bed", "nozzle0", "nozzle1", "nozzle2", "nozzle3"],
      "controls.heaters: bed + four toolheads (%s)" % [h.get("id") for h in c.get("heaters", [])])
check(heater(p, "bed").get("max") == 100 and heater(p, "nozzle3").get("max") == 300,
      "limits: bed 100, toolheads 300 (%s/%s)" % (heater(p, "bed").get("max"), heater(p, "nozzle3").get("max")))
check(heater(p, "nozzle1").get("label") == "Nozzle 2", "toolheads are labelled 1..4 (%s)" % heater(p, "nozzle1").get("label"))
check(all(h.get("settable") for h in c.get("heaters", [])), "every U1 heater is settable")
check((c.get("speed") or {}).get("kind") == "factor" and (c.get("speed") or {}).get("value") == 100,
      "speed: factor 100 %% (%s)" % c.get("speed"))
check((c.get("light") or {}).get("on") is True, "light: the cavity LED, on (%s)" % c.get("light"))
check([f.get("id") for f in c.get("fans", [])] == ["part", "cavity"], "fans: part and cavity (%s)" % c.get("fans"))
check(len(p.get("toolheads") or []) == 4, "four toolheads (%d)" % len(p.get("toolheads") or []))
check(p.get("print_status") in ("standby", "complete"), "the mock is idle (%s)" % p.get("print_status"))

print("\n-- refused before anything is sent --", flush=True)
before = len(scripts())
for form, want, why in [
    (dict(action="set_temp", heater="chamber", target="40", confirm="1"), 404, "a heater the U1 has not got"),
    (dict(action="set_temp", heater="nozzle0", target="301", confirm="1"), 400, "past the toolhead limit"),
    (dict(action="set_temp", heater="bed", target="101", confirm="1"), 400, "past the bed limit"),
    (dict(action="set_temp", heater="bed", target="60.5", confirm="1"), 400, "not a whole number"),
    (dict(action="set_temp", heater="bed", target="60"), 400, "heating an idle printer above 50 C without confirm"),
    (dict(action="set_temp", target="60", confirm="1"), 400, "no heater"),
    (dict(action="set_speed", value="5"), 400, "a speed factor below 10 %"),
    (dict(action="set_speed", value="301"), 400, "a speed factor above 300 %"),
    (dict(action="set_light", on="yes"), 400, "a light switch that is not 1 or 0"),
    (dict(action="set_fan", fan="aux", percent="50"), 404, "a fan the U1 has not got"),
    (dict(action="set_fan", fan="part", percent="101"), 400, "a fan above 100 %"),
]:
    st, j = control(**form)
    check(st == want, "%s -> %d (%d %s)" % (why, want, st, j.get("error")))
check("confirm" in (control(action="set_temp", heater="bed", target="60")[1].get("error") or ""),
      "the idle refusal says confirm=1 is what is missing")
st, j = api("/printers/%s/control" % urllib.parse.quote("sm:nosuch", safe=""), "POST", "action=set_temp&heater=bed&target=0")
check(st == 404, "an unknown printer -> 404 (%d %s)" % (st, j.get("error")))
check(len(scripts()) == before, "the mock received nothing while all that was refused")

print("\n-- a dry run composes and sends nothing --", flush=True)
st, j = control(action="set_temp", heater="bed", target="60", confirm="1", dry_run="1")
check(st == 200 and j.get("job") is not None, "dry-run set_temp -> 200 with a job (%d %s)" % (st, j.get("error") or j.get("job")))
if st == 200:
    r = wait_job(j["job"])
    res = r.get("result") or {}
    check(r.get("state") == "done", "the job finished (%s %s)" % (r.get("state"), r.get("error")))
    check(res.get("dry_run") is True and res.get("script") == "SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60",
          "it would send SET_HEATER_TEMPERATURE HEATER=heater_bed TARGET=60 (%s)" % res.get("script"))
    check("/printer/gcode/script?script=" in (res.get("url") or "") and MOCK in (res.get("url") or ""),
          "to the mock's /printer/gcode/script (%s)" % res.get("url"))
    check(res.get("setting") == "Bed to 60 C", "described as 'Bed to 60 C' (%s)" % res.get("setting"))
check(len(scripts()) == before, "the dry run sent nothing")

print("\n-- sent to the mock, and read back --", flush=True)


def send(want_script, **form):
    st, j = control(**form)
    if st != 200:
        check(False, "%s -> %d %s" % (form, st, j.get("error")))
        return
    r = wait_job(j["job"])
    check(r.get("state") == "done", "%s: job %s (%s)" % (form.get("action"), r.get("state"), r.get("error") or r.get("text")))
    check(want_script in scripts(), "the mock received %r" % want_script)


send("SET_HEATER_TEMPERATURE HEATER=extruder1 TARGET=210", action="set_temp", heater="nozzle1", target="210", confirm="1")
send("SET_HEATER_TEMPERATURE HEATER=extruder2 TARGET=0", action="set_temp", heater="nozzle2", target="0")
send("SET_HEATER_TEMPERATURE HEATER=extruder3 TARGET=45", action="set_temp", heater="nozzle3", target="45")
send("M220 S150", action="set_speed", value="150")
send("SET_LED LED=cavity_led WHITE=0", action="set_light", on="0")
send("M106 S128", action="set_fan", fan="part", percent="50")
send("SET_FAN_SPEED FAN=cavity_fan SPEED=0.40", action="set_fan", fan="cavity", percent="40")

p = until(lambda p: heater(p, "nozzle1").get("target") == 210 and (p.get("controls") or {}).get("light", {}).get("on") is False)
check(heater(p, "nozzle1").get("target") == 210, "nozzle 2 now reports target 210 (%s)" % heater(p, "nozzle1").get("target"))
check(heater(p, "nozzle3").get("target") == 45, "nozzle 4 now reports target 45 (%s)" % heater(p, "nozzle3").get("target"))
check(((p.get("controls") or {}).get("speed") or {}).get("value") == 150, "the speed factor now reads 150 %")
check(((p.get("controls") or {}).get("light") or {}).get("on") is False, "the light now reads off")
check(fan(p, "part").get("percent") == 50 and fan(p, "cavity").get("percent") == 40,
      "the fans now read 50 / 40 (%s / %s)" % (fan(p, "part").get("percent"), fan(p, "cavity").get("percent")))

print("\n-- printing: a change needs no confirm --", flush=True)
http(MOCK_URL + "/mock/print?state=printing", timeout=15)
until(lambda p: p.get("print_status") == "printing")
send("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=215", action="set_temp", heater="nozzle0", target="215")
http(MOCK_URL + "/mock/print?state=standby", timeout=15)

print("\n-- filament load / unload (assumed U1 macros) --", flush=True)
# The printing check above left the mock printing a moment ago: wait for the idle reading.
p = until(lambda p: bool((p.get("controls") or {}).get("filament")) and p.get("print_status") == "standby")
fil = (p.get("controls") or {}).get("filament") or {}
check(fil.get("load") is True and fil.get("unload") is True and fil.get("assumed") is True,
      "controls.filament: load, unload, assumed (%s)" % fil)
heads = p.get("toolheads") or []
check([h.get("can_load") for h in heads] == [False, False, True, False],
      "can_load: only the empty toolhead 3 (%s)" % [h.get("can_load") for h in heads])
check([h.get("can_unload") for h in heads] == [True, True, False, False],
      "can_unload: toolheads 1, 2; not the empty 3 nor the TPU in 4 (%s)" % [h.get("can_unload") for h in heads])
check((heads[3] if len(heads) > 3 else {}).get("filament_why") == "flexible filament is unloaded by hand",
      "the TPU toolhead says why (%s)" % (heads[3] if len(heads) > 3 else {}).get("filament_why"))
before = len(scripts())
for form, want, words, why in [
    (dict(action="load_filament", slot="0"), 409, "already loaded", "loading a toolhead that is already loaded"),
    (dict(action="unload_filament", slot="2"), 409, "empty", "unloading an empty toolhead"),
    (dict(action="unload_filament", slot="3"), 409, "flexible", "unloading flexible filament"),
    (dict(action="load_filament", slot="7"), 404, "toolhead", "a toolhead the U1 has not got"),
    (dict(action="load_filament"), 400, "slot", "no toolhead"),
]:
    st, j = control(**form)
    check(st == want and words in (j.get("error") or ""), "%s -> %d (%d %s)" % (why, want, st, j.get("error")))
check(len(scripts()) == before, "the mock received nothing while all that was refused")
UNLOAD_1 = "T1\nINNER_FILAMENT_UNLOAD TEMP=220 NOZZLE_DIAMETER=0.4\nPARK_EXTRUDER"
LOAD_2 = "SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=2 TEMP=140\nSM_PRINT_AUTO_FEED EXTRUDER=2"
st, j = control(action="unload_filament", slot="1", dry_run="1")
if st == 200:
    r = wait_job(j["job"])
    check((r.get("result") or {}).get("script") == UNLOAD_1,
          "dry-run unload of toolhead 2 composes T1 / INNER_FILAMENT_UNLOAD TEMP=220 / PARK_EXTRUDER (%r)" % (r.get("result") or {}).get("script"))
else:
    check(False, "dry-run unload -> %d %s" % (st, j.get("error")))
check(len(scripts()) == before, "the dry run sent nothing")
send(UNLOAD_1, action="unload_filament", slot="1")
send(LOAD_2, action="load_filament", slot="2")
p = until(lambda p: [h.get("loaded") for h in (p.get("toolheads") or [])] == [True, False, True, True])
check([h.get("loaded") for h in (p.get("toolheads") or [])] == [True, False, True, True],
      "toolhead 2 now reads empty and toolhead 3 loaded (%s)" % [h.get("loaded") for h in (p.get("toolheads") or [])])
http(MOCK_URL + "/mock/print?state=printing", timeout=15)
until(lambda p: p.get("print_status") == "printing")
st, j = control(action="load_filament", slot="1")
check(st == 409 and "print" in (j.get("error") or ""), "load while printing -> 409 (%d %s)" % (st, j.get("error")))
p = row()
check(all(h.get("can_load") is False and h.get("can_unload") is False for h in (p.get("toolheads") or [])),
      "while printing no toolhead offers load or unload")
http(MOCK_URL + "/mock/print?state=standby", timeout=15)

print("\n-- /summary carries the controls to the app --", flush=True)
summary_row = {}
t0 = time.time()
while time.time() - t0 < 40:  # the hub re-reads its windows every 10 s
    st, s = http("%s/r/%s/summary" % (HUB, TOKEN), timeout=30)
    summary_row = next((r for r in s.get("printers", []) if r.get("id") == "sm:" + MOCK_ID), {})
    if summary_row.get("controls") and summary_row.get("toolheads"):
        break
    time.sleep(2)
check(bool((summary_row.get("controls") or {}).get("heaters")), "/summary row carries controls.heaters")
check(len(summary_row.get("toolheads") or []) == 4, "/summary row carries the four toolheads")
check("can_load" in ((summary_row.get("toolheads") or [{}])[0]), "/summary toolheads carry can_load / can_unload")

print("\n-- the same authenticated route as pause --", flush=True)
bad = "%s/r/%s/i/%d/api/printers/%s/control" % (HUB, "wrongtoken000", PID, urllib.parse.quote("sm:" + MOCK_ID, safe=""))
st, _ = http(bad, "POST", "action=set_light&on=1")
check(st in (401, 403, 404, 410), "a wrong token is refused (%d)" % st)

http(MOCK_URL + "/mock/reset", timeout=15)
st, j = api("/snapmaker/remove?id=" + urllib.parse.quote(MOCK_ID), "POST")
check(st == 200, "the mock removed from the LAN list again (%d)" % st)

print("\nRESULT: %s (%d checks, %d failed)" % ("PASS" if not fails else "FAIL", checks, len(fails)))
for f in fails:
    print("  - " + f)
sys.exit(1 if fails else 0)
