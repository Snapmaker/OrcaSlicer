# EdgeSlicer

**Bleeding edge featureset, pulled from all slicing worlds. Per-part support control, CAD-style mesh editing and cut tools, a phone app with push notifications and reprints, remote access, camera streams, filament manager, expanded printer profiles, expanded assembly and multicolor toolsets.**

[Releases](https://github.com/aceRage/EdgeSlicer/releases) · Windows installer & portable · Linux AppImage · macOS (unsigned) · Based on [Snapmaker Orca](https://github.com/Snapmaker/OrcaSlicer) 2.3.6 · AGPL-3.0

![Stream camera wall, Support Filament Matching, Compare Slices and the Assemble tool's Auto-fit](docs/images/hero.jpg)

## Download

### Latest Release

📥 **[Download the Latest Stable Release](https://github.com/aceRage/EdgeSlicer/releases/latest)**

- **Windows installer** — the `EdgeSlicer_Windows_Installer_*.exe` on the [releases page](https://github.com/aceRage/EdgeSlicer/releases/latest). Installs side by side with a stock Snapmaker Orca, with its own folder, icon and data directory, and upgrades a previous EdgeSlicer in place.
- **Windows portable** — the `EdgeSlicer_Windows_V*_portable.zip` on the [releases page](https://github.com/aceRage/EdgeSlicer/releases/latest). Unzip and run `EdgeSlicer.exe`; it needs the Edge WebView2 runtime and the VC++ redistributable, usually already present.
- **macOS (Apple silicon)** — the `.dmg` on the [releases page](https://github.com/aceRage/EdgeSlicer/releases/latest). It is **unsigned**, so macOS refuses it the first time: right-click the app → *Open* → *Open*, or run `xattr -dr com.apple.quarantine "/Applications/EdgeSlicer.app"` once.
- **Linux (x86_64)** — the `.AppImage` on the [releases page](https://github.com/aceRage/EdgeSlicer/releases/latest). `chmod +x` it and run it; the host must provide WebKitGTK 4.1 and libOpenGL.

Every release also carries a standalone network plug-in zip and a `SHA256SUMS.txt` covering each file. See [Download and install](#download-and-install) for firewall rules and the data-directory migration.

EdgeSlicer is a fork of Snapmaker Orca, itself a fork of OrcaSlicer. It keeps everything Snapmaker Orca does — Snapmaker U1 / J1 / Artisan / A-series support, the full OrcaSlicer printer library and its print hosts (Klipper/Moonraker, OctoPrint, Duet, PrusaLink and the rest) — and adds Bambu Lab profiles and LAN sending (validated on an H2C), per-part support control, glTF/GLB import, sculpting and cut-tool joints, and a phone / remote access layer with its own companion app.

The application, its binary (`EdgeSlicer.exe`) and its data directory (`%APPDATA%\EdgeSlicer`) all carry the product name, and the icon is an italic **E** whose stem is a katana with a red edge. It installs and runs beside a stock Snapmaker Orca.

---

## Highlights

### Support sets and per-part support groups

- **Support sets** — save the Support-category process settings under a name and apply them to any project. The **Support set** row sits at the top of the process tab's Support page, with *Apply*, a pop-out editor, save, delete and a **Groups** button. A set stores its interface filament as a *type* (*Same as the part*, a loaded material, or PVA / BVOH / HIPS / …), so it travels between printers; applying one copies the values in, after which the process preset shows as modified, like a hand edit.
- **Support groups** — right-click parts in the object list and use the **Support group** submenu to put them in a named group, either empty (seeded from the object's current settings) or created straight from a support set. A group's values live on the parts themselves, so a project 3MF carries them and a stock OrcaSlicer simply ignores the unknown key. Grouped parts get a `[group]` badge in the object list.
- **Per group:** interface filament, interface pattern, top / bottom interface layers and spacing, and support ironing (pattern, flow, line spacing). Support style, threshold angle and top Z distance are stored per group but remain object-wide in effect, and the support *base* geometry is never per group — only the interface and its ironing follow one.
- **Support groups window** (object menu → *Support groups…*, or the *Groups* button) — one row per group showing its parts, interface filament, top Z distance, interface layers, pattern and spacing, and the set it came from; *New*, *Rename*, *Delete*, *Re-apply set* and *Select parts*.
- **Warnings** say plainly what will not follow a group, both in the window and during slicing: classic tree supports take interface *layer count* object-wide (organic trees and normal supports do not); a group asking for a soluble interface forces a 0 mm top Z distance on the whole object; a group's own interface filament switches Support Filament Matching off for that object; and an interface filament on a different nozzle, or not loaded at all, is called out.

### Over-support surfaces

A bottom face resting on support is normally classified as a bridge, so it takes bridge flow and bridge speed and looks nothing like the walls framing it. Turn on **Over-support surfaces** (process tab → Support, off by default) and those faces — plus the wall segments printed over support, in both the classic and Arachne wall generators — get their own surface type, extrusion role and preview colours (*Bottom surface over support*, *Wall over support*), printed at **Over-support flow** and **Over-support speed**, where `0` means "follow the outer wall speed" so the underside matches the perimeters around it. All three keys are per part, so one part in an object can use them while another does not. True bridges over air are untouched, and with the switch off the G-code is byte-identical.

Because supports are generated *after* slicing, the "is there support under this face" test is a reconstruction of what the generator will do. It can over-claim — build-plate-only supports, small-overhang removal and the sharp-tail heuristics are not modelled — in which case a fringe prints as bottom shell instead of a bridge; a support blocker is the escape hatch. The feature stands down entirely at a zero top Z distance, with supports off, and for manual support types outside enforcers.

### Edge Hub — phone and remote access

- The hub is the same binary run as a **tray-only background process**. It serves the phone page and the camera relays, any slicer window starts it, and it keeps running after you close them.
- **Phone access on the LAN** — enable it on the hub page and scan the **QR code**. The phone gets `http://<pc>:13640/r/<token>/` with *Streams*, *Prepare*, *Devices* and *Reprints* tabs: the camera wall, plate previews and estimates, starting a slice, sending to a printer with a printer and toolhead picker, Pause / Resume / Stop, a plate layout editor, and re-sending any file this PC has printed before. In *Reprints*, swipe a row left to delete it. The link is token-gated and survives restarts; *New link* rotates it (and invalidates saved links and home-screen icons).
- **EdgeSlicer app** (iOS and Android) — a companion app that pairs by scanning the same QR, keeps both the home and the remote address and picks whichever is reachable (home network first, Tailscale when away), shows the hub's pages full screen, and receives native push notifications with the app closed. A **Devices** screen lists your printers with state, progress, temperatures and thumbnails, and keeps answering when no slicer window is open — the hub remembers what it last saw and reports the reading's age. Camera streams play in the app, and a stored print can be re-sent from the phone. In testing on both platforms (TestFlight for iOS, Firebase App Distribution for Android); testers are added by invitation, so ask on [Discussions](https://github.com/aceRage/EdgeSlicer/discussions). Push notifications currently go out through the developer's own hub credentials — a hosted push service is planned.
- **Remote access over [Tailscale](https://tailscale.com)** — the hub page detects whether Tailscale is installed, whether you are signed in and whether HTTPS certificates are available, and offers the matching action at each step. It then runs `tailscale serve` for you and gives you an `https://<machine>.<tailnet>.ts.net/…` link with its own QR and a **Copy link** button. No port forwarding and no hosting fees. Access is restricted to an allow-list of Tailscale logins, identified by a header the hub trusts only from a loopback peer, on top of the path token. Remote access and its allow-list are kept apart from the hub's runtime record, so they survive quitting the hub instead of coming back off.
- **Slicer windows** — the hub can open further slicer windows, and **hidden** ones that run with no window at all, so the phone can slice and send without anything appearing on the desktop. A hidden instance that would have raised a dialog nobody can answer flags *needs attention* instead, shown on the hub page and in the tray menu; hidden and visible can be toggled either way at any time.
- The hub page and the tray menu answer only on **loopback**, so no tunnel reaches the control plane.
- **Ports and restarts** — the phone switch is remembered across a hub restart, turning phone access on no longer shuffles the hub off 13640, and a genuine port conflict names the program holding it and the firewall state instead of leaving a dead link. The installer's rule covers **13640-13659**, plus UDP/TCP 8555-8574 for WebRTC camera media.

### Printer event notifications

Each slicer watches its printers and emits an event on a state change — started, resumed, paused, filament runout, finished, failed, cancelled, and new printer error codes with the printer's own explanation — with a cooldown per printer and event kind, and nothing at all on the first poll, so starting the slicer beside a running print does not announce it. LAN printers are probed in parallel and a switched-off one is asked again only every 30 s, so one dead address does not slow the rest. The hub raises a tray balloon and fans the event out to whatever you configure on the hub page's **Notifications** card. Every destination has its own severity filter, **checkboxes for the event kinds it wants** (errors and completions without the start messages, say), a *Test* button and a status badge. The same start seen from two slicer windows is one event.

- **Web Push** to the phone's browser, encrypted end to end (RFC 8291 / 8292). Browsers only allow this from a secure origin, so it wants the page installed to the Home Screen and/or reached over the Tailscale HTTPS link.
- **Pushover**, and **webhooks** — the whole event as JSON, posted to an address you own (Home Assistant, Gotify, Discord, Slack, your own script); https only, except to `127.0.0.1`.
- **Native app push** to the EdgeSlicer app — APNs and FCM with your own credentials, payloads encrypted so Apple and Google see only ciphertext; the app registers itself when it pairs.

### Store G-Code Files

Preferences → Extras → **Store G-Code Files** keeps a copy of every G-code this PC sends to a printer — from the desktop or from the phone — beside a JSON sidecar recording which printer it went to (the same printer identity the API uses), the plate and project, the filaments and the estimates, plus the plate thumbnail when there was one. You choose the **Storage folder** (default `<data dir>/gcode_archive`) and a **Maximum Retention** file count (default 100); past that the oldest records are deleted with their sidecars and previews. Off by default.

Stored files can be listed and deleted through the instance API, and the phone's **Reprints** tab lists them — thumbnail, printer, age, size and filament colours — and sends one again with the same confirmation and progress a fresh print gets (Snapmaker-LAN and print-host printers for now). A phone send deducts filament in Spoolman like a desktop send does.

### Bambu Lab, including dual-nozzle — in progress

- **Profiles** for H2D, H2D Pro, H2C and X2D (dual-nozzle) plus P2S, A2L and H2S, with the full official Polymaker filament catalogue — including Panchroma, Fiberon and PolyLite presets for the H2C, validated against Polymaker's published presets.
- **Printer error texts per series** — HMS and error messages resolve per printer family instead of one shared table.
- **H2C sending works** — the sliced 3MF carries Bambu Studio's multi-nozzle metadata that the H2C firmware requires, and a LAN send waits for the printer to accept the job. The H2C's rack nozzle change is emitted the way Bambu Studio does it, and nozzle offset calibration can be requested from the send dialog. Printer error codes resolve to the printer's own text per device series.
- **P1 and A1 on the LAN** — the network plug-in ships with the Windows packages and the setup wizard turns it on for Bambu printers, so LAN printers are found on a fresh install; sign-in offers the plug-in instead of a dead page when it is missing. A P1 or A1 that started the print no longer reports "did not acknowledge the start command", and uploads honour the printer's SD-card folder.
- **LAN printers reconnect on their own**, with no Bambu cloud login and without opening the Device tab, and a print announces its start once however much the connection flaps. Every live MQTT session is bounced when the PC resumes from sleep so the Device page comes back.
- **Later-batch model codes** — an H2C from a newer batch reports itself as `O1C2-V2`; model codes resolve through the printer definitions' sub-series list with a revision-strip fallback, over SSDP, MQTT and the cloud list alike.
- **Dual-nozzle slicing** — filaments are grouped onto nozzles automatically (from the connected printer's AMS layout when one is attached), cross-nozzle changes skip the purge, and the grouping is saved in the project 3MF. Dual-extruder machines can load every colour on the first layer, so nothing waits on a mid-print load.
- **Mixed nozzle sizes** — each extruder can have its own nozzle diameter. Every feature is mapped to the extruder that can print it, and widths and layer heights are checked against the nozzle actually used; changing one diameter no longer forces it onto the others.
- **Nozzle flow type** (Standard / High Flow) is declared in sliced files and matched to the installed nozzle at send time.
- **Import Bambu Studio user presets** — one-way mirror of your custom print / filament / machine presets, at startup or via *Sync now*.
- **Printer connectivity** — a network plug-in (UltraNet), developed separately and not part of this repository, adds live status, camera and send-to-printer for supported Bambu Lab machines (Preferences → Extras → Bambu Network). It is bundled with the Windows packages. Live view additionally needs Bambu's own camera component, which cannot be redistributed here, so the Device tab offers to download it once.
- **Status:** slicing is verified on every model; LAN sending and printing are validated on an H2C. Treat the rest of the group as beta.

### Printer devices

Several printers of the same model each get their own entry in a **Devices** list, so they are no longer one shared slot. Printing is enabled as soon as any of them is known, the send dialog has a device dropdown and the Device tab a picker, and Elegoo uploads go to the Elegoo host rather than the Snapmaker page.

On the **Snapmaker U1**, *Unload filaments after print* is a machine setting with a per-print checkbox in the send dialog; the printer unloads the toolheads it used when the job ends. U1 jobs are recorded in Reprints.

**FlashForge Creator 5 / Adventurer 5** print over the local network: the port-8898 HTTP API with discovery and the material station, a **serial number and check code per device** (so two machines under one preset each speak to their own printer), and *Flow dynamic control* and *First layer inspection* as checkboxes on the send dialog. The plate uploads as a proper `.gcode.3mf` with per-nozzle slice info, so the printer shows the thumbnail instead of freezing on the file. **Export plate sliced file** and **Export all sliced file** are in the Print dropdown for every printer.

### Multicolor and materials

- **Support Filament Matching** (opt-in) — supports, interfaces, ironing and brims print in the colour of the surface they touch; **Brim filament → Nearest wall** for brims alone.
- **Outer wall filament** separate from inner walls.
- **Paint depth** — a painted multi-material claim can be bounded by wall count or by distance instead of running all the way through the part.
- **Split by painted colour** — turn a painted model into separate parts, one closed shell per painted region, placed back where they were.
- **Filament colours and count survive printer switches**; **Apply All** sets every filament slot in one click; **per-filament Z offset**.
- **Spool Manager** — [Spoolman](https://github.com/Donkie/Spoolman) inventory, spool-to-slot bindings, and optional automatic usage deduction when a job is sent. A Reprint or a send from the phone deducts filament the same way a desktop send does.

![Normal and tree supports printing in the colour of the surface they touch, and the Support Filament Matching option](docs/images/support-matching.png)
![Four brims each printed in the colour of the wall they touch, and the Brim filament: Nearest wall setting](docs/images/brim-match.png)
![Apply All, the Outer wall filament setting and the Spool Manager preferences](docs/images/materials.png)

### Assembly and fitting

- **Auto-Fit Assembly** — right-click a multi-selection: the largest part stays fixed, the rest mate to it by matching faces, holes and pegs, and everything merges into one print-ready object.
- **Assemble tool → Auto-fit** — pick one feature on each part (flat face, circular rim, a single triangle or a curved patch), press *Auto-fit*, nudge with the live **Rotate / Offset** sliders, then **Merge parts**. Rotation is chosen by outline fit *and* a whole-mesh collision check, so pegs seat square without intersecting.
- New assembly modes **Triangle and triangle** and **Curve and curve**; **Point and point** snaps to vertices at any zoom and has one-click **Coincide points**.
- **Flexi joints in the Cut tool** — join the halves of a cut part with a print-in-place joint instead of a plug: a double ring, a ball and socket, a chain link, a hinge with a chosen number of knuckles that lets the halves fold shut, a screw-top **thread**, or a quarter-turn **bayonet** lock. Set the clearance and turn the joint to suit the part. Joints survive saving and reopening a project, and the slicer warns when a gap-closing setting would fuse them.
- **Curved cut** — the cut plane can bend into a curved sheet: drag the control points, snap them to the model with a right-click drag, and give either half Visible, Ghost or Hidden so the result reads from both sides. The control grid takes separate column and row counts, so a 10 x 2 grid is a true ruled bend with every column grabbable from either end. Both halves are always kept, and connectors, Flexi joints included, sit on the curved surface.
- **Draw cut** — paint a stroke on the part and it becomes the cutter: a swept surface following the surface normal, the view or an axis, with **Extension**, **Depth**, **Thickness** and a lip **Angle** (0 a shelf, 45 a chamfer, 90 a plug), so you can sink a recess as readily as split a part. Strokes continue from either endpoint and a snap radius closes the loop; *Through all* projects inward through the part. It works on curved surfaces and on real cylinders, including a band that wraps all the way round, and connectors stand on the drawn surface.
- **Copy cut to… and Mirror** — mirror a cut about X, Y or Z from the Cut panel, or copy an existing cut onto another object from the right-click menu, for matched halves and repeated splits across a set of parts.
- **Re-editable cuts** — a cut keeps its pre-cut mesh and its recipe in the project 3MF, so **Edit cut…** reopens the gizmo on the uncut object with the plane, sheet, stroke and connectors where you left them. Covers flat, curved and drawn cuts; toggled by *Keep cut editable*.
- **Cut thickness** — flat and curved cuts can remove a slab of material (a kerf) centred on, above or below the cut, so reassembled halves leave room for glue or a joint; connectors grow to span the gap. Zero, the default, cuts as before.

![Assemble tool: the four assembly modes, and a curve-and-curve mate with Auto-fit, live Rotate / Offset sliders and Merge parts](docs/images/assembly.png)

### Modeling and Prepare view

- **glTF / GLB import** — meshes, node hierarchies and transforms, materials mapped to filaments through the same colour dialog OBJ uses, vertex colours and textures sampled to painted faces. Draco-compressed files are refused with a clear message.
- **Sculpt** — a sculpting tool on the toolbar: grab, inflate, deflate, smooth, flatten, crease, pinch, magnify, nudge, snake hook and clay strips, with undo per stroke and a mask brush that protects painted vertices. The bed contact and sharp edges are protected automatically, and painted supports, seams and colours stay where they are. Press F and move the mouse to resize the brush, Shift+F for strength, hold Ctrl to invert; a coarse mesh can be subdivided from the same panel.
- **Image Fill** — project an image onto a part (flat, wrapped, box, or from the model's own texture coordinates) and print it in the loaded filaments. With nozzle-resolution dithering on, the top surface is built from short colour runs following the image, so shades between your filaments come out as mixes rather than nearest-colour blocks.
- **Fill bed with copies** — a dialog for the gap between copies, the distance from the bed edge, an optional larger margin at the front (where printers draw their calibration lines) and whether copies may rotate, with a live count. Compact packing no longer gives up early and leaves strips empty, a **Grid** layout gives an exact regular block, and filling plate 2 or later puts the copies on that plate. When it cannot place more copies it names the support clearance as the reason, and can waive it. The entry sits under *Clone* in the object menu.
- **Scale to build volume** — on the object menu for every printer, with a dialog for uniform or per-axis scaling, an edge gap, a top gap and auto-centre. Non-uniform scaling of a rotated part warns before it shears.
- **Edit (CAD) gizmo** — select a planar face or a feature-edge chain and **push/pull** it with a real stem-and-cone handle, the surrounding geometry stretching to follow; self-intersecting pushes are allowed with a warning rather than blocked. **Bevel / chamfer** an edge chain with a width slider, including faceted rims: a cylinder's or cone's top edge chamfers geometrically in milliseconds, and a chain too curved for that falls back to a voxel-rounded band instead of refusing. The bevel runs in a cancellable worker, and the commit preserves painted data.
- **Slice bake** — *Bake slice to mesh…* turns an object's slices back into a solid mesh, one closed prism per layer with zero open edges, so a 0.3 mm fuzzy skin survives a re-slice at 0.12 mm. Bake from the slice contours or from the extrusion paths, replace the object or add it as a new one, with a Resolution control and *Smooth vertical steps*.
- **Quad remesh** — an optional QuadriFlow quad remesher in the object menu and the Sculpt panel, deterministic run to run; it refuses non-edge-manifold assemblies with a clear message and runs off the UI thread.
- **Round all edges** — a whole-mesh fillet by radius, with no bottom slab by default, so vertical fillets run to the bed with rounded footprint corners while the base stays flat and sharp.
- **Mesh booleans** on the Manifold backend (automatic fallback) with a part picker (Union no longer comes back inside-out on mirrored volumes); **Repair/Remesh** asks for a voxel size, keeps the bottom exactly flat and snaps sharp edges back.
- **Visibility** — Normal / Ghost (X-ray) / Hidden per object or part, with an eye column in the object list.
- **Move panel align & distribute**, **bottom-referenced Z**, **keep imported Z** (drop-to-bed toggle), double-click to select a part.
- **Assemble Separately / Separate** parts out of an assembly in place; **Merge into Single Part** keeps paint and seam annotations.

### Print quality

- **Offset layers (experimental)** — odd-numbered walls are shifted by half a layer height so layers interlock, on both the classic and the Arachne wall generator. It needs a first layer height equal to the layer height, matching top-surface and outer-wall line widths, and spiral mode off; the slicer offers to fix them for you when you switch it on.
- **Z contouring** — smooths the staircase on shallow top surfaces by shaping each layer to the model. Speed scaling keeps the flow steady across the shaped path, and it works with layer cooling on. Off by default; tune the angle and minimum height per profile.
- **Locked Zag infill** — the skin and the skeleton can each take their own infill pattern, as in Bambu Studio, and *skin follows the surface* hugs the contour on sloped tops. Existing profiles slice exactly as before until you opt in, and Bambu profiles that set these keys import as-is.
- **Fuzzy skin** — overhanging wall segments can be left unfuzzed, so the jitter stops pushing extrusion into open air, and a width floor tied to the layer height keeps Extrusion and Combined modes from aborting a slice.
- **Layer time speed smoothing** — limits how much the estimated print time may jump between adjacent layers, which otherwise shows as banding on glossy and high-shrinkage filaments. Pick *Speed up (exclude outer walls)*, *Speed up (all)* or *Slow down*, set the **Max layer time variation**, and cap the result with **Max speed-up**, **Max slowdown** and **Max print time increase**. The prime/wipe tower is protected in every mode and nothing is taken below what the cooling stage already set. Off by default.
- **Seam position** can be Left or Right as well as Back.
- **Create filament** dialog is sorted and easier to scan.
- **Print unsupported walls last**, **Undertop surface pattern**, **Z overrides X/Y** support option, **machine prepare time** in estimates.
- **Deterministic slicing** — slice lines are sorted after the parallel pass, so the same project gives the same G-code every time, at any thread count. Internal bridges anchor correctly over Hilbert Curve and Octagram Spiral infill, and the wipe tower's footprint, placement and preview match what is printed, from the command line too.

![Offset layers and Z overrides X/Y print settings](docs/images/print-quality.png)

### Workflow and UX (Preferences → Extras)

The fork's own settings live on one **Extras** tab, in six sections:

- **Project** — Auto-Save project (with an interval), Keep my printer when opening project files, Skip Settings Mapping Warnings, Drop imported models to the bed, Bottom-referenced Z position.
- **Presets** — Prefer Last Used Print Profile, Seamless System Filament Edits, Import Bambu Studio user presets (plus *Sync now*).
- **Bambu Network** — enable the plugin, Stealth mode (no Bambu cloud telemetry), legacy plugin for old firmware.
- **Spool Manager** — enable Spoolman, server URL, deduct filament usage when sending a print.
- **Phone access** — start hidden (serve the phone without a window), and remember Snapmaker printer certificates so the phone can connect them (off by default, since it keeps a private key at rest).
- **G-Code Archive** — Store G-Code Files, storage folder, maximum retention.

**Startup is about twice as fast** — vendor profile JSONs are parsed in parallel with a per-vendor preset cache, and the Calibration, Monitor, Multi-machine and Project panels are built the first time you open them rather than at launch. The splash screen animates with the real loading progress while it happens. The Cut, Assembly, Sculpt and Edit gizmo panels each have a **pin that docks them to the right edge** of the view, remembered per gizmo, with a collapsible title row.

Elsewhere: **Compare Slices** (View menu) — settings, time and filament diff plus per-layer toolpath comparison of any two slices or sliced files. **Import Config from G-code**. `edgeslicer://` links, with the older `ultraone://`, `snapmaker-orca://` and Printables' `orcaslicer://` spellings still accepted. Silent exit.

**Stream tab** — a live camera wall up to 9×9 for Snapmaker U1, Moonraker, Bambu Lab (LAN liveview) and Flashforge cameras, plus any RTSP / ONVIF IP camera (Reolink, Amcrest, Hikvision, Tapo, …; Wyze via RTSP firmware or wyze-bridge) with LAN discovery; drag names to reorder. It bundles the open-source [go2rtc](https://github.com/AlexxIT/go2rtc) helper and an LGPL ffmpeg, and shares the phone link and QR code described under Edge Hub. Phone streams play over **WebRTC** (with STUN, and an MSE fallback over the Tailscale HTTPS link), with per-phone Video and Quality settings where the Medium and Low steps genuinely transcode; each tile takes its own camera's aspect ratio.

![Auto-Save project setting on the fork's own preferences tab](docs/images/options-autosave.png)
![Compare Slices window: time and filament deltas, the changed setting, and a per-layer toolpath overlay](docs/images/compare-slices.png)
![Stream tab showing five printer cameras in a 3x2 wall](docs/images/menu-stream.jpg)

### Fixes to upstream issues

Plate deletion during a slice no longer crashes; reopening a project with a named plate no longer crashes; fuzzy skin no longer leaves dots or seam blobs; JSON profiles with unquoted numbers/booleans keep their settings; Repair no longer produces inside-out meshes; forced preset and U1 → U1 device switches no longer pop the transfer/discard dialog; the false nozzle-mismatch nag on Bambu sends is gone; face-and-face assembly snapping picks the facet under the cursor again; the preset comparison dialog no longer crashes on missing options or old presets; over-long ASCII STL scans are bounded; the setup wizard picks a sensible default printer instead of an arbitrary one; and a missing icon can no longer block the app, so the Flashforge Device tab opens.

---

## Supported printers

| Family | Status |
|---|---|
| **Snapmaker** U1, J1, Artisan, A250 / A350 (Dual, Quick Swap, Bracing Kit variants) | Inherited from Snapmaker Orca; **physically validated on the U1** |
| **Bambu Lab** A1 mini, A1, A2L, P1P, P1S, P2S, X1, X1 Carbon, X1E, H2S, H2D, H2D Pro, H2C, X2D | Slicing verified; **LAN sending validated on the H2C**, and P1 / A1 LAN status and sending work with the bundled plug-in; live view needs Bambu's camera component, downloaded on request |
| **Flashforge** Creator 5, Creator 5 Pro, Adventurer 5 series | Profiles, plus **local-network sending over the HTTP API**; validated on a Creator 5 |
| OrcaSlicer vendor library (Creality, Prusa, Voron, QIDI, Anycubic, Elegoo, Sovol, …) | Unchanged from upstream, with **Creality brought to parity** (K2 / K2 Pro / K2 SE, K1 CFS-C variants, Ender-3 V4, SparkX i7), **Prusa CORE One L / L HF**, **Elegoo Centauri Carbon 2 / Centauri 2**, Sovol SV07 0.6 / 0.8 / 1.0, and QIDI / Anycubic refreshed |

Print hosts are inherited from OrcaSlicer and unchanged. The **Host Type** list is PrusaLink, PrusaConnect, **Octo/Klipper**, Duet, FlashAir, AstroBox, Repetier, MKS, ESP3D, CrealityPrint, Obico, Flashforge, SimplyPrint and Elegoo Link — a Klipper machine running Moonraker goes under *Octo/Klipper*; there is no separate Moonraker entry in the dropdown.

---

## Download and install

All builds are on the [Releases](https://github.com/aceRage/EdgeSlicer/releases) page. Each release carries a **Windows installer**, a **Windows portable zip**, a **Linux AppImage** and a **macOS dmg**.

- **Windows installer** — installs **side by side** with the official Snapmaker Orca (its own folder, Start-menu entry and Add/Remove entry) and upgrades a previous EdgeSlicer install. It has its own name, icon and data directory, so the two are easy to tell apart. It also adds two firewall rules: **TCP 13640** for the Edge Hub and **UDP 2021 / 1990** for Bambu LAN discovery, so LAN printers are found without waiting on Windows' first-run prompt.
- **Windows portable** — unzip and run `EdgeSlicer.exe` (needs the Edge WebView2 runtime and the VC++ redistributable, usually already present). It asks for the firewall once on first start.
- **Linux (x86_64)** — `chmod +x` the AppImage and run it. The host must provide WebKitGTK 4.1 and libOpenGL (Ubuntu: `libwebkit2gtk-4.1-0 libopengl0`); they are not bundled.
- **macOS (Apple silicon)** — the dmg is **unsigned** (no Apple Developer account), so macOS refuses it the first time: right-click the app → *Open* → *Open*, or run `xattr -dr com.apple.quarantine "/Applications/EdgeSlicer.app"` once.

Both Windows packages include the Bambu network plug-in (UltraNet); the Linux and macOS builds currently do not. A **standalone plug-in zip** is published beside them, to repair an earlier install that predates the bundling. The Linux AppImage and the macOS dmg are automated, unsigned builds of the same commit and have not been tried on hardware.

### Data directory and migration

Settings, presets and the hub's state live in `%APPDATA%\EdgeSlicer` (`EdgeSlicer.conf`). The fork has been renamed twice — Snapmaker_Orca → UltraOne → EdgeSlicer — so on first run it **copies** (never moves) the newest legacy data directory it finds into the new one, once, rewriting absolute paths in the config. On Windows it also carries over the embedded browser's profile (`%LOCALAPPDATA%\<name>\EBWebView`), which holds the Stream tab's layout and your Snapmaker account session. Older URL schemes, file associations and uninstall keys stay recognised, so links and shortcuts made under the old names keep working.

### Privacy and crash reports

EdgeSlicer sends no usage statistics. It can send **crash reports**, and they are **opt-in**: *Preferences → General → Privacy → Send crash reports*, off by default. The only time EdgeSlicer asks is once, as a notification, on the first start after a crash. Turning the setting off takes effect at once.

A crash report holds:
- the crash stack trace (a minidump);
- the app version;
- your operating system;
- the last warning and error lines of the app log.

Those log lines are scrubbed before they are stored. IP addresses, host names, printer serial numbers, access codes, passwords, tokens, API keys, e-mail addresses and the user name in file paths are all replaced. Log files, configs, projects and presets are never sent. Builds you compile yourself, forks and the Linux builds have no crash-report destination and never send anything. The details, including exactly what a minidump carries, are in [docs/privacy.md](docs/privacy.md).

---

## Build from source

Same toolchain as Snapmaker Orca / OrcaSlicer (CMake, C++17, wxWidgets). Dependencies build into `deps/build/OrcaSlicer_dep`, which is then passed to the slicer configure as `CMAKE_PREFIX_PATH=<that>/usr/local`; the slicer builds into `build/` and installs into `build/Snapmaker_Orca/` (the install folder still carries the old name).

```bash
build_release_vs2022.bat            # Windows: VS 2022, CMake <= 3.31, git-lfs, Strawberry Perl (deps | slicer | debug)
./build_release_macos.sh            # macOS: -d deps, -s slicer, -a arm64|x86_64|universal
./build_linux.sh -u && ./build_linux.sh -dsi   # Linux: system deps, then deps + slicer + AppImage
./build_flatpak.sh                  # Flatpak
cd build && ctest --output-on-failure          # tests (Catch2)
```

On Windows the build produces `EdgeSlicer.dll` (the slicer itself) and a small `EdgeSlicer.exe` shim that loads it; the product name is defined once, in `src/common_func/common_func.hpp`, and everything else — CMake, the bundle id, the installer, the data directory — is derived from it.

Headless slicing for scripts and agents: `python scripts/orca_cli.py --exe <EdgeSlicer.exe> --printer "Snapmaker U1 (0.4 nozzle)" --process "0.20 Standard @Snapmaker U1 (0.4 nozzle)" --filament "Snapmaker PLA Matte @U1" --export-3mf out.3mf model.stl` slices with presets by name, streams JSON progress, and returns time/filament estimates, G-code paths and warnings (`result.json`); see [docs/superpowers/specs/2026-09-01-headless-slicer-roadmap.md](docs/superpowers/specs/2026-09-01-headless-slicer-roadmap.md).

Windows packaging: `cpack -G NSIS` in `build/` produces the installer (needs NSIS); zip the install folder for the portable build. The optional connectivity plugin lives outside this tree and is only built when `src/ultranet/CMakeLists.txt` exists, so the repository builds without it. Run `git lfs pull` after cloning on Windows. See [`CLAUDE.md`](CLAUDE.md) and [`AGENTS.md`](AGENTS.md) for details.

---

## Status and roadmap

The current release is **[v2.3.8.5-edge](https://github.com/aceRage/EdgeSlicer/releases/latest)**. Releases before v2.3.8.0-edge shipped without the LAN discovery firewall rule, the live-view camera component or the network plug-in and have been removed, so there is no upgrade path from them other than installing fresh — your data directory is migrated automatically (see above).

Known limits and work in progress, stated plainly:

- **Bambu Lab** — slicing is verified; hardware validation covers the H2C, not yet the other models.
- **Previews** — draw cut, slice bake, quad remesh, the Edit gizmo's bevel and push/pull, curved cut, Flexi joints (thread and bayonet included), Sculpt, Image Fill dithering and the fill-bed Grid layout are verified against test fixtures but have not yet been printed on hardware by the maintainer; check the first print.
- **Layer time speed smoothing** — new; verify against a known-good print before using it on a long job.
- **Reprints** — Snapmaker-LAN and print-host printers; Bambu and the PC's own connection are not yet reprintable from the phone.
- **Linux and macOS builds** — automated, unsigned, and not yet tried on hardware.
- **Support groups** — the support *base* is never per group, and classic tree supports take interface layer count object-wide.
- **Flashforge** — local-network sending over the HTTP API is validated on a Creator 5; the Adventurer 5 path is ported from upstream and untested here. The older TCP-8899 device tab still needs the vendor's own network library, which is **not** distributed with this fork.
- **Dual-nozzle follow-ups** — nozzle-aware tool ordering and per-nozzle AMS slot mapping in the send dialog.
- The EdgeSlicer identity is applied on Windows; the side-by-side identity on the Linux and macOS builds is still to do.

---

## Lineage and licence

EdgeSlicer is licensed under the **GNU Affero General Public License, version 3** ([`LICENSE.txt`](LICENSE.txt)). It is a fork of [Snapmaker Orca](https://github.com/Snapmaker/OrcaSlicer) (Snapmaker), based on [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer) by SoftFever, based on [Bambu Studio](https://github.com/bambulab/BambuStudio) by Bambu Lab, based on [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer) by Prusa Research, based on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community; OrcaSlicer also incorporates features from SuperSlicer by @supermerill. All are AGPL-3.0: if you use any part of this software in any way, even behind a web server, your software must be released under the same licence.

Ported features: align/distribute helpers, "Sub merge" (our *Assemble Separately*) and *Z overrides X/Y* from Bambu Studio; *Print unsupported walls last* (#15411), *Merge into Single Part* (#15413), *Undertop surface pattern* (#15389), the JSON-config fix (#15370), per-filament Z offset (#4660), drop-to-bed / bottom-referenced Z (#8194, #5315), machine prepare time (#5796) and the Flashforge Creator 5 profiles (#13259) from OrcaSlicer; the Flashforge device stack from the Orca-Flashforge project.

Third-party components added by this fork: [libvgcode](https://github.com/prusa3d/PrusaSlicer/tree/master/src/libvgcode), the G-code preview renderer from PrusaSlicer 2.8 by Prusa Research (Enrico Turri, Pavel Mikuš, Vojtěch Bubník; AGPL-3.0-or-later), as ported to OrcaSlicer by [#10735](https://github.com/OrcaSlicer/OrcaSlicer/pull/10735) with its later OrcaSlicer changes (vendored in `src/libvgcode`, file headers kept, EdgeSlicer edits marked `// EDGE`); [Manifold](https://github.com/elalish/manifold) (Apache-2.0); [go2rtc](https://github.com/AlexxIT/go2rtc) v1.9.14 (MIT, bundled unmodified in `resources/tools/go2rtc`); Polymaker presets from [Polymaker3D/Polymaker-Preset](https://github.com/Polymaker3D/Polymaker-Preset) (MIT, notice in `resources/profiles/BBL/filament/PANCHROMA_POLYMAKER_LICENSE.txt`); the nearest-mix colour solver from [OrcaSlicer-ImageMap](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap) (AGPL-3.0, Copyright (C) 2026 sentientstardust; vendored trimmed in `deps_src/colorsolver`, notice in `deps_src/colorsolver/NOTICE`); the pressure-advance calibration pattern adapted from Andrew Ellis' generator (GPL-3.0), itself adapted from Sineos' Marlin generator (GPL-3.0). No proprietary printer-vendor network libraries are included in this repository.

Snapmaker, Bambu Lab, Flashforge, Tailscale and other names are trademarks of their respective owners; this project is not affiliated with or endorsed by any of them.

---

## Contributing

Bug reports and feature requests go to [GitHub Issues](https://github.com/aceRage/EdgeSlicer/issues) — include the release version, printer and a project `.3mf` where possible. Pull requests target **`main`**; read [`AGENTS.md`](AGENTS.md) for layout and conventions, keep fork-specific settings on the Extras preferences tab, and prefer porting from upstream with attribution over re-implementing.

Security issues: please use a [private security advisory](https://github.com/aceRage/EdgeSlicer/security/advisories/new) on GitHub rather than a public issue.
