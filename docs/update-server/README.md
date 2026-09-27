# EdgeSlicer new-version check

EdgeSlicer tells people when a newer release is out: at startup (once, silently on
any error) and from **Help > Check for Update** (which reports errors). The offer is
the "New version of EdgeSlicer" dialog: short release notes, a link to the full notes,
**Download**, **Skip this Version** and **Cancel**.

By default the app reads the **GitHub release feed** of `aceRage/EdgeSlicer`. Nothing
has to be hosted: publishing a GitHub release is what makes the dialog appear. A
self-hosted server can still replace GitHub with one config key (see the end of this page).

Code: `src/slic3r/Utils/AppUpdateCheck.{hpp,cpp}` (parsing and decisions, no GUI;
tests in `tests/slic3rutils/app_update_check_tests.cpp`) and
`GUI_App::check_new_version_sf()` (request and dialog).

## How the check works

1. `GET https://api.github.com/repos/aceRage/EdgeSlicer/releases/latest` with the
   User-Agent `EdgeSlicer/<release version>` (GitHub requires one), `Accept: application/vnd.github+json`,
   and no other headers (the app's X-BBL-* headers, which carry a device id, are
   dropped for this request). Unauthenticated: GitHub allows 60 requests an hour per
   IP address, far above one per start.
2. `/releases/latest` never returns drafts or releases marked **pre-release**; the
   client ignores them as well, and also ignores a tag with a prerelease part
   (`v2.4.1.0-beta1-edge`) even if the pre-release box was not ticked.
3. The version comes from `tag_name`: a leading `v` and the `-edge` suffix are
   dropped, and 2-4 numeric parts are compared part by part, so `v2.4.0.0-edge`,
   `2.4.0.0` and `2.4` are the same version and `2.3.9` is newer than `2.3.8.5`.
   (The app's `Semver` class folds a fourth part into `patch*100`, which gets that
   last comparison wrong, so it is not used here.)
4. The release is offered only when it is newer than the build's own
   `Snapmaker_VERSION` (`src/common_func/common_func.hpp`).
5. **Download** opens the browser on:
   - Windows: the asset whose name ends in `.exe` and contains `Installer`
     (`EdgeSlicer_Windows_Installer_V<ver>.exe`), else any `.exe`;
   - macOS: the `.dmg` containing `universal`, else the one for the build's
     architecture, else any `.dmg`;
   - Linux (AppImage / tarball) and anything not found above: the release page
     (`html_url`), where people pick their file.
   Only links under `https://github.com/aceRage/EdgeSlicer/` are used; anything else
   falls back to the releases page. (The HTTP client runs with TLS peer
   verification off, so this is what keeps a tampered answer from redirecting the
   Download button.)
6. **Flatpak** builds (`$FLATPAK_ID` set, or `/.flatpak-info` present) never check:
   Flathub delivers their updates. Help > Check for Update says so.

**Skip this Version** stores the offered version (`skip_version` in the app config);
that version and anything older are not offered again at startup. A newer release
clears the key. Help > Check for Update always shows the dialog (without the Skip
button).

**Preference:** Preferences > General > "Check for new versions on startup"
(`check_for_updates_on_startup`, default on) turns the startup check off; the Help
menu item keeps working.

**Update channel:** Preferences > General > "Update channel" (`update_channel`, default
`stable`). Everything above is the Stable channel. The Nightly channel reads
`/releases/tags/nightly` instead and compares build dates and commits; a nightly build on
the Stable channel is offered a release only when it is newer than the code the nightly was
built from. See `docs/nightly.md`.

## Release notes in the dialog: the update-notice block

GitHub release bodies are long. The dialog shows only a compact section of the body,
delimited by two HTML comments, which GitHub does not display:

```markdown
<!-- update-notice -->
**EdgeSlicer 2.4.0.0** - faster startup and CAD-style edge tools.
- Bevel and chamfer on curved rims
- Draw cut on cylinders and flat faces
- FlashForge Creator 5 prints over the local network
<!-- /update-notice -->
```

Rules:

- Put the block near the top of the body; it doubles as the summary on the release
  page. Aim for 3-8 lines and under ~600 characters.
