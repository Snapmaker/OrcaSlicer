#!/usr/bin/env python3
"""
Snapmaker CLI Batch Slicing Test Tool
Zero-dependency local web app (Python 3.8+ standard library only).
"""
import http.server, json, os, queue, re, shutil, socketserver
import subprocess, threading, time, traceback, webbrowser
from datetime import datetime
from pathlib import Path
from urllib.parse import urlparse, parse_qs

PORT = 18964
HERE = Path(__file__).resolve().parent
PROJECT_ROOT = HERE.parent.parent

def _resolve(path_str):
    """Resolve a relative path against PROJECT_ROOT; absolute paths pass through."""
    p = Path(str(path_str))
    if p.is_absolute():
        return str(p.resolve())
    return str((PROJECT_ROOT / p).resolve())

DEFAULT_CLI_PATH = r"build/src/Release/snapmaker-orca.exe"
DEFAULT_DATADIR = r"resources"
TOOL_LOG_PATH = HERE / "tool.log"

# ---------------------------------------------------------------------------
# CLI binary diagnostics (run once at startup)
# ---------------------------------------------------------------------------
_CLI_DIAG_CACHE = None

def cli_binary_diagnostic(cli_path=None):
    """Quick self-check of the CLI binary. Returns (ok, message)."""
    global _CLI_DIAG_CACHE
    if _CLI_DIAG_CACHE is not None:
        return _CLI_DIAG_CACHE

    exe = _resolve(cli_path or DEFAULT_CLI_PATH)
    exe_path = Path(exe)

    if not exe_path.exists():
        _CLI_DIAG_CACHE = (False, "CLI not found: " + exe)
        return _CLI_DIAG_CACHE

    # Quick smoke test: --allow-newer-file=1 --help must exit 0
    try:
        cp = subprocess.run([exe, "--allow-newer-file=1", "--help"],
                            capture_output=True, text=True, timeout=10)
        if cp.returncode != 0:
            _CLI_DIAG_CACHE = (False, "Binary rejected --help (exit " + str(cp.returncode) + ").")
            return _CLI_DIAG_CACHE
    except subprocess.TimeoutExpired:
        _CLI_DIAG_CACHE = (False, "Binary not responding. Corrupted file?")
        return _CLI_DIAG_CACHE
    except Exception as ex:
        _CLI_DIAG_CACHE = (False, "Binary check failed: " + str(ex))
        return _CLI_DIAG_CACHE

    # Check binary timestamp vs. latest commit in repo
    try:
        mod_ts = exe_path.stat().st_mtime
        mod_time = datetime.datetime.fromtimestamp(mod_ts)
        repo = HERE.parent.parent
        cp = subprocess.run(
            ["git", "log", "-1", "--format=%ct"],
            capture_output=True, text=True, timeout=5,
            cwd=str(repo)
        )
        if cp.returncode == 0 and cp.stdout.strip():
            latest_ts = int(cp.stdout.strip())
            latest_time = datetime.datetime.fromtimestamp(latest_ts)
            age_min = (latest_time - mod_time).total_seconds() / 60.0
            if age_min > 30:
                msg = ("Binary is outdated (built " + mod_time.strftime("%m-%d %H:%M")
                       + ", latest commit " + latest_time.strftime("%m-%d %H:%M")
                       + ", ~" + str(int(age_min)) + " min behind). "
                       + "Run: cmake --build build --target snapmaker-orca --config Release")
                _CLI_DIAG_CACHE = (False, msg)
                return _CLI_DIAG_CACHE
    except Exception:
        pass

    _CLI_DIAG_CACHE = (True, "Binary looks current.")
    return _CLI_DIAG_CACHE


_EXIT_TABLE = {
    0:            ("success", "Success", "G-code generated."),
    0xC0000005:   ("crashed", "SIGSEGV (access violation)", "Null pointer dereference. Verify CLI crash fixes are applied and binary is rebuilt."),
    0xC0000135:   ("crashed", "DLL not found (0xC0000135)", "CLI exe directory must be in PATH for DLL resolution."),
    0xC0000409:   ("crashed", "Stack buffer overrun", "Stack corruption detected."),
    0xC00000FD:   ("crashed", "Stack overflow", "Possible infinite recursion."),
    0xC0000142:   ("crashed", "DLL init failed (0xC0000142)", "A dependent DLL failed to initialize."),
    -1:           ("failed",  "Environment error", "CLI initialization failed."),
    -2:           ("failed",  "Invalid CLI params", "Parameter parsing error."),
    -3:           ("failed",  "File not found", "CLI could not find the input file. Check path encoding."),
    -4:           ("failed",  "File list invalid order", "Input file ordering error."),
    -5:           ("failed",  "Config file error", "Configuration file could not be loaded."),
    -6:           ("failed",  "Data file error", "Data/resource file error. Check --datadir."),
    -7:           ("failed",  "Invalid printer technology", "Printer technology not supported."),
    -8:           ("failed",  "Unsupported operation", "The requested operation is not supported."),
    18:           ("failed",  "Invalid values in 3MF", "Profile missing or contains illegal values."),
    -24:          ("failed",  "File version not supported", "3MF version too high. Add --allow-newer-file."),
    24:           ("failed",  "File version not supported", "3MF version too high. Add --allow-newer-file."),
    50:           ("failed",  "No suitable objects", "No objects within print volume."),
    -51:          ("failed",  "Validation error", "Likely relative extruder mode. Add --use-relative-e-distances=0."),
    51:           ("failed",  "Validation error", "Likely relative extruder mode. Add --use-relative-e-distances=0."),
    52:           ("failed",  "Object partly inside error", "Object partially outside print volume."),
    58:           ("timeout", "Slice time exceeded", "Internal per-plate timeout. Increase --mstpp."),
    59:           ("failed",  "Triangle count exceeded", "Increase --mtcpp."),
    101:          ("failed",  "G-code conflict", "G-code output path conflict."),
    -100:         ("failed",  "Slicing error", "Internal slicer error during processing."),
    -101:         ("failed",  "G-code conflict", "G-code output path conflict."),
}

