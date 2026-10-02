# Shipping FlashForge LAN support (Creator 5 and friends)

Branch: `feat/flashforge-lan-ship`, from `feat/ultra-preferences`.

## The problem

EdgeSlicer carries a FlashForge device tab, ported from FlashForge's own OrcaSlicer fork
(`Orca-Flashforge`, AGPL-3.0). Every printer operation in that tab — discovery, identification,
status, and sending a job — goes through `FlashNetwork.dll`, a closed, pre-built library that is
FlashForge's own work. The repository does not contain it and, until now, no package shipped it.

The consequence on a user's machine: `GUI_App` looked for `FlashNetwork.dll` beside the executable,
did not find it, logged one line, and carried on. The Device tab then initialised nothing and
rendered an empty page. There was no message, no path, and no way for a user to discover what was
missing — and no LAN send to a Creator 5 was possible on any install but the author's, which had
the DLL copied in by hand.

## Provenance

The copy in the owner's live install was identified by hashing it against the DLLs inside
FlashForge's published Windows releases.

| | |
|---|---|
| Source | FlashForge **Flash Studio Desktop 1.7.13**, published 2026-09-01 |
| Release asset | `Flash_Studio_Desktop_1.7.13_x64.exe` (193,406,528 bytes) |
| File | `FlashNetwork.dll`, 3,917,824 bytes |
| sha256 | `e19e49e7b5509065e2165267c22acd402245f9beb1c5100327d9a5229ba39152` |
| PE timestamp | Jul 31 2026 08:26:08 |
| Machine | x86-64 (PE32+), MSVC linker 14.44 |

Nearby releases were checked and ruled out — note that **size alone does not identify the build**:
1.7.9-beta, 1.7.11 and 1.7.13 all ship a 3,917,824-byte DLL with three different hashes.

| Release | size | sha256 (prefix) |
|---|---|---|
| 1.7.15 | 3,996,160 | `57683553…` |
| **1.7.13** | **3,917,824** | **`e19e49e7…` ← ours** |
| 1.7.11 | 3,917,824 | `49d45378…` |
| 1.7.9-beta | 3,917,824 | `a4b60cc9…` |
| 1.7.8-beta | 3,895,296 | `c2e71069…` |

### Dependencies

`dumpbin /dependents` lists only Windows system libraries — `IPHLPAPI`, `WS2_32`, `WLDAP32`,
`CRYPT32`, `KERNEL32`, `USER32`, `ADVAPI32`, `bcrypt`, `RPCRT4`, and the `api-ms-win-crt-*` stubs —
plus the MSVC 2015+ redistributable runtime (`MSVCP140`, `VCRUNTIME140`, `VCRUNTIME140_1`), which
EdgeSlicer is built against and already ships. The library statically links its own HTTP/TLS stack
(consistent with its size and its `fnet_downloadFile*` exports). **No companion DLLs need to travel
with it.**

## Ship, not download

The brief allowed either: auto-download from a FlashForge CDN if one exists, or ship the file.

**There is no CDN.** FlashForge's own slicer loads the library straight from its executable's
directory with no fetch and no fallback; if it is absent, `MultiComMgr::initalize` logs a failure
and the app degrades. Their source does contain `download_plugin` / `get_plugin_url` and
`update.flashforge.com/api/updates/*`, but those are inherited Bambu Studio plumbing (used only for
`networking_plugins.zip`) and whole-application update checks that return an installer URL. Neither
serves `FlashNetwork.dll`. The library is obtainable only by extracting a release installer, and
upstream itself has no CMake install rule for it — it is injected at packaging time into their
Advanced Installer package.

So EdgeSlicer ships it.

### The CMake option

`CMakeLists.txt` gained a `FLASHNETWORK_BIN_DIR` block beside the existing `ULTRANET_BIN_DIR` one:

```
-DFLASHNETWORK_BIN_DIR=C:/Users/acesa/AppData/Local/Temp/snorca_hubtest/flashnetwork_out/ship
```

