# Privacy

EdgeSlicer collects no usage statistics and no analytics. This page covers the two things that
can leave your computer: crash reports (off unless you turn them on) and phone app notifications.

# Crash reports

EdgeSlicer collects no usage statistics and no analytics. The one thing it can send by itself is
a **crash report**, and only after you turn that on.

## Turning it on and off

**Preferences > General > Privacy > Send crash reports.** It is **off** by default and a change
applies at once, without a restart.

EdgeSlicer asks about it once. On the first start after a crash, a notification offers to turn
crash reports on. Closing the notification means no, and you are not asked again. Nothing about
the crash that was just caught is sent. A report is only ever sent for a crash that happens while
the setting is on.

Some builds have no crash-report destination: builds you compile yourself, forks, and the Linux
AppImage and Flatpak. They never send anything. On Windows and macOS the switch is greyed out in
those builds.

## What a crash report contains

- **The crash itself:** a minidump. It holds the CPU registers and stack memory of each program
  thread at the moment of the crash, plus the list of loaded program modules (file paths and
  versions). Like any Windows crash dump, it also holds the program's command line and
  environment variables. Those include your Windows user and computer name. EdgeSlicer blanks
  secret command-line values such as the phone hub's `--hub-token` before anything can crash.
  Sentry turns the minidump into a stack trace using the debug symbols we upload for each
  release. Stack memory can hold fragments of whatever the program was working on at that
  moment. The minidump is used only to compute the trace; the project does not keep raw
  minidumps.
- **App version and build:** for example release `edgeslicer@2.4.0.0`, environment `release`.
- **Operating system:** name, version and CPU architecture.
- **Recent log lines:** the most recent warning and error lines of the app log from the session
  that crashed, at most 100. There are at most 20 per second, each is cut to 512 characters,
  and each is scrubbed as described below *before* it is stored.

**Not sent:** log files, configuration files, projects or models, presets, printer settings,
account details, a user or machine ID, the computer name, usage events. EdgeSlicer's old "bury
point" usage events and Sentry sessions and traces are all switched off in the code.