def _normalize_code(code):
    unsigned = code & 0xFFFFFFFF
    signed = unsigned if unsigned < 0x80000000 else unsigned - 0x100000000
    return signed, unsigned

def analyze_exit_code(code):
    signed, unsigned = _normalize_code(code)
    for c in (code, unsigned, signed):
        if c in _EXIT_TABLE:
            return _EXIT_TABLE[c]
    if unsigned >= 0xC0000000:
        return ("crashed", f"Process crash (0x{unsigned:08X})", "Unhandled exception. Check crash logs.")
    if signed < 0:
        return ("failed", f"CLI error ({signed})", "Unrecognized CLI error code.")
    return ("unknown", f"Exit {code}", "")

_LOG_PATTERNS = [
    (re.compile(r"negative spacing", re.I),       "Flow::spacing() negative spacing", "Geometry degeneracy in the model."),
    (re.compile(r"Nothing to be sliced", re.I),   "Nothing to be sliced",             "Plate shape or object placement issue."),
    (re.compile(r"Wipe tower.*failed", re.I),     "Wipe tower generation failed",     "Multi-material wipe tower geometry error."),
    (re.compile(r"filament_is_support", re.I),    "filament_is_support mismatch",     "Filament support count mismatch in 3MF config."),
    (re.compile(r"relative.*extrud", re.I),       "Relative extruder error",          "Add --use-relative-e-distances=0."),
    (re.compile(r"triangle.*exceed", re.I),       "Triangle count exceeded",          "Increase --mtcpp."),
    (re.compile(r"version.*not.*support", re.I),  "File version unsupported",         "Add --allow-newer-file."),
    (re.compile(r"SlicingError", re.I),           "Slicing engine error",             "Internal slicer error during processing."),
]

def analyze_log(log_text):
    hits = []
    for pattern, desc, suggestion in _LOG_PATTERNS:
        if pattern.search(log_text):
            hits.append({"keyword": desc, "suggestion": suggestion})
    return hits


def extract_log_summary(log_lines, status="unknown"):
    """Extract important/informative lines from the full log for quick review."""
    if not log_lines:
        return []
    important = []
    # Always include the CLI command (first line)
    if log_lines and log_lines[0].startswith("$ "):
        important.append(log_lines[0])
        important.append("")
    # Patterns that indicate important lines
    key_patterns = [
        "warning", "error", "critical", "fail", "exception",
        "G92 E0", "incompatible", "validation", "cannot",
        "Will start to read model", "Successfully",
        "gcode file", "sliced", "slice finished",
        "total cost", "estimated", "used time",
    ]
    seen = set()
    for line in log_lines:
        line_lower = line.lower().strip()
        if not line_lower or line_lower in seen:
            continue
        for pat in key_patterns:
            if pat.lower() in line_lower:
                important.append(line)
                seen.add(line_lower)
                break
    # Last 3 non-empty lines (summary info)
    tail = []
    for line in reversed(log_lines):
        if line.strip():
            tail.append(line)
            if len(tail) >= 3:
                break
    for line in reversed(tail):
        lk = line.strip().lower()
        if lk and lk not in seen:
            important.append(line)
            seen.add(lk)
    # Deduplicate while preserving order
    result = []
    seen2 = set()
    for line in important:
        lk = line.strip().lower()
        if lk and lk not in seen2:
            result.append(line)
            seen2.add(lk)
    return result


class EventBroker:
    def __init__(self):
        self._subscribers = []
        self._lock = threading.Lock()
    def subscribe(self):
        q = queue.Queue()
        with self._lock:
            self._subscribers.append(q)
        return q
    def unsubscribe(self, q):
        with self._lock:
            if q in self._subscribers:
                self._subscribers.remove(q)
    def publish(self, event):
        with self._lock:
            subs = list(self._subscribers)
        for q in subs:
            q.put(event)

broker = EventBroker()

