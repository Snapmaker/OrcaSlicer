# Nightly builds

EdgeSlicer publishes an **untested** build of `main` every night that `main` has changed. It lives
in one GitHub pre-release, tag [`nightly`](https://github.com/aceRage/EdgeSlicer/releases/tag/nightly),
whose files are replaced by each new nightly. Stable releases are unaffected: the nightly is a
pre-release, is never marked Latest, and the app's normal update check never offers it.

## What a nightly is

- **When:** `.github/workflows/nightly.yml` runs daily at 18:00 UTC (GitHub may start it later).
  It first compares `main` with the commit recorded in the current nightly's release body
  (`<!-- nightly-commit: ... -->`). If they are the same it stops: no build, no new files.
- **What:** the same packages as a release, built by `build_all.yml` on GitHub's hosted runners:
  Windows installer and portable zip, the macOS universal dmg (signed and notarized when the
  repository has the Developer ID secrets, as releases are), the Linux AppImage and the two
  Flatpaks, plus `SHA256SUMS`.
- **Version:** `<Snapmaker_VERSION>-nightly.<YYYYMMDD>+<shortsha>`, for example
  `2.4.1.0-nightly.20260926+16fd5aa` (UTC date, 7-character commit). About, the splash screen,
  the log, the update check and the crash reporter all use it; crash reports go to the
  Sentry release `edgeslicer@<that version>` in the environment `nightly`. File names carry it
  with `-` instead of `+` (GitHub renames assets with special characters):
  `EdgeSlicer_Windows_Installer_V2.4.1.0-nightly.20260926-16fd5aa.exe`.
  Project files, presets and printer protocols keep the plain release number.
- **The release body** says it is untested, names the commit and date, lists the pull requests
  merged since the latest stable release, says what the build lacks, and explains how to go back.
  It is written by `scripts/release/nightly_body.sh`; its `<!-- nightly-version: ... -->` line is
  what the app reads.

### What a nightly does not have

Some parts of a release come from the owner's Windows build machine, not from the source:

| Part | Nightly | Why |
|---|---|---|
| UltraNet (Bambu network plug-in), all platforms | yes, when the `ULTRANET_DEPLOY_KEY` secret is set | built in CI from the private `aceRage/ultranet` with a read-only deploy key; only the binaries are packaged (`scripts/build_ultranet_posix.sh`, `scripts/build_ultranet_windows.ps1`) |
| FlashForge `FlashNetwork.dll` (Windows) | **no** | a closed binary extracted from FlashForge's Flash Studio installer; there is no source or download for CI to use |
| Bundled `ffmpeg.exe` (Windows) | **no** | the pinned LGPL build's dated BtbN release has since been deleted upstream, and it is not kept anywhere CI can fetch it |

The release body lists whatever is missing from that night's build.

## Channels in the app

Preferences > General > **Update channel**:

- **Stable** (default): the app reads the latest release, exactly as before. If the running build
  is itself a nightly, a stable release is offered when its version is higher, or when it has the
  same release number and was published after the day the nightly was built.
- **Nightly**: the app reads the `nightly` pre-release instead and offers it when it is newer than
  the running build: a higher release number or a later date, or the same day with a different
  commit (a second build that day). A stable build on this channel is offered the nightly of its
  own release line or a later one. The dialog shows the release body's update-notice block, or
  "New nightly build (<date>, <commit>)." when there is none.

The setting is `update_channel` (`stable` / `nightly`) in `EdgeSlicer.conf`. The logic and its
tests are in `src/slic3r/Utils/AppUpdateCheck.{hpp,cpp}` and
`tests/slic3rutils/app_update_check_tests.cpp` (`[Nightly]`).

## Going back to stable

1. Preferences > General > Update channel: **Stable**.
2. Install the [latest release](https://github.com/aceRage/EdgeSlicer/releases/latest) over the
   nightly: the Windows installer replaces it in place; on macOS replace the app; on Linux use the
   release AppImage.

Nightlies and releases share one settings folder. A nightly can save settings an older release
does not understand, so copy the folder (Help > Show Configuration Folder) before trying one.

## Maintainers

- **Run it by hand:** Actions > Nightly > Run workflow. On `main`, tick **publish** to publish;
  tick **force** to build even though `main` has not moved. On any other branch, or without
  publish, the run builds everything and uploads the release body and `SHA256SUMS` it would have
  published as the `nightly-preview` artifact; the packages are the run's normal artifacts.
- **Publishing** is only ever done by the scheduled run on `main` or a manual run of `main` with
  publish ticked. It creates the `nightly` release the first time, afterwards moves the `nightly`
  tag to the new commit, uploads the new files with `--clobber`, deletes the files the new set
  did not replace, and updates the body last. It never touches another release or tag, and fails
  if the nightly ever came back as Latest.
- **Try the naming without publishing:** Actions > Build all > Run workflow with a
  `version-suffix` such as `-nightly.20260926+abc1234`.
- A nightly is built from `main` only: `ci/*` and `release/*` builds keep their release versions.