Reports go over HTTPS to [Sentry](https://sentry.io), the service EdgeSlicer's developers use to
read them. Like any web server, Sentry sees the IP address a report comes from. The project is set
not to store it.

## What is removed from log lines

Before a log line becomes part of a crash report, EdgeSlicer replaces:

| Removed | Replaced by |
|---|---|
| Printer and other LAN IP addresses (IPv4 and IPv6) and MAC addresses | `<ip>`, `<mac>` |
| Host names in URLs (except public services such as github.com or bambulab.com) and LAN names (`*.local`, `*.lan`, `*.ts.net`, ...) | `<host>` |
| `user:password@` in URLs, every query-string value, URL fragments | `<redacted>` |
| Access codes, check codes, passwords, tokens, API keys (`X-Api-Key`), cookies, `Bearer`/`Basic` credentials, `--hub-token` | `<redacted>` |
| Printer serial numbers and device IDs (`dev_id`, `sn`, `serialNumber`, Bambu MQTT topics), UUIDs | `<serial>`, `<redacted>`, `<uuid>` |
| E-mail addresses, user IDs and names, Wi-Fi SSIDs | `<email>`, `<redacted>` |
| Your user name in file paths: `C:\Users\<name>\`, `/Users/<name>/`, `/home/<name>/` | `<user>` |
| Other long random-looking tokens and JWTs | `<token>` |

The scrubber is `src/sentry_wrapper/SentryScrub.cpp`. It is unit-tested against real log lines
from Bambu MQTT, OctoPrint, Moonraker, Snapmaker U1, FlashForge and the phone hub
(`tests/slic3rutils/sentry_scrub_tests.cpp`).

## Local crash files

With reports on or off, the crash handler keeps its minidumps on your machine for local debugging.
They are in `%LOCALAPPDATA%\EdgeSlicer\reports` on Windows and in
`~/Library/Application Support/EdgeSlicer/SentryData` on macOS. An instance started with
`--datadir <dir>` keeps them in `<dir>/SentryData` instead. You can delete them at any time.

# Phone app notifications

The EdgeSlicer phone app can show notifications when a printer starts, finishes, fails or reports
an error. Your EdgeSlicer on the PC (the phone hub) creates them.

## How a notification reaches your phone

Apple and Google only deliver notifications to an app when the request is signed with the app
developer's keys. Starting with 2.4.1.0, a hub that doesn't have its own keys hands each
notification to the **EdgeSlicer push service** at `push.edgeslicer.com`, which forwards it to
Apple (APNs) or Google (Firebase Cloud Messaging). You don't need to change anything to use it.

**The content is end-to-end encrypted.** Your hub encrypts the title and text with a key that only
your phone holds (Web Push encryption, RFC 8291). The push service, Apple and Google only see a
placeholder ("Printer update", "Tap to open") and an encrypted blob. The phone decrypts it on the
device.

## What the push service sees and keeps

| What | Kept |
|---|---|
| Your hub's random ID and its public signing key (created by the hub, not tied to you or your account) | until the hub is unused for 365 days |
| When the hub registered and when it was last seen, and how many notifications it sent per day | daily counts for 90 days |
| Your phone's push token (issued by Apple or Google for this app), with each notification | not stored; passed on to Apple or Google (a keyed hash is held in memory for the day, to count devices per hub) |
| A service log line per request: time, hub ID, result, platform, size | 7 days |
| Your IP address | not logged; used only in memory, as a hashed network prefix, to limit sign-up abuse |

Nightly backups of the hub list are kept for 30 days. The service never sees printer names, file
names, your e-mail or anything else about your prints; those are inside the encrypted part.

The service runs on a server rented from Hetzner in Helsinki, Finland. There is a limit of 300
notifications per hub per day; past it, the hub page shows a notice and the rest of that day's
notifications are not sent.

## Not using it

- To use your own Apple/Firebase keys instead, add them in the hub page's push settings. A hub
  with its own keys uses them and never contacts the push service.
- To have no phone notifications at all, don't pair the phone app, or turn notifications off for
  the app in your phone's settings.

---

## For maintainers

**Destination.** CI compiles the DSN in from the `SENTRY_DSN` repository secret. It passes the
`EDGESLICER_SENTRY_DSN` environment variable, or `-DEDGESLICER_SENTRY_DSN=...`, to CMake. CMake
writes it into `<build>/generated/edgeslicer_sentry_config.h` and never prints it. An empty DSN
means no uploads, and that is the case for every local build. Never commit a DSN.

**Environment.** `EDGESLICER_SENTRY_ENVIRONMENT` is set as follows:
- `release` for tags, `main`, `ci/*` and `release/*`.
- `ci` for other CI builds.
- `dev` locally.

**Release.** The app reports `edgeslicer@<Snapmaker_VERSION>`. CI uses the same name when it uploads
symbols.

**Symbols.** Two workflows upload symbols after a successful build:
- `build_all.yml` (through `build_deps.yml` and `sentry_cli.yml`), for the Windows PDBs and PE
  files and the macOS dSYMs.
- `build_windows_selfhosted.yml`, for the Windows build on the self-hosted VM.

Both use sentry-cli 3.8.0, pinned and sha256-verified. They read `SENTRY_AUTH_TOKEN`,
`SENTRY_ORG` and `SENTRY_PROJECT`. Without those secrets the steps skip. When Sentry is down the
upload step fails, but the build does not. The Sentry release is created and finalized only for
the release refs listed above.

**Overrides for testing:**
- `"ultra_sentry_dsn"` in `EdgeSlicer.conf` replaces the compiled-in DSN, for example with a
  personal project or a local listener such as `http://publickey@127.0.0.1:8765/1`. Consent is
  still required.
- `EDGESLICER_SENTRY_DEBUG=<file>` writes sentry-native's own diagnostics to that file.

### Testing crash reports

1. Use a build that has a DSN: a CI build of this repository, or any build with `ultra_sentry_dsn`
   set.
2. Turn on Preferences > General > Privacy > Send crash reports.
3. Quit EdgeSlicer, then start it with `EDGESLICER_TEST_CRASH=1` set. In PowerShell:
   `$env:EDGESLICER_TEST_CRASH=1; & "C:\Program Files\EdgeSlicer\EdgeSlicer.exe"`.
   About 3 seconds after the window opens, it logs a few lines full of fake secrets and then
   crashes on purpose with an access violation in `GUI_App::run_test_crash_if_asked`.
4. Start EdgeSlicer normally. Any report crashpad has not uploaded yet goes out now.
5. In Sentry, go to Issues and filter by the release `edgeslicer@<version>`. The event should show:
   - a symbolicated stack ending in `run_test_crash_if_asked`;
   - breadcrumbs in which every fake secret appears as `<ip>`, `<host>`, `<redacted>`, `<serial>`,
     `<email>` or `<user>`.
6. Repeat with the setting off. Nothing arrives.

`EDGESLICER_TEST_CRASH=startup` crashes at the very start instead, before any window opens and
without the log breadcrumbs. It is useful from a script, for example:
`EdgeSlicer.exe --datadir <dir> --hub-token 1234abcd` with the variable set. Only testers set
either value. Without the variable, neither path does anything.