def tool_log(msg):
    ts = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    line = f"[{ts}] {msg}"
    try:
        with open(TOOL_LOG_PATH, "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception:
        pass
    try:
        print(line, flush=True)
    except OSError:
        pass

def _find_crash_log(cli_dir):
    log_dir = Path(cli_dir) / "log"
    if not log_dir.exists():
        return None
    crash_logs = sorted(log_dir.glob("crash_*.log"), key=lambda f: f.stat().st_mtime, reverse=True)
    if not crash_logs:
        return None
    newest = crash_logs[0]
    age = time.time() - newest.stat().st_mtime
    if age > 120:
        return None
    try:
        content = newest.read_text(encoding="utf-8", errors="replace")
    except Exception:
        content = "(unable to read)"
    return {"name": newest.name, "content": content.strip(), "age_seconds": round(age)}

class SliceResult:
    def __init__(self, file_path):
        self.file_path = file_path
        self.file_name = Path(file_path).name
        self.file_size = Path(file_path).stat().st_size if Path(file_path).exists() else 0
        self.status = "pending"
        self.exit_code = None
        self.category = None
        self.label = None
        self.suggestion = None
        self.duration = 0.0
        self.log_lines = []
        self.gcode_files = []
        self.gcode_total_size = 0
        self.error_keywords = []
        self.log_summary = []
        self.started_at = None
        self.finished_at = None
        self._start_ts = 0.0
        self.progress_pct = 0
        self.stage_label = ""
        self.quality_scores = []
    def to_dict(self):
        return {
            "file_path": self.file_path, "file_name": self.file_name,
            "file_size": self.file_size, "status": self.status,
            "exit_code": self.exit_code, "category": self.category,
            "label": self.label, "suggestion": self.suggestion,
            "duration": round(self.duration, 2),
            "log": "\n".join(self.log_lines),
            "gcode_files": self.gcode_files, "gcode_total_size": self.gcode_total_size,
            "error_keywords": self.error_keywords,
            "log_summary": self.log_summary,
            "started_at": self.started_at, "finished_at": self.finished_at,
            "progress_pct": self.progress_pct, "stage_label": self.stage_label,
            "quality_scores": self.quality_scores,
        }

class SlicingSession:
    def __init__(self):
        self.results = []
        self.state = "idle"
        self.config = {}
        self.started_at = None
        self.finished_at = None
        self.current_index = -1
        self.current_file = ""
        self.live_log = []
        self._stop_flag = threading.Event()
        self._thread = None
        self._current_proc = None
        self._proc_lock = threading.Lock()
    @property
    def summary(self):
        total = len(self.results)
        counts = {}
        for r in self.results:
            counts[r.status] = counts.get(r.status, 0) + 1
        success = counts.get("success", 0)
        rate = (success / total * 100) if total else 0
        return {
            "total": total, "success": success,
            "failed": counts.get("failed", 0),
            "timeout": counts.get("timeout", 0),
            "crashed": counts.get("crashed", 0),
            "skipped": counts.get("skipped", 0),
            "success_rate": round(rate, 1),
            "total_duration": round(sum(r.duration for r in self.results), 2),
            "state": self.state,
            "started_at": self.started_at, "finished_at": self.finished_at,
            "current_index": self.current_index,
        }
    def to_dict(self):
        return {
            "summary": self.summary,
            "config": self.config,
            "results": [r.to_dict() for r in self.results],
            "live_log": self.live_log[-300:],
            "current_file": self.current_file,
        }

session = SlicingSession()
def scan_3mf_files(path):
    p = Path(path)
    if not p.exists():
        return []
    if p.is_file() and p.suffix.lower() == ".3mf":
        return [str(p)]
    if p.is_dir():
        return sorted(str(f) for f in p.rglob("*.3mf") if f.is_file())
    return []

# ---------------------------------------------------------------------------
# Slicing stage detection (for per-card progress bar)
# ---------------------------------------------------------------------------
_STAGE_PATTERNS = [
    (re.compile(r"Will start to read model file", re.I),  5,  "读取模型文件"),
    (re.compile(r"import 3mf IMPORT_STAGE_OPEN", re.I),   10, "打开 3MF"),
    (re.compile(r"IMPORT_STAGE_READ_FILES", re.I),        15, "读取文件"),
    (re.compile(r"load_3mf", re.I),                       20, "加载 3MF"),
    (re.compile(r"load.*config|load.*preset", re.I),      30, "加载配置"),
    (re.compile(r"Generating ground geometry", re.I),     35, "生成几何"),
    (re.compile(r"validat|normative", re.I),              40, "参数校验"),
    (re.compile(r"Fill|infill", re.I),                    55, "填充"),
    (re.compile(r"Wall|wall_loops|perimeter", re.I),      50, "外壁"),
    (re.compile(r"Slicing|layer_height|layer result", re.I), 45, "切片"),
    (re.compile(r"Generat.*gcode|export.*gcode|gcode.*file", re.I), 70, "生成 G-code"),
    (re.compile(r"wipe.*tower|brim|skirt|support", re.I), 60, "生成辅助结构"),
    (re.compile(r"Successfully|finished|done|completed", re.I), 95, "完成"),
]

def _detect_stage(line, current_pct):
    """Return (new_pct, stage_label) from a CLI output line."""
    for pattern, pct, label in _STAGE_PATTERNS:
        if pattern.search(line):
            if pct > current_pct:
                return pct, label
    return current_pct, None

# ---------------------------------------------------------------------------
# Gcode-diff quality scoring
# ---------------------------------------------------------------------------
_GCODE_DIFF_DEV = r"C:\workDir\programing\Gcode-diff\gcode-diff-rs\target\release\gcode-diff.exe"

def _find_gcode_diff():
    """Resolve gcode-diff.exe: bundled next to tool, then tools/ dir, then dev path."""
    candidates = [
        HERE / "gcode-diff.exe",
        HERE.parent / "gcode-diff.exe",
        PROJECT_ROOT / "tools" / "gcode-diff.exe",
        Path(_GCODE_DIFF_DEV),
    ]
    for c in candidates:
        if c.exists():
            return str(c)
    return str(candidates[-1])

GCODE_DIFF_PATH = _find_gcode_diff()

def _run_gcode_score(gcode_path, out_dir):
    """Run gcode-diff --score on a gcode file. Returns dict with score + html path, or None."""
    if not Path(GCODE_DIFF_PATH).exists():
        return None
    gcode_name = Path(gcode_path).stem
    html_out = str(Path(out_dir) / f"quality_{gcode_name}.html")
    json_out = str(Path(out_dir) / f"quality_{gcode_name}.json")
    try:
        r = subprocess.run(
            [GCODE_DIFF_PATH, "--score", gcode_path, "--format", "json", "--no-overhang-viz"],
            capture_output=True, text=True, timeout=120,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        # gcode-diff exits 0 (pass) or 2 (below threshold) -- both mean it ran successfully
        if r.returncode not in (0, 2):
            return None
        # The Rust binary writes score JSON to stdout (not to --json file)
        stdout = r.stdout.strip()
        if not stdout:
            return None
        data = json.loads(stdout)
        score = data.get("overall_score") or data.get("score")
        if score is None:
            return None
        # Save parsed JSON + generated HTML to disk
        Path(json_out).write_text(json.dumps(data, indent=2, ensure_ascii=False), encoding="utf-8")
        Path(html_out).write_text(_generate_quality_html(data, Path(gcode_path).name), encoding="utf-8")
        return {"score": score, "html": html_out, "json": json_out, "data": data}
    except Exception as ex:
        tool_log("gcode-diff score failed for " + gcode_path + ": " + str(ex))
        pass
    return None

def _generate_quality_html(data, gcode_name):
    """Build a standalone HTML quality report from gcode-diff score JSON."""
    score = data.get("overall_score") or data.get("score", 0)
    summary = data.get("summary", "")
    has_critical = data.get("has_critical", False)
    material = data.get("material", "")
    slicer_info = data.get("slicer", "")
    printer = data.get("printer_model", "")
    physics_trust = data.get("physics_trust", "")
    dimensions = data.get("dimensions", [])
    if score >= 80:
        sc = "#34c759"
    elif score >= 60:
        sc = "#ff9500"
    else:
        sc = "#ff3b30"
    rows = ""
    for d in dimensions:
        ds = d.get("score", 0)
        if isinstance(ds, (int, float)):
            if ds >= 80:
                dc = "#34c759"
            elif ds >= 60:
                dc = "#ff9500"
            else:
                dc = "#ff3b30"
            ds_str = "{:.1f}".format(ds)
        else:
            dc = "#86868b"
            ds_str = str(ds)
        sev = d.get("severity", "")
        rows += "<tr><td>" + str(d.get("name", "")) + "</td>"
        rows += "<td style='text-align:center;font-weight:700;color:" + dc + "'>" + ds_str + "</td>"
        rows += "<td>" + sev + "</td>"
        rows += "<td>" + str(d.get("verdict", "")) + "</td>"
        rows += "<td>" + str(d.get("recommendation", "")) + "</td></tr>"
    crit = '<span class="crit">CRITICAL</span>' if has_critical else ""
    parts = []
    parts.append("<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>")
    parts.append("<title>Quality Report - " + gcode_name + "</title>")
    parts.append("<style>")
    parts.append("body{font-family:-apple-system,'Segoe UI',Roboto,sans-serif;background:#f0f0f3;color:#1d1d1f;margin:0;padding:24px}")
    parts.append("h1{font-size:20px;margin-bottom:4px}")
    parts.append(".meta{color:#86868b;font-size:13px;margin-bottom:20px;line-height:1.6}")
    parts.append(".score-card{background:#fff;border-radius:8px;padding:24px;text-align:center;margin-bottom:20px;box-shadow:0 1px 3px rgba(0,0,0,.08)}")
    parts.append(".score-num{font-size:48px;font-weight:800;color:" + sc + "}")
    parts.append(".score-lbl{font-size:18px;font-weight:600;margin-top:4px}")
    parts.append("table{width:100%;border-collapse:collapse;background:#fff;border-radius:8px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,.08)}")
    parts.append("th{padding:10px 14px;text-align:left;font-size:12px;text-transform:uppercase;letter-spacing:.5px;color:#86868b;border-bottom:2px solid #e0e0e5}")
    parts.append("td{padding:10px 14px;font-size:13px;border-bottom:1px solid #f0f0f3;vertical-align:top}")
    parts.append("tr:last-child td{border-bottom:none}")
    parts.append(".crit{display:inline-block;padding:2px 10px;border-radius:4px;font-size:11px;font-weight:700;color:#fff;background:#ff3b30;margin-left:8px}")
    parts.append("</style></head><body>")
    parts.append("<h1>G-code Quality Report</h1>")
    parts.append("<div class='meta'>")
    parts.append(gcode_name + "<br>")
    if slicer_info:
        parts.append("Slicer: " + slicer_info + "<br>")
    if printer:
        parts.append("Printer: " + printer + "<br>")
    if material:
        parts.append("Material: " + material + "<br>")
    if physics_trust:
        parts.append("Physics: " + physics_trust)
    parts.append("</div>")
    parts.append("<div class='score-card'><div class='score-num'>" + "{:.1f}".format(score) + "</div>")
    parts.append("<div class='score-lbl'>" + str(summary) + crit + "</div></div>")
    parts.append("<table><thead><tr><th>Dimension</th><th style='text-align:center'>Score</th><th>Severity</th><th>Verdict</th><th>Recommendation</th></tr></thead>")
    parts.append("<tbody>" + rows + "</tbody></table>")
    parts.append("</body></html>")
    return "\n".join(parts)

# ---------------------------------------------------------------------------
# Per-file report generation
# ---------------------------------------------------------------------------
def _generate_single_report(result, output_base):
    """Generate a JSON report for a single completed slice + run gcode-diff if applicable."""
    report_dir = Path(output_base) / "_reports"
    report_dir.mkdir(parents=True, exist_ok=True)
    file_name_safe = re.sub(r"[^\w\-.]", "_", result.file_name)
    gcode_dir_name = re.sub(r"[^\w\-.]", "_", Path(result.file_name).stem)
    gcode_dir = Path(output_base) / gcode_dir_name
    json_path = report_dir / f"{file_name_safe}.json"
    # Run gcode-diff on each generated gcode
    quality_scores = []
    if gcode_dir.exists():
        for gc in sorted(gcode_dir.glob("*.gcode")):
            result_info = _run_gcode_score(str(gc), str(gcode_dir))
            if result_info:
                rdata = result_info.get("data", {})
                quality_scores.append({
                    "file": gc.name,
                    "plate": Path(gc.name).stem,
                    "score": result_info["score"],
                    "summary": rdata.get("summary", ""),
                    "has_critical": rdata.get("has_critical", False),
                    "material": rdata.get("material", ""),
                    "html": str(Path(result_info["html"]).relative_to(output_base)),
                    "dimensions": rdata.get("dimensions", []),
                })
                result.log_lines.append(f"[QUALITY] {gc.name}: score={result_info['score']}")
    result.quality_scores = quality_scores
    report_data = result.to_dict()
    report_data["quality_scores"] = quality_scores
    try:
        json_path.write_text(json.dumps(report_data, indent=2, ensure_ascii=False), encoding="utf-8")
    except Exception:
        pass

def build_cli_command(file_path, config, output_dir):
    cli = _resolve(config["cli_path"])
    cmd = [cli]
    cmd += ["--datadir", _resolve(config["datadir"])]
    cmd += ["--outputdir", output_dir]
    cmd += ["--slice", str(config.get("slice_mode", 0))]
    mstpp = config.get("mstpp", 0)
    if mstpp and int(mstpp) > 0:
        cmd += ["--mstpp", str(int(mstpp))]
    mtcpp = config.get("mtcpp", 0)
    if mtcpp and int(mtcpp) > 0:
        cmd += ["--mtcpp", str(int(mtcpp))]
    debug = config.get("debug", 3)
    if debug is not None and int(debug) >= 0:
        cmd += ["--debug", str(int(debug))]
    if config.get("allow_newer_file", True):
        cmd += ["--allow-newer-file"]
    if config.get("no_relative_e", False):
        cmd += ["--use-relative-e-distances=0"]
    extra = config.get("extra_args", "").strip()
    if extra:
        cmd += extra.split()
    cmd += [file_path]
    return cmd

def run_one_slice(result, config, output_base):
    """Slice one file, streaming output, tracking stage progress, auto-retry on relative-e error."""
    file_name_safe = re.sub(r"[^\w\-.]", "_", Path(result.file_path).stem)
    out_dir = str(Path(output_base) / file_name_safe)
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    result.started_at = datetime.now().isoformat()
    result.status = "running"
    result.progress_pct = 0
    result.stage_label = "启动中"
    result._start_ts = time.time()
    tool_log(f"START slicing: {result.file_path}")
    broker.publish({"type": "slice_started", "file": result.file_name})

    # First attempt
    _do_slice_subprocess(result, config, out_dir)

    # Auto-retry: if failed with relative extruder error, retry with --use-relative-e-distances=0
    if result.exit_code and result.exit_code != 0:
        full_log = "\n".join(result.log_lines)
        if "relative" in full_log.lower() and "extrud" in full_log.lower():
            tool_log(f"RETRY {result.file_name} with --use-relative-e-distances=0")
            result.log_lines.append("")
            result.log_lines.append("[TOOL] Auto-retry with --use-relative-e-distances=0")
            result.log_lines.append("")
            result.progress_pct = 0
            result.stage_label = "重试中"
            # Clone config and force the flag
            retry_config = dict(config)
            retry_config["no_relative_e"] = True
            # Reset result state
            result.log_lines = result.log_lines[:2]  # keep command header
            _do_slice_subprocess(result, retry_config, out_dir)
            if result.exit_code == 0:
                result.label = "Success (auto-retry with --use-relative-e-distances=0)"

    # Detect gcode files
    gcodes = list(Path(out_dir).glob("*.gcode"))
    result.gcode_files = [str(f.name) for f in gcodes]
    result.gcode_total_size = sum(f.stat().st_size for f in gcodes)

    full_log = "\n".join(result.log_lines)
    result.error_keywords = analyze_log(full_log)
    result.log_summary = extract_log_summary(result.log_lines, result.status)
    result.progress_pct = 100 if result.status == "success" else result.progress_pct
    result.stage_label = "完成" if result.status == "success" else result.stage_label
    result.finished_at = datetime.now().isoformat()
    broker.publish({"type": "slice_done", "file": result.file_name, "result": result.to_dict()})


def _do_slice_subprocess(result, config, out_dir):
    """Run the CLI subprocess, streaming output and tracking stage. Updates result in-place."""
    cmd = build_cli_command(result.file_path, config, out_dir)
    env = os.environ.copy()
    cli_dir = str(Path(_resolve(config["cli_path"])).parent)
    env["PATH"] = cli_dir + os.pathsep + env.get("PATH", "")
    if config.get("allow_newer_file", True):
        env["SNAPMAKER_ORCA_ALLOW_NEWER_FILE"] = "1"
    cmd_display = " ".join(f'"{a}"' if " " in a else a for a in cmd)
    result.log_lines.append(f"$ {cmd_display}")
    result.log_lines.append("")
    timeout = int(config.get("timeout", 600))
    try:
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env,
            cwd=cli_dir,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0,
        )
        # Register proc so stop can kill it
        with session._proc_lock:
            session._current_proc = proc
        _line_q = queue.Queue()
        def _reader():
            for raw in iter(proc.stdout.readline, b""):
                _line_q.put(raw)
            _line_q.put(None)
        _reader_t = threading.Thread(target=_reader, daemon=True)
        _reader_t.start()
        deadline = time.time() + timeout
        timed_out = False
        stopped = False
        while True:
            try:
                raw = _line_q.get(timeout=0.3)
            except queue.Empty:
                if session._stop_flag.is_set():
                    stopped = True
                    break
                if proc.poll() is not None:
                    break
                if time.time() > deadline:
                    timed_out = True
                    break
                continue
            if raw is None:
                break
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            result.log_lines.append(line)
            session.live_log.append(line)
            # Stage detection
            new_pct, stage = _detect_stage(line, result.progress_pct)
            if stage:
                result.progress_pct = new_pct
                result.stage_label = stage
                broker.publish({"type": "slice_progress", "file": result.file_name,
                               "pct": result.progress_pct, "stage": result.stage_label})
        # Unregister proc
        with session._proc_lock:
            session._current_proc = None
        if stopped:
            proc.kill()
            proc.wait()
            result.log_lines.append("\n[TOOL] Stopped by user.")
            tool_log(f"STOPPED: {result.file_name}")
            result.exit_code = -998
            result.duration = time.time() - result._start_ts
            result.status = "skipped"
            result.category = "skipped"
            result.label = "用户停止"
            result.stage_label = "已停止"
            return
        if timed_out:
            proc.kill()
            proc.wait()
            result.log_lines.append(f"\n[TOOL] Process killed after {timeout}s timeout.")
            tool_log(f"TIMEOUT: {result.file_name} after {timeout}s")
            result.exit_code = -999
            result.duration = time.time() - result._start_ts
            result.status = "timeout"
            result.category = "timeout"
            result.label = f"Tool timeout ({timeout}s)"
            result.suggestion = "Increase timeout or investigate model complexity."
            result.stage_label = "超时"
            return
        proc.wait()
        result.exit_code = proc.returncode
        result.duration = time.time() - result._start_ts
        tool_log(f"DONE: {result.file_name} exit={result.exit_code} (0x{result.exit_code & 0xFFFFFFFF:08X})")
    except FileNotFoundError:
        result.log_lines.append(f"[TOOL ERROR] CLI executable not found: {config['cli_path']}")
        tool_log(f"ERROR: CLI not found: {config['cli_path']}")
        result.duration = time.time() - result._start_ts
        result.exit_code = -1
        result.status = "failed"
        result.category = "failed"
        result.label = "CLI not found"
        result.suggestion = "Check the CLI path in settings."
        result.stage_label = "CLI未找到"
        return
    except Exception as ex:
        result.log_lines.append(f"[TOOL ERROR] {ex}")
        result.log_lines.append(traceback.format_exc())
        tool_log(f"EXCEPTION: {result.file_name}: {ex}")
        result.duration = time.time() - result._start_ts
        result.exit_code = -100
        result.status = "failed"
        result.category = "failed"
        result.label = f"Tool exception: {ex}"
        result.stage_label = "异常"
        return
    cat, label, suggestion = analyze_exit_code(result.exit_code)
    result.category = cat
    result.label = label
    result.suggestion = suggestion
    if result.exit_code == 0:
        result.status = "success"
    elif cat == "crashed":
        result.status = "crashed"
        crash_log = _find_crash_log(cli_dir)
        if crash_log:
            result.log_lines.append(f"\n[CRASH LOG] {crash_log['name']}")
            result.log_lines.append(crash_log['content'])
            result.suggestion = (result.suggestion or "") + f" Crash log: {crash_log['name']}"
            tool_log(f"CRASH LOG found: {crash_log['name']}")
    elif cat == "timeout":
        result.status = "timeout"
    else:
        result.status = "failed"


def _run_batch(files, config):
    global session
    output_base = _resolve(config.get("output_dir", str(PROJECT_ROOT / "slice_output")))
    Path(output_base).mkdir(parents=True, exist_ok=True)
    session.results = [SliceResult(f) for f in files]
    session.config = config
    session.state = "running"
    session.started_at = datetime.now().isoformat()
    session.finished_at = None
    session.current_file = ""
    session.live_log = []
    session._stop_flag.clear()
    session._current_proc = None
    broker.publish({"type": "session_started", "total": len(files)})
    for i, result in enumerate(session.results):
        if session._stop_flag.is_set():
            result.status = "skipped"
            result.label = "已跳过"
            result.stage_label = "已跳过"
            broker.publish({"type": "slice_done", "file": result.file_name, "result": result.to_dict()})
            continue
        session.current_index = i
        session.current_file = result.file_name
        broker.publish({"type": "progress", "current": i + 1, "total": len(files), "file": result.file_name})
        try:
            run_one_slice(result, config, output_base)
        except Exception as ex:
            result.log_lines.append(f"[TOOL ERROR] Unhandled: {ex}")
            result.log_lines.append(traceback.format_exc())
            result.duration = time.time() - result._start_ts
            result.status = "failed"
            result.category = "failed"
            result.label = f"Unhandled: {ex}"
            result.finished_at = datetime.now().isoformat()
            broker.publish({"type": "slice_done", "file": result.file_name, "result": result.to_dict()})
        # Generate per-file report immediately after each slice
        if result.status not in ("skipped", "pending"):
            _generate_single_report(result, output_base)
        # Append to session live_log
        session.live_log.append(f"=== {result.file_name} ({result.status}) ===")
        session.live_log.extend(result.log_lines[-20:])
        session.live_log.append("")
        session.current_file = ""
    session.state = "stopped" if session._stop_flag.is_set() else "done"
    session.finished_at = datetime.now().isoformat()
    # Always generate final session report (even on stop)
    report_dir = Path(output_base) / "_reports"
    report_dir.mkdir(parents=True, exist_ok=True)
    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    json_path = report_dir / f"report_{ts}.json"
    try:
        json_path.write_text(json.dumps(session.to_dict(), indent=2, ensure_ascii=False), encoding="utf-8")
    except Exception:
        pass
    tool_log(f"SESSION {session.state}: report saved to {json_path}")
    broker.publish({"type": "session_done", "summary": session.summary})
    session._thread = None


def start_batch(files, config):
    if session.state == "running":
        return False, "A session is already running."
    if not files:
        return False, "No 3MF files selected."
    if not Path(_resolve(config.get("cli_path", ""))).exists():
        return False, f"CLI not found: {config.get('cli_path')}"
    t = threading.Thread(target=_run_batch, args=(files, config), daemon=True)
    session._thread = t
    t.start()
    return True, "Session started."

def stop_batch():
    session._stop_flag.set()
    # Kill the currently running subprocess immediately
    with session._proc_lock:
        proc = session._current_proc
    if proc:
        try:
            proc.kill()
            tool_log("Killed current slicing subprocess on stop")
        except Exception:
            pass
    return True

def reset_session():
    """Clear all session state for a fresh start."""
    if session.state == "running":
        return False
    session.results = []
    session.state = "idle"
    session.config = {}
    session.started_at = None
    session.finished_at = None
    session.current_index = -1
    session.current_file = ""
    session.live_log = []
    return True

def native_pick(item_type, filetype=None):
    """Native Windows file/folder picker using Win32 API (thread-safe).
    Returns (path, error). path is None when cancelled or on error.
    """
    try:
        if item_type == "dir":
            return _win32_pick_folder()
        elif item_type == "file":
            return _win32_pick_file(filetype)
        elif item_type == "save":
            return _win32_pick_save()
        else:
            return (None, "Unknown pick type: %s" % item_type)
    except Exception as ex:
        import traceback
        tb = traceback.format_exc()
        tool_log("PICKER CRASH: type=%s err=%s" % (item_type, ex))
        for line in tb.splitlines()[-5:]:
            tool_log("  " + line.strip())
        return (None, "Picker error: %s" % ex)


def _win32_pick_folder():
    """Pick a directory via tkinter in a subprocess (each process has own message pump)."""
    import subprocess, sys, json
    code = "import ctypes; ctypes.windll.shcore.SetProcessDpiAwareness(1); import tkinter as tk, tkinter.filedialog, json; root = tk.Tk(); root.withdraw(); root.attributes('-topmost', True); path = tkinter.filedialog.askdirectory(title='选择目录'); root.destroy(); print(json.dumps({'path': path}))"
    try:
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=30)
        data = json.loads(r.stdout.strip() or "{}")
        p = data.get("path")
        return (p, None) if p else (None, None)
    except subprocess.TimeoutExpired:
        return (None, "Folder dialog timed out")
    except Exception as ex:
        return (None, "Folder picker error: " + str(ex))