- Markdown is flattened to plain text: `#` headings lose the hashes, `**bold**`,
  `*italic*` and `` `code` `` lose their markers, `[text](url)` keeps the text,
  images and HTML comments/tags disappear, `- ` / `* ` bullets become dots,
  numbered items keep their number. Blank lines separate paragraphs.
- The block is shown whole up to a safety cap of 40 lines / 4000 characters.
- Spacing and case inside the comments do not matter (`<!--Update-Notice-->` works).
  An empty block, or a begin marker without an end marker, counts as no block.
- Under the text the dialog always links **See the full release notes** to the
  GitHub release page.

**Fallback** when the body has no block: the first 12 non-empty lines of the body
(after the same flattening), at most 800 characters; a line that would cross the
limit is cut at a word and ends in an ellipsis. For the 2.3.8.5 body that is the
opening quote, the title and the first paragraphs - readable, but the block is better.

**Every release gets one:** the template `scripts/release/RELEASE_BODY.template.md`
has the block, and the release orchestrator (`release_<ver>.sh` in the hubtest
folder) refuses a `RELEASE_BODY.md` without it. To check a published release:
`gh release view v<ver>-edge -R aceRage/EdgeSlicer --json body --jq .body | grep -n update-notice`.
A release body can be edited after publishing (`gh release edit ... --notes-file`);
the next check picks it up.

## Seeing the dialog without a newer release

Set the debug-only environment variable `EDGESLICER_FAKE_LOCAL_VERSION` to an older
version before starting the app; the check then compares against that instead of
the build's version, and the log says so (`DEBUG EDGESLICER_FAKE_LOCAL_VERSION`):

```bat
set EDGESLICER_FAKE_LOCAL_VERSION=2.3.0.0
EdgeSlicer.exe --datadir C:\some\scratch\datadir
```

Then use Help > Check for Update (or restart: the startup check fires once the main
window is up). If you clicked Skip this Version earlier, delete `skip_version` from
the config or use the Help menu. The live GitHub parse can also be exercised without
the GUI: `slic3rutils_tests "[AppUpdateLive]"` prints what the dialog would show.

## Self-hosted server (override)

Set `orca_upgrade_url` in the app config to replace GitHub with your own endpoint:

- Windows: `%APPDATA%\EdgeSlicer\EdgeSlicer.conf`
- macOS: `~/Library/Application Support/EdgeSlicer/EdgeSlicer.conf`

```json
"app": {
    "orca_upgrade_url": "https://downloads.example.com/edgeslicer/version.json",
    ...
}
```

(The config file is JSON; edit it with the app closed.)

With the key set the app uses the old Snapmaker JSON schema below instead of the
GitHub feed; removing (or emptying) it goes back to GitHub. The startup preference,
Skip this Version and `EDGESLICER_FAKE_LOCAL_VERSION` apply to both.

A **static file is enough** - any endpoint that returns HTTP 200 with this JSON works
(no signing, no auth). See `version.example.json` next to this file.

| Field | Meaning / requirement |
|---|---|
| `code` | Must be `200`, top level, or the response is ignored. |
| `data.version` | 2-4 numeric parts, optional `-tag`. Must be **greater than the app's build version**. |
| `data.release_type` | Must be exactly `"stable"` or the release is ignored. |
| `data.platform_type` | `"win"` or `"mac"`. **Linux is refused** by this schema. |
| `data.is_force_upgrade` | `false` normally. `true` shows the mandatory-upgrade dialog that closes the app and only offers to open the download URL - use only for "must not run this build" situations. The GitHub feed never forces. |
| `data.is_full_upgrade` | Kept for schema compatibility, not acted on. |
| `data.full.file_describe` | Release-notes text, shown verbatim in the dialog. Plain text with newlines. |
| `data.full.default.file_url` | Download link (Windows / fallback). |
| `data.full.arm.file_url` / `data.full.intel.file_url` | macOS Apple Silicon / Intel links, picked by build, falling back to `default`. |
| `file_size`, `file_md5`, `file_sha256`, `reserved_1`, `reserved_2` | Parsed but unused. |

One `version.json` per platform URL is the simplest layout
(`.../win/version.json`, `.../mac/version.json`).