It installs `FlashNetwork.dll` (and any `*.dll` beside it, should a future build need companions)
into the install root, next to the executable. Without the flag the configure prints

```
EdgeSlicer: no FLASHNETWORK_BIN_DIR - FlashForge LAN printing will be unavailable in this package
```

and the package simply carries no FlashForge support — the same as before, but now said out loud.

The DLL is kept **out of the repository**, staged at
`C:\Users\acesa\AppData\Local\Temp\snorca_hubtest\flashnetwork_out\ship\FlashNetwork.dll`.

### Licensing note

The library is FlashForge's and is redistributed **unmodified**. The FlashForge connection dialog
carries a visible notice saying so. This is the same posture as the FlashForge product artwork
decision (2026-09-08): acceptable while EdgeSlicer is non-commercial, and a question to revisit if
that changes.

### The server-settings file: FLASHNETWORK9.DAT (added after 2.4.2.0)

The DLL is not enough on its own. `fnet_initlize(serverSettingsPath, ...)` reads FlashForge's
server-settings file and returns `FNET_ERROR` (-1) without it. Tested against the shipped
`FlashNetwork.dll` (it reports version **3.4.2**) with a small harness:

| `serverSettingsPath` | return |
|---|---|
| missing file | -1 |
| empty file | -1 |
| `FLASHNETWORK7.DAT` (the older generation, Orca-Flashforge `main`) | -1 |
| `FLASHNETWORK9.DAT` | **0** |
| `nullptr` | 0 (not used: FlashForge's client always passes the file) |

2.4.0 to 2.4.2.0 shipped the DLL without the file, so every install logged
`initalize FlashNetwork failed` and the FlashForge Device tab (and its camera) never started.

| | |
|---|---|
| Source | FlashForge's public **Orca-Flashforge** repository (AGPL-3.0), `resources/data/FLASHNETWORK9.DAT` on `develop`, `release_0730` and `release_0817` |
| Git blob | `8175390575a6f105f96f196b1340bd58c44dfc68` |
| Size | 7,482 bytes (header `FNETDAT-1.2.0`, the rest encrypted) |
| sha256 | `889d2f9a3bb1801b37a1d2a13e8914aa9d9fe1b1fb5de34bb2a684c0fb3d88ce` |

FlashForge commit it to an AGPL-3.0 repository with no carve-out for `resources/data`. It is kept
out of this tree because it is generation-matched to the DLL (a 7 file fails the 3.4.2 library), so
it travels with the DLL in the same ship folder: put it beside `FlashNetwork.dll` in
`FLASHNETWORK_BIN_DIR` and CMake installs it to `resources/data`, where FlashForge's own client
reads it. The self-hosted workflow's "Verify the shipped payloads landed" step fails the run if it
is missing.

At run time `ff_flashnetwork_dat_search_paths()` looks in `<resources>/data`, then beside the DLL,
then in `<dll dir>/resources/data` (an Orca-Flashforge install picked with Locate), then
`<data dir>/plugins`. A missing file is logged by name, and every failure step (LoadLibrary with
its system error, a missing export, the version, or `fnet_initlize` with its return code and
settings path) is logged at error level and shown on the Device tab.

## Where the library is looked for

`ff_flashnetwork_search_paths()` returns, in order:

1. `<exe dir>/FlashNetwork.dll` — where the installer puts it.
2. `<data dir>/plugins/FlashNetwork.dll` — the only place a user without administrator rights can
   fill in by hand.

The installed copy deliberately wins, so a stale hand-placed file cannot shadow the one that
matches the build. `GUI_App::init_flashnetwork()` walks the list, records which paths were tried,
and records *why* it failed if it did.

## Visible failure

The Device tab no longer renders empty. `GUI_App` exposes `flashnetwork_loaded()`,
`flashnetwork_error()`, `flashnetwork_path()` and `flashnetwork_searched()`, and two message
builders produce the text:

- **Missing** — `ff_flashnetwork_missing_text()` lists every path that was tried and offers
  *Download* and *Locate*.
- **Found but would not load** — `ff_flashnetwork_load_failed_text()` names the path and points at
  the overwhelmingly common cause, a 32-bit library beside a 64-bit build (or a truncated copy).

`init_flashnetwork(explicit_path)` takes the path a user picks with *Locate*, so the stack can come
up without a restart.

## The C5 LAN flow, as ported

LAN mode only: a device is addressed by **IP + port (8898) + serial number + check code**. The check
code is shown on the printer under *Settings > Network > Network Mode*. There is no cloud login in
this build.

| Step | Call |
|---|---|
| Discovery | `fnet_getLanDevList` — a UDP sweep; it returns non-FlashForge devices too, which the tab filters |
| Identify | `fnet_getLanDevProduct` — which controls the machine advertises (temperature, light, fans) |
| Describe / poll | `fnet_getLanDevDetail` — name, `firmwareVersion`, `nozzleModel`, `macAddr`, `status`, temperatures, progress, material station contents |
| Send a job | `fnet_lanDevSendGcode` — uploads the sliced file (`gcodeFilePath`, `thumbFilePath`, `gcodeDstName`) with `printNow`, `levelingBeforePrint`, `flowCalibration`, `firstLayerInspection`, `timeLapseVideo`, `useMatlStation` and the material mappings |
| Start an already-uploaded file | `fnet_lanDevStartJob` — same job struct, referencing a file already on the printer |

**Material mappings.** `com_material_mapping_t` values are converted by
`MultiComUtils::comMaterialMappings2Fnet()` into `fnet_material_mapping_t` and passed as
`materialMappings` with `gcodeToolCnt`. Each entry ties a G-code tool id to a material-station slot
id plus the material's name and colour, which is how a multi-material C5 job is bound to the slots
actually loaded. A single-extruder send passes `useMatlStation = 0` and no mappings.

Every call in the connection test (`getLanDevProduct`, `getLanDevDetail`) is read-only. **No job is
started by the test or by diagnostics.**

## Connection test

The dialog (`FFDiagnosticsDialog`) takes serial number, IP and check code — prefilled from the
selected device where there is one — and runs the two read-only calls. It reports:

- reachable yes/no,
- product name, machine type, firmware version, MAC address, printer status,
- and on failure, the FNET error code with its constant name and a sentence explaining it.

The error map is the point. FlashNetwork reports every failure as a bare integer, and the one a C5
owner meets most is `1001`:

| Code | Name | Text |
|---|---|---|
| 0 | `FNET_OK` | ok |
| -1 | `FNET_ERROR` | no reply from the printer — check the IP, that it is powered on and on the same network, and that LAN mode is enabled |
| 2 | `FNET_DIVICE_IS_BUSY` | the printer is busy — printing or calibrating, and refuses new work |
| 3 | `FNET_GCODE_NOT_FOUND` | the printer does not have that G-code file |
| **1001** | **`FNET_VERIFY_LAN_DEV_FAILED`** | **serial number or check code rejected — the printer answered but did not accept these credentials** |
| 2001 | `FNET_UNAUTHORIZED` | the FlashForge account session has expired |
| 3001 | `FNET_CONN_SEND_ERROR` | the cloud connection dropped while sending |

An unrecognised code still yields usable text and quotes the number, because a newer DLL can return
codes a given build predates.

Note that `FNET_OK` is the *only* success: `FNET_ERROR` is `-1` but the busy / aborted /
gcode-not-found failures are positive, so a `ret < 0` test would wave them through. `fnet_succeeded()`
exists for that reason and is tested.

## Diagnostics

The *Diagnostics* button:

1. Raises FlashNetwork's log level to `FNET_LOG_LEVEL_DEBUG` for the session. The library fixes its
   log level in `fnet_initlize` and exports no runtime setter, so `MultiComMgr::setDebugLogging()`
   tears the interface down and rebuilds it. This is why the existing `FLASHNETWORK_DEBUG` marker
   file previously required a restart; that file is still honoured, but is no longer needed.
2. Runs the connection test — so the first thing in the fresh debug log is the test itself.
3. Writes **one** zip to the Desktop (falling back to Downloads, then Documents), named
   `EdgeSlicer_FlashForge_diagnostics_<YYYY-MM-DD>.zip`, with a numeric suffix rather than
   overwriting an earlier archive from the same day.
4. Shows the path with an *Open folder* button.

### Contents

```
summary.txt           app version, build, OS, DLL path, whether it loaded,
                      FlashNetwork version, the connection test report, and
                      a line stating no check code is included
connection_test.txt   the same test report on its own
flashnetwork/*        the newest FlashNetwork logs (up to 6)
log/debug_*           EdgeSlicer's newest logs (up to 2)
```

The printer entry in the report carries the **serial number and IP address, and never the check
code** — the line reads `check code : not recorded`. The mechanism is structural:
`FFDiagnosticsInput` has no field for a check code, so the manifest builder cannot emit one. The
dialog additionally asserts it with `ff_diagnostics_manifest_is_free_of()` against the code the user
actually typed, and refuses to write the archive if that ever fails.

Log counts are capped so the archive stays mailable. A log that is locked or has vanished is skipped
rather than failing the whole export.

The existing **Export Logs / Upload log (email)** flow in `ExportLogs.cpp` is untouched.

## How to report a Creator 5 problem

> Install EdgeSlicer and open the **Device** tab. If the tab tells you FlashForge's FlashNetwork
> library is missing, use **Download** or **Locate** as it suggests and reopen the tab. Select your
> printer (or enter its IP address and serial number by hand) and press **Test connection** — the
> printer's check code is on the printer itself, under *Settings > Network > Network Mode*. If the
> test succeeds it will show your printer's name and firmware version, and printing over the LAN
> should work. If it fails, press **Diagnostics**: that turns on detailed logging, retries the test,
> and saves a single file named `EdgeSlicer_FlashForge_diagnostics_<date>.zip` to your Desktop, then
> offers an **Open folder** button to find it. Send that zip with your report. It contains the logs,
> your app version, and your printer's serial number, IP address and firmware version — it does
> **not** contain your check code, so it is safe to share.

## Files

| File | What |
|---|---|
| `src/slic3r/GUI/FlashForge/FFDiagnostics.hpp/.cpp` | FNET error map, connection-test result, diagnostics manifest, library search paths. No wx, no filesystem, no network — all pure functions |
| `src/slic3r/GUI/FlashForge/FFDiagnosticsDialog.hpp/.cpp` | The Test connection / Diagnostics / Open folder dialog, and the zip writer |
| `src/slic3r/GUI/FlashForge/MultiComMgr.hpp/.cpp` | `setDebugLogging()`, `libraryVersion()`, `logFileDir()`; init parameters remembered so the level can be changed |
| `src/slic3r/GUI/GUI_App.hpp/.cpp` | `init_flashnetwork()` and the recorded failure state |
| `CMakeLists.txt` | the `FLASHNETWORK_BIN_DIR` install block |
| `tests/slic3rutils/ff_diagnostics_tests.cpp` | the error map and manifest tests |

## Tests

`tests/slic3rutils/ff_diagnostics_tests.cpp`, tagged `[FFDiagnostics]`. They cover the FNET names
and texts (including that 1001 reads as a credential problem and -1 as a network one, that an
unknown code still produces usable text, and that only `FNET_OK` is success), the test-result
summarising, the manifest layout (log files keep base names under their own folders, colliding
names both survive, the home directory never becomes part of the archive layout), **that the check
code never reaches the manifest** — with a poisoned-manifest control proving the assertion can
actually fail — and the library search order.

## Build

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="C:\Dev\SnapmakerOrca\deps\build/OrcaSlicer_dep/usr/local" \
  -DBUILD_TESTS=ON \
  -DULTRANET_BIN_DIR=.../ultranet_out/ship/ultranet \
  -DFLASHNETWORK_BIN_DIR=.../flashnetwork_out/ship
```

Note `BUILD_TESTS` is **off** by default; the test targets are not generated without it.