def _win32_pick_file(filetype=None):
    """Pick a file via tkinter in a subprocess."""
    import subprocess, sys, json
    if filetype == "exe":
        title = "Select snapmaker-orca.exe"
        ftypes = "[('Executable files', '*.exe'), ('All files', '*.*')]"
    elif filetype == "3mf":
        title = "Select 3MF file"
        ftypes = "[('3MF files', '*.3mf'), ('All files', '*.*')]"
    else:
        title = "Select file"
        ftypes = "[('All files', '*.*')]"
    code = "import ctypes; ctypes.windll.shcore.SetProcessDpiAwareness(1); import tkinter as tk, tkinter.filedialog, json; root = tk.Tk(); root.withdraw(); root.attributes('-topmost', True); path = tkinter.filedialog.askopenfilename(title='" + title + "', filetypes=" + ftypes + "); root.destroy(); print(json.dumps({'path': path}))"
    try:
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=30)
        data = json.loads(r.stdout.strip() or "{}")
        p = data.get("path")
        return (p, None) if p else (None, None)
    except subprocess.TimeoutExpired:
        return (None, "File dialog timed out")
    except Exception as ex:
        return (None, "File picker error: " + str(ex))


def _win32_pick_save():
    """Pick a save path via tkinter in a subprocess."""
    import subprocess, sys, json
    code = "import ctypes; ctypes.windll.shcore.SetProcessDpiAwareness(1); import tkinter as tk, tkinter.filedialog, json; root = tk.Tk(); root.withdraw(); root.attributes('-topmost', True); path = tkinter.filedialog.asksaveasfilename(title='保存报告', defaultextension='.html', filetypes=[('HTML report', '*.html'), ('JSON data', '*.json')]); root.destroy(); print(json.dumps({'path': path}))"
    try:
        r = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True, timeout=30)
        data = json.loads(r.stdout.strip() or "{}")
        p = data.get("path")
        return (p, None) if p else (None, None)
    except subprocess.TimeoutExpired:
        return (None, "Save dialog timed out")
    except Exception as ex:
        return (None, "Save picker error: " + str(ex))
