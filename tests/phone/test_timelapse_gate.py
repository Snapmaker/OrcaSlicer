"""Gate for the phone's timelapse routes, against a running hub and mock_u1_timelapse.py.

Covers, on a Snapmaker over the LAN (sm:<id>) played by the mock, everything through the hub's
phone link (/r/<token>/i/<pid>/api/...), exactly as the app asks:
  * the instance manifest lists the three routes, and the hub proxies GET on them only;
  * GET .../timelapses: the videos (not the frames sub-folder, not the log), newest first, with
    size, time, mime, has_thumbnail; source moonraker; the printer block;
  * GET .../timelapses/thumbnail?name=: the preview picture byte for byte; 404 no_thumbnail for a
    video without one; 400 bad_name for a path; 404 no_file for a name the printer does not have;
  * GET .../timelapses/video?name=: the whole file (200, Accept-Ranges, Content-Length, bytes), a
    middle range, an open range, a suffix range (206 + Content-Range, bytes), a range past the end
    (416 + bytes */N), download=1 (Content-Disposition); the Range header reaches the printer;
  * a printer that ignores Range: the hub still answers 206 with the right bytes;
  * a large file streams through the hub without the slicer's memory growing with it;
  * a printer with no timelapse root: 200, no files, a note;
  * unknown printers: sm:nosuch and a Bambu-style id -> 404 no_printer;
  * a wrong token is refused.

Nothing is ever sent to a real printer: the only printer this adds is the mock on 127.0.0.1, and it
is removed again at the end.

usage: test_timelapse_gate.py --datadir=<the test instance's data dir> [--mock=127.0.0.1:18190]
                              [--token=testtoken12345] [--big-mb=64]
"""
import ctypes
import hashlib
import json
import os
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

opts = {a.split("=")[0]: a.split("=", 1)[1] for a in sys.argv[1:] if a.startswith("--") and "=" in a}
DATADIR = opts.get("--datadir")
MOCK = opts.get("--mock", "127.0.0.1:18190")
TOKEN = opts.get("--token", "testtoken12345")
BIG_MB = int(opts.get("--big-mb", "64"))
if not DATADIR:
    raise SystemExit(__doc__)
fails = []
checks = 0

SEEDS = {"timelapse_benchy_20261001_1200.mp4": (3 * 1024 * 1024 + 123, 11),
         "timelapse_cube_20260930_1000.mp4": (200 * 1024 + 7, 23),
         "timelapse_nopic_20260901_0800.mkv": (4096, 37),
         "big.mp4": (BIG_MB * 1024 * 1024, 41)}