def generate_html_report():
    data = session.to_dict()
    s = data["summary"]
    results = data["results"]
    def status_badge(status):
        colors = {"success": "#34c759", "failed": "#ff3b30", "timeout": "#ff9500",
                  "crashed": "#af52de", "skipped": "#8e8e93", "pending": "#8e8e93", "running": "#007aff"}
        color = colors.get(status, "#8e8e93")
        return f'<span style="background:{color};color:#fff;padding:2px 10px;border-radius:4px;font-size:12px;font-weight:600;">{status.upper()}</span>'
    def fmt_size(n):
        if n < 1024: return f"{n} B"
        if n < 1048576: return f"{n / 1024:.1f} KB"
        return f"{n / 1048576:.2f} MB"
    cards = []
    for r in results:
        log_escaped = (r.get("log", "") or "").replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
        gcode_str = ", ".join(r.get("gcode_files", [])) or "N/A"
        keywords_html = ""
        for kw in r.get("error_keywords", []):
            keywords_html += f'<div class="kw"><strong>{kw["keyword"]}</strong> &mdash; {kw["suggestion"]}</div>'
        suggestion = r.get("suggestion", "") or ""
        suggestion_html = f'<div class="suggestion">{suggestion}</div>' if suggestion else ""
        log_lines_count = len((r.get("log", "") or "").splitlines())
        cards.append(f'''<div class="card"><div class="card-header"><span class="fname">{r["file_name"]}</span>{status_badge(r["status"])}<span class="duration">{r.get("duration", 0):.1f}s</span><span class="exitcode">exit {r.get("exit_code", "N/A")}</span></div><div class="card-meta"><span>{fmt_size(r.get("file_size", 0))}</span><span>G-code: {gcode_str} ({fmt_size(r.get("gcode_total_size", 0))})</span><span>{r.get("label", "")}</span></div>{suggestion_html}{keywords_html}<details><summary>Full log ({log_lines_count} lines)</summary><pre class="log">{log_escaped}</pre></details></div>''')
    total = s["total"]
    return f'''<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><title>Slicing Report</title><style>body{{font-family:-apple-system,Segoe UI,sans-serif;background:#f5f5f7;color:#1d1d1f;margin:0;padding:24px}}h1{{font-size:24px;margin:0 0 4px}}.meta{{color:#86868b;font-size:14px;margin-bottom:24px}}.stats{{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px;margin-bottom:24px}}.stat{{background:#fff;border-radius:8px;padding:16px;text-align:center}}.stat .num{{font-size:28px;font-weight:700}}.stat .lbl{{font-size:12px;color:#86868b;text-transform:uppercase}}.card{{background:#fff;border-radius:8px;padding:16px;margin-bottom:12px}}.card-header{{display:flex;align-items:center;gap:12px;flex-wrap:wrap}}.fname{{font-weight:600;flex:1;min-width:200px}}.duration,.exitcode{{color:#86868b;font-size:13px;font-family:monospace}}.card-meta{{display:flex;gap:16px;margin:8px 0;font-size:13px;color:#555;flex-wrap:wrap}}.suggestion{{background:#fff3cd;padding:8px 12px;border-radius:4px;margin:8px 0;font-size:13px}}.kw{{font-size:13px;color:#555;margin:4px 0}}pre.log{{background:#1d1d1f;color:#e0e0e0;padding:12px;border-radius:6px;overflow:auto;font-size:12px;max-height:400px}}details summary{{cursor:pointer;color:#007aff;font-size:13px;margin-top:8px}}</style></head><body><h1>Snapmaker CLI Slicing Report</h1><div class="meta">Generated: {datetime.now().strftime("%Y-%m-%d %H:%M:%S")} | Session: {session.started_at or "N/A"}</div><div class="stats"><div class="stat"><div class="num">{total}</div><div class="lbl">Total</div></div><div class="stat"><div class="num" style="color:#34c759">{s["success"]}</div><div class="lbl">Success</div></div><div class="stat"><div class="num" style="color:#ff3b30">{s["failed"]}</div><div class="lbl">Failed</div></div><div class="stat"><div class="num" style="color:#ff9500">{s["timeout"]}</div><div class="lbl">Timeout</div></div><div class="stat"><div class="num" style="color:#af52de">{s["crashed"]}</div><div class="lbl">Crashed</div></div><div class="stat"><div class="num">{s["success_rate"]}%</div><div class="lbl">Success Rate</div></div><div class="stat"><div class="num">{s["total_duration"]:.0f}s</div><div class="lbl">Total Time</div></div></div>{"".join(cards)}</body></html>'''

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "SnapmakerCLITester/1.0"
    def _send_json(self, data, code=200):
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def _send_html(self, text, code=200):
        body = text.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def _read_body(self):
        length = int(self.headers.get("Content-Length", 0))
        if length == 0: return {}
        raw = self.rfile.read(length)
        return json.loads(raw.decode("utf-8"))
    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/" or path == "/index.html":
            html_path = HERE / "templates" / "index.html"
            self._send_html(html_path.read_text(encoding="utf-8"))
        elif path == "/api/defaults":
            self._send_json({"cli_path": DEFAULT_CLI_PATH, "datadir": DEFAULT_DATADIR})
        elif path == "/api/diag":
            d_ok, d_msg = cli_binary_diagnostic(
                _resolve(self.headers.get("X-Cli-Path") or DEFAULT_CLI_PATH))
            self._send_json({"ok": d_ok, "message": d_msg})
        elif path == "/api/session":
            self._send_json(session.to_dict())
        elif path == "/api/stream":
            self._handle_sse()
        elif path == "/api/report/export":
            self._send_html(generate_html_report())
        elif path == "/api/quality":
            qs = parse_qs(urlparse(self.path).query)
            file_name = qs.get("file", [""])[0]
            plate = qs.get("plate", [""])[0]
            if not file_name or not plate:
                self._send_json({"error": "file and plate required"}, 400)
                return
            output_base = _resolve(session.config.get("output_dir", str(PROJECT_ROOT / "slice_output")))
            gcode_dir_name = re.sub(r"[^\w\-.]", "_", Path(file_name).stem)
            html_file = Path(output_base) / gcode_dir_name / f"quality_{plate}.html"
            if html_file.exists():
                self._send_html(html_file.read_text(encoding="utf-8"))
            else:
                self._send_json({"error": "report not found: " + str(html_file)}, 404)
        else:
            self._send_json({"error": "not found"}, 404)
    def do_POST(self):
        path = urlparse(self.path).path
        tool_log("POST: " + path)
        try:
            if path == "/api/browse":
                body = self._read_body()
                result = native_pick(body.get("type", "dir"), body.get("filetype"))
                if isinstance(result, tuple):
                    self._send_json({"path": result[0], "error": result[1]})
                else:
                    self._send_json({"path": result})
            elif path == "/api/scan":
                body = self._read_body()
                files = scan_3mf_files(body.get("path", ""))
                self._send_json({"files": files, "count": len(files)})
            elif path == "/api/start":
                body = self._read_body()
                ok, msg = start_batch(body.get("files", []), body.get("config", {}))
                self._send_json({"ok": ok, "message": msg})
            elif path == "/api/stop":
                self._send_json({"ok": stop_batch()})
            elif path == "/api/reset":
                ok = reset_session()
                self._send_json({"ok": ok, "message": "Session cleared." if ok else "Cannot reset while running."})
            else:
                self._send_json({"error": "not found"}, 404)
        except Exception as ex:
            self._send_json({"error": str(ex), "trace": traceback.format_exc()}, 500)
    def _handle_sse(self):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "keep-alive")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        q = broker.subscribe()
        try:
            self._sse_write({"type": "init", "session": session.to_dict()})
            while True:
                try:
                    event = q.get(timeout=15)
                    self._sse_write(event)
                except queue.Empty:
                    self.wfile.write(b": heartbeat\n\n")
                    self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            broker.unsubscribe(q)
    def _sse_write(self, data):
        text = json.dumps(data, ensure_ascii=False)
        self.wfile.write(f"data: {text}\n\n".encode("utf-8"))
        self.wfile.flush()
    def log_message(self, fmt, *args):
        pass

class ThreadedHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

def main():
    import sys
    port = PORT
    if len(sys.argv) > 1:
        try:
            port = int(sys.argv[1])
        except ValueError:
            pass
    server = ThreadedHTTPServer(("127.0.0.1", port), Handler)
    url = f"http://127.0.0.1:{port}"
    print(f"[cli_test_tool] Server running at {url}")
    print(f"[cli_test_tool] Press Ctrl+C to stop.")
    threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[cli_test_tool] Shutting down.")
        server.shutdown()

if __name__ == "__main__":
    main()