def expect(name, start, end):
    seed = SEEDS[name][1]
    cycle = bytes(((i * 7 + seed) % 251) for i in range(251))
    n = end + 1 - start
    off = start % 251
    return (cycle * ((off + n) // 251 + 1))[off:off + n]


def check(cond, what):
    global checks
    checks += 1
    print(("  ok   " if cond else "  FAIL ") + what, flush=True)
    if not cond:
        fails.append(what)


def fetch(url, method="GET", headers=None, timeout=120):
    """(status, headers dict lower-cased, body bytes)."""
    req = urllib.request.Request(url, method=method, headers=headers or {})
    try:
        r = urllib.request.urlopen(req, timeout=timeout)
        return r.status, {k.lower(): v for k, v in r.headers.items()}, r.read()
    except urllib.error.HTTPError as e:
        return e.code, {k.lower(): v for k, v in e.headers.items()}, e.read()
    except Exception as e:  # noqa: BLE001
        return 0, {}, str(e).encode()


def jbody(b):
    try:
        return json.loads(b or b"{}")
    except Exception:  # noqa: BLE001
        return {"raw": b[:300].decode("utf-8", "replace")}


hubj = json.load(open(os.path.join(DATADIR, "hub", "hub.json"), encoding="utf-8"))
HUB = "http://127.0.0.1:%d" % hubj["port"]
ADMIN = "http://127.0.0.1:%d" % (hubj.get("admin_port") or hubj["port"])
st, _, b = fetch(ADMIN + "/hub/instances", headers={"X-Hub-Secret": hubj.get("secret", "")})
inst = jbody(b)
live = [i for i in (inst.get("instances") or []) if i.get("title")]
check(bool(live), "hub %s lists a live instance" % HUB)
if not live:
    sys.exit(1)
PID = live[0]["pid"]
API = "%s/r/%s/i/%d/api" % (HUB, TOKEN, PID)
MOCK_URL = "http://" + MOCK


def api_json(path, method="GET", timeout=120):
    st, h, b = fetch(API + path, method=method, timeout=timeout)
    return st, jbody(b)


fetch(MOCK_URL + "/mock/reset")

# ---- the manifest and the proxy ----
st, man = api_json("")
paths = [r.get("path", "") for r in man.get("routes", [])]
for want in ("/api/printers/{id}/timelapses", "/api/printers/{id}/timelapses/thumbnail?name={file}",
             "/api/printers/{id}/timelapses/video?name={file}[&download=1]"):
    check(want in paths, "instance manifest lists " + want)

# ---- the mock joins the LAN list ----
st, j = api_json("/snapmaker/add?ip=" + urllib.parse.quote(MOCK), "POST", timeout=60)
check(st == 200 and j.get("id"), "POST /api/snapmaker/add?ip=%s -> %s %s" % (MOCK, st, j.get("error") or j.get("id")))
MOCK_ID = j.get("id") or "MOCKU1TIMELAPSE0001"
PRINTER = urllib.parse.quote("sm:" + MOCK_ID, safe="")
TL = "/printers/%s/timelapses" % PRINTER
# One /api/printers round so the LAN list has probed it (online in the cached status).
api_json("/printers", timeout=60)

for method in ("POST", "DELETE"):
    st, j = api_json(TL, method)
    check(st == 404 and "not proxied" in (j.get("error") or ""), "%s %s is not proxied (%s)" % (method, TL, st))

# ---- the list ----
st, j = api_json(TL)
check(st == 200, "GET timelapses -> %s %s" % (st, j.get("error", "")))
files = j.get("files") or []
names = [f.get("name") for f in files]
check(names == ["timelapse_benchy_20261001_1200.mp4", "timelapse_cube_20260930_1000.mp4",
                "timelapse_nopic_20260901_0800.mkv", "big.mp4"], "videos only, newest first: %s" % names)
check(j.get("source") == "moonraker", "source moonraker")
check((j.get("printer") or {}).get("id") == "sm:" + MOCK_ID and (j.get("printer") or {}).get("kind") == "snapmaker",
      "printer block %s" % j.get("printer"))
by = {f.get("name"): f for f in files}
b0 = by.get("timelapse_benchy_20261001_1200.mp4", {})
check(b0.get("size") == SEEDS["timelapse_benchy_20261001_1200.mp4"][0] and b0.get("time") == 1759320000,
      "size and time: %s" % b0)
check(b0.get("mime") == "video/mp4" and b0.get("has_thumbnail") is True and b0.get("duration_s") is None,
      "mime, has_thumbnail, duration_s null")
check(by.get("timelapse_nopic_20260901_0800.mkv", {}).get("has_thumbnail") is False
      and by.get("timelapse_nopic_20260901_0800.mkv", {}).get("mime") == "video/x-matroska", "the .mkv has no thumbnail")
check(j.get("stale") is False, "not stale")


def tl(what, name, extra=""):
    return API + TL + "/" + what + "?name=" + urllib.parse.quote(name, safe="") + extra


# ---- thumbnails ----
st, h, b = fetch(tl("thumbnail", "timelapse_benchy_20261001_1200.mp4"))
check(st == 200 and h.get("content-type") == "image/jpeg" and b.startswith(b"\xff\xd8") and b"benchy-preview" in b,
      "thumbnail: %s %s %d bytes" % (st, h.get("content-type"), len(b)))
st, h, b = fetch(tl("thumbnail", "big.mp4"))
check(st == 200 and h.get("content-type") == "image/png" and b.startswith(b"\x89PNG"), "png thumbnail: %s" % st)
st, h, b = fetch(tl("thumbnail", "timelapse_nopic_20260901_0800.mkv"))
check(st == 404 and jbody(b).get("code") == "no_thumbnail", "no picture -> 404 no_thumbnail (%s %s)" % (st, jbody(b)))
st, h, b = fetch(tl("thumbnail", "../secret.mp4"))
check(st == 400 and jbody(b).get("code") == "bad_name", "a path -> 400 bad_name (%s)" % st)
st, h, b = fetch(tl("thumbnail", "nope.mp4"))
check(st == 404 and jbody(b).get("code") == "no_file", "unknown file -> 404 no_file (%s %s)" % (st, jbody(b)))

# ---- video ----
NAME = "timelapse_benchy_20261001_1200.mp4"
TOTAL = SEEDS[NAME][0]
st, h, b = fetch(tl("video", NAME))
check(st == 200 and h.get("accept-ranges") == "bytes" and h.get("content-length") == str(TOTAL),
      "whole video: %s %s len %s" % (st, h.get("accept-ranges"), h.get("content-length")))
check(b == expect(NAME, 0, TOTAL - 1), "whole video: bytes match (%d)" % len(b))
check(h.get("content-type") == "video/mp4" and "content-disposition" not in h, "video/mp4, no attachment")

for rng, a, z in (("bytes=1000-1999", 1000, 1999), ("bytes=%d-" % (TOTAL - 500), TOTAL - 500, TOTAL - 1),
                  ("bytes=-300", TOTAL - 300, TOTAL - 1), ("bytes=0-0", 0, 0),
                  ("bytes=%d-%d" % (TOTAL - 10, TOTAL + 99999), TOTAL - 10, TOTAL - 1)):
    st, h, b = fetch(tl("video", NAME), headers={"Range": rng})
    want_cr = "bytes %d-%d/%d" % (a, z, TOTAL)
    check(st == 206 and h.get("content-range") == want_cr and b == expect(NAME, a, z),
          "Range %s -> %s %s (%d bytes)" % (rng, st, h.get("content-range"), len(b)))
seen = [r.get("range") for r in jbody(fetch(MOCK_URL + "/mock/state")[2]).get("requests", [])]
check("bytes=1000-1999" in seen, "the Range reached the printer (%s)" % seen[-3:])
st, h, b = fetch(tl("video", NAME), headers={"Range": "bytes=%d-" % TOTAL})
check(st == 416 and h.get("content-range") == "bytes */%d" % TOTAL, "range past the end -> 416 (%s %s)" % (st, h.get("content-range")))
st, h, b = fetch(tl("video", NAME, "&download=1"), headers={"Range": "bytes=0-99"})
check(st == 206 and ("attachment" in h.get("content-disposition", "")) and NAME in h.get("content-disposition", ""),
      "download=1 -> Content-Disposition %s" % h.get("content-disposition"))
st, h, b = fetch(tl("video", "nope.mp4"))
check(st == 404 and jbody(b).get("code") == "no_file", "unknown video -> 404 no_file (%s)" % st)
st, h, b = fetch(tl("video", "timelapse_benchy_20261001_1200.jpg"))
check(st == 400 and jbody(b).get("code") == "bad_name", "not a video name -> 400 (%s)" % st)

# A printer that ignores Range: the hub answers the range itself.
fetch(MOCK_URL + "/mock/ranges?on=0")
st, h, b = fetch(tl("video", "timelapse_cube_20260930_1000.mp4"), headers={"Range": "bytes=5000-5999"})
check(st == 206 and h.get("content-range") == "bytes 5000-5999/%d" % SEEDS["timelapse_cube_20260930_1000.mp4"][0]
      and b == expect("timelapse_cube_20260930_1000.mp4", 5000, 5999),
      "printer ignores Range -> hub still answers 206 with the right bytes (%s %s)" % (st, h.get("content-range")))
st, h, b = fetch(tl("video", "timelapse_cube_20260930_1000.mp4"), headers={"Range": "bytes=999999999-"})
check(st == 416, "printer ignores Range, range past the end -> 416 (%s)" % st)
fetch(MOCK_URL + "/mock/ranges?on=1")


# ---- a large file streams; the slicer does not hold it in memory ----
class PMC(ctypes.Structure):
    _fields_ = [("cb", ctypes.c_ulong), ("PageFaultCount", ctypes.c_ulong), ("PeakWorkingSetSize", ctypes.c_size_t),
                ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                ("PeakPagefileUsage", ctypes.c_size_t)]


def private_bytes(pid):
    """The process's committed private memory (PagefileUsage), or None off Windows."""
    if os.name != "nt":
        return None
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return None
    try:
        c = PMC()
        c.cb = ctypes.sizeof(PMC)
        ok = ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb)
        return c.PagefileUsage if ok else None
    finally:
        ctypes.windll.kernel32.CloseHandle(h)


pids = {PID, hubj.get("pid") or PID}
base = {p: private_bytes(p) for p in pids}
peak = dict(base)
stop = threading.Event()


def sample():
    while not stop.is_set():
        for p in pids:
            v = private_bytes(p)
            if v is not None and (peak.get(p) is None or v > peak[p]):
                peak[p] = v
        time.sleep(0.05)


sampler = threading.Thread(target=sample, daemon=True)
sampler.start()
h_ = hashlib.sha256()
got = 0
req = urllib.request.Request(tl("video", "big.mp4"))
t0 = time.time()
try:
    with urllib.request.urlopen(req, timeout=300) as r:
        status = r.status
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            h_.update(chunk)
            got += len(chunk)
except Exception as e:  # noqa: BLE001
    status = "error %s" % e
stop.set()
sampler.join()
big_total = SEEDS["big.mp4"][0]
want = hashlib.sha256(expect("big.mp4", 0, big_total - 1)).hexdigest()
check(status == 200 and got == big_total and h_.hexdigest() == want,
      "big.mp4 (%d MB) streamed whole in %.1f s" % (BIG_MB, time.time() - t0))
for p in pids:
    if base.get(p) is None:
        check(True, "memory of pid %s not measurable here" % p)
        continue
    grew = (peak[p] - base[p]) / (1 << 20)
    check(grew < max(24, BIG_MB / 4), "pid %s private memory grew %.1f MB while %d MB streamed" % (p, grew, BIG_MB))

# A player seeking: an open request closed early, then a later range, all fine.
s = socket.create_connection(("127.0.0.1", hubj["port"]), timeout=30)
path = (tl("video", "big.mp4"))[len(HUB):]
s.sendall(("GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n" % path).encode())
s.recv(65536)
s.close()
st, h, b = fetch(tl("video", "big.mp4"), headers={"Range": "bytes=%d-%d" % (big_total // 2, big_total // 2 + 65535)})
check(st == 206 and b == expect("big.mp4", big_total // 2, big_total // 2 + 65535), "seek after an aborted stream (%s)" % st)

# ---- no timelapse root ----
fetch(MOCK_URL + "/mock/root?on=0")
st, j = api_json(TL)
check(st == 200 and j.get("files") == [] and "timelapse" in (j.get("note") or ""), "no root -> 200, no files, note: %s" % j.get("note"))
fetch(MOCK_URL + "/mock/root?on=1")

# ---- unknown printers, wrong token ----
st, j = api_json("/printers/%s/timelapses" % urllib.parse.quote("sm:nosuch", safe=""))
check(st == 404 and j.get("code") == "no_printer", "sm:nosuch -> 404 no_printer (%s %s)" % (st, j))
st, j = api_json("/printers/01P00A000000000/timelapses")
check(st == 404 and j.get("code") == "no_printer", "unknown Bambu id -> 404 no_printer (%s %s)" % (st, j))
st, h, b = fetch("%s/r/%s/i/%d/api%s" % (HUB, "wrongtoken000", PID, TL))
check(st == 404, "wrong token -> 404 (%s)" % st)

# ---- clean up ----
st, j = api_json("/snapmaker/remove?id=" + urllib.parse.quote(MOCK_ID), "POST")
check(st == 200, "mock removed from the LAN list (%s)" % st)

print("\n%d checks, %d failed" % (checks, len(fails)))
for f in fails:
    print("  FAILED: " + f)
sys.exit(1 if fails else 0)
