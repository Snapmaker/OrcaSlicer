# EdgeSlicer Bridge for Blender.
#
# One file, two uses:
#
#  * Installed as a Blender add-on, it adds "Send to EdgeSlicer" (Object menu, File > Export and the
#    EdgeSlicer tab in the 3D view sidebar). The selected meshes are written as STL files and handed
#    to EdgeSlicer, which adds them to the open project (or starts EdgeSlicer if it is not running).
#
#  * Run by EdgeSlicer's "Edit in Blender" as `blender --python edgeslicer_bridge.py -- <args>`, it
#    opens one part for editing. Saving the file (Ctrl+S) or pressing "Send back to EdgeSlicer" writes
#    the edited mesh to the file EdgeSlicer is watching, and EdgeSlicer swaps it into the part.
#
# Units: one Blender unit is one millimetre, matching Blender's own STL exporter defaults. The
# edit session sets the scene to show millimetres so sizes read the same in both programs.
#
# STL reading and writing is done here with numpy rather than through Blender's importers, whose
# operator names changed between Blender 3.x and 4.x.

bl_info = {
    "name": "EdgeSlicer Bridge",
    "author": "EdgeSlicer",
    "version": (1, 0, 2),
    "blender": (3, 0, 0),
    "location": "View3D > Sidebar > EdgeSlicer, Object > Send to EdgeSlicer",
    "description": "Send meshes to EdgeSlicer and send parts opened from EdgeSlicer back to it",
    "category": "Import-Export",
}

import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

import bpy
import numpy as np
from bpy.app.handlers import persistent

ADDON_ID = "edgeslicer_bridge"

# Scene custom properties that mark a scene as an EdgeSlicer edit session. They are saved in the
# .blend, so reopening that file later still sends back to the same part while EdgeSlicer watches it.
PROP_OUT = "edgeslicer_out"
PROP_EXE = "edgeslicer_exe"
PROP_NAME = "edgeslicer_name"
PROP_SENT = "edgeslicer_last_sent"
# Object custom property marking the mesh that came from EdgeSlicer.
PROP_PART = "edgeslicer_part"

# While above zero, saves do not send the mesh back (see start_edit_session).
_suppress_send = 0


# ------------------------------------------------------------------------------------------------
# STL I/O
# ------------------------------------------------------------------------------------------------

_STL_BINARY_DTYPE = np.dtype([("normal", "<f4", (3,)), ("v", "<f4", (3, 3)), ("attr", "<u2")])


def read_stl(path):
    """Return (vertices Nx3 float32, faces Mx3 int32) with shared vertices merged."""
    with open(path, "rb") as f:
        data = f.read()
    tris = None
    if len(data) >= 84:
        count = int(np.frombuffer(data, "<u4", 1, 80)[0])
        if 84 + 50 * count == len(data):
            tris = np.frombuffer(data, _STL_BINARY_DTYPE, count, 84)["v"].reshape(-1, 3)
    if tris is None:
        text = data.decode("utf-8", "replace")
        nums = re.findall(r"^\s*vertex\s+(\S+)\s+(\S+)\s+(\S+)", text, re.MULTILINE)
        tris = np.array(nums, dtype=np.float32).reshape(-1, 3)
    if len(tris) == 0:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.int32)
    verts, inverse = np.unique(tris, axis=0, return_inverse=True)
    faces = inverse.reshape(-1, 3).astype(np.int32)
    # Merging can collapse a sliver triangle to a line; Blender would reject it anyway.
    keep = (faces[:, 0] != faces[:, 1]) & (faces[:, 1] != faces[:, 2]) & (faces[:, 0] != faces[:, 2])
    return verts.astype(np.float32), faces[keep]


def write_stl(path, vertices, faces, header=b"EdgeSlicer Bridge"):
    """Write a binary STL atomically, so a watcher never sees a half-written file."""
    tri = vertices[faces].astype(np.float32)  # M x 3 x 3
    normals = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
    lengths = np.linalg.norm(normals, axis=1, keepdims=True)
    normals = np.divide(normals, lengths, out=np.zeros_like(normals), where=lengths > 0)
    records = np.zeros(len(faces), _STL_BINARY_DTYPE)
    records["normal"] = normals
    records["v"] = tri
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(header[:80].ljust(80, b" "))
        f.write(np.array([len(faces)], "<u4").tobytes())
        f.write(records.tobytes())
    os.replace(tmp, path)


def mesh_from_arrays(name, vertices, faces):
    me = bpy.data.meshes.new(name)
    me.from_pydata(vertices.tolist(), [], faces.tolist())
    me.validate()
    me.update()
    return me


def objects_to_arrays(objects, scale=1.0):
    """Triangulate the evaluated meshes (modifiers applied) in world space and stack them."""
    depsgraph = bpy.context.evaluated_depsgraph_get()
    all_verts, all_faces, offset = [], [], 0
    for obj in objects:
        if obj.mode == "EDIT":
            obj.update_from_editmode()
        evaluated = obj.evaluated_get(depsgraph)
        me = evaluated.to_mesh()
        try:
            me.calc_loop_triangles()
            n = len(me.vertices)
            co = np.empty(n * 3, np.float32)
            me.vertices.foreach_get("co", co)
            co = co.reshape(-1, 3)
            tri = np.empty(len(me.loop_triangles) * 3, np.int32)
            me.loop_triangles.foreach_get("vertices", tri)
            mat = np.array(obj.matrix_world, dtype=np.float64)
            co = (co @ mat[:3, :3].T + mat[:3, 3]) * scale
            all_verts.append(co.astype(np.float32))
            all_faces.append(tri.reshape(-1, 3) + offset)
            offset += n
        finally:
            evaluated.to_mesh_clear()
    if not all_verts:
        return np.zeros((0, 3), np.float32), np.zeros((0, 3), np.int32)
    return np.concatenate(all_verts), np.concatenate(all_faces)


# ------------------------------------------------------------------------------------------------
# Finding and starting EdgeSlicer
# ------------------------------------------------------------------------------------------------

def _log(message):
    # Goes to the console Blender was started from (Window > Toggle System Console on Windows), so a
    # send that reports success but shows nothing in EdgeSlicer can be traced to the exe and the
    # arguments that were used.
    try:
        print("EdgeSlicer Bridge: " + message, flush=True)
    except Exception:
        pass


def _prefs():
    addon = bpy.context.preferences.addons.get(ADDON_ID)
    return addon.preferences if addon else None


def _remember_session_exe(exe):
    # The EdgeSlicer that opened this Blender is the one the user is working in. Keep it as the default
    # for sends from other files, so they do not fall back to a different install.
    prefs = _prefs()
    if prefs is None or not exe or getattr(prefs, "edgeslicer_last_exe", None) is None:
        return
    if prefs.edgeslicer_last_exe == exe:
        return
    prefs.edgeslicer_last_exe = exe
    if not bpy.app.background:
        try:
            bpy.ops.wm.save_userpref()
        except Exception:
            pass


def default_edgeslicer_paths():
    if sys.platform == "win32":
        roots = [os.environ.get("ProgramW6432"), os.environ.get("ProgramFiles"), r"C:\Program Files"]
        return [os.path.join(r, "EdgeSlicer", "EdgeSlicer.exe") for r in roots if r]
    if sys.platform == "darwin":
        return ["/Applications/EdgeSlicer.app", os.path.expanduser("~/Applications/EdgeSlicer.app")]
    found = [shutil.which(n) for n in ("EdgeSlicer", "edgeslicer")]
    return [p for p in found if p]


def find_edgeslicer(scene=None):
    # Precedence: the EdgeSlicer that started this edit session (its exe is saved in the scene), then
    # the location typed into the add-on preferences, then the EdgeSlicer that started the last
    # session, then the usual install location. The session comes first because a hand-off only
    # reaches an EdgeSlicer of the very same executable: another install (for example the normal
    # install while a test build is open) would start or find a different instance, and the files
    # would never appear in the window the user is looking at.
    prefs = _prefs()
    candidates = []
    if scene is not None and scene.get(PROP_EXE):
        candidates.append(("the edit session", scene[PROP_EXE]))
    if prefs and prefs.edgeslicer_path:
        candidates.append(("the add-on preferences", bpy.path.abspath(prefs.edgeslicer_path)))
    last = getattr(prefs, "edgeslicer_last_exe", "") if prefs else ""
    if last:
        candidates.append(("the last edit session", last))
    candidates += [("the default install location", p) for p in default_edgeslicer_paths()]
    for source, path in candidates:
        if path and os.path.exists(path):
            _log("using the EdgeSlicer from %s: %s" % (source, path))
            return path
        if path:
            _log("ignoring the EdgeSlicer from %s, it does not exist: %s" % (source, path))
    _log("no EdgeSlicer found")
    return None


def launch_edgeslicer(exe, files):
    # --single-instance hands the files to an EdgeSlicer that is already open instead of starting a
    # second window. On macOS an .app bundle goes through `open`, which does the same thing.
    if sys.platform == "darwin" and exe.endswith(".app"):
        argv = ["open", "-a", exe] + files
    else:
        argv = [exe, "--single-instance"] + files
    kwargs = {"close_fds": True}
    if sys.platform == "win32":
        kwargs["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs["start_new_session"] = True
    # When no EdgeSlicer is open this starts one. Somebody just asked for these files, so it must
    # show a window even if EdgeSlicer is set to "Start hidden" (an open one is unaffected).
    env = dict(os.environ)
    env["SNORCA_HIDDEN"] = "0"
    kwargs["env"] = env
    _log("launching %s" % argv)
    proc = subprocess.Popen(argv, **kwargs)
    _log("launched, pid %d" % proc.pid)


def _safe_filename(name):
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(" .")
    return name or "part"


def _send_dir():
    base = os.path.join(tempfile.gettempdir(), "EdgeSlicer-from-Blender")
    # Old sends are only useful until EdgeSlicer has loaded them.
    try:
        for entry in os.listdir(base):
            full = os.path.join(base, entry)
            if os.path.isdir(full) and time.time() - os.path.getmtime(full) > 24 * 3600:
                shutil.rmtree(full, ignore_errors=True)
    except OSError:
        pass
    path = os.path.join(base, time.strftime("%Y%m%d-%H%M%S") + "-%d" % (int(time.time() * 1000) % 1000))
    os.makedirs(path, exist_ok=True)
    return path


def _unit_scale(scene):
    prefs = _prefs()
    if prefs and prefs.use_scene_units and scene.unit_settings.system != "NONE":
        # Blender units to millimetres.
        return scene.unit_settings.scale_length * 1000.0
    return 1.0


# ------------------------------------------------------------------------------------------------
# Edit session started by EdgeSlicer
# ------------------------------------------------------------------------------------------------

def parse_session_args(argv):
    if "--" not in argv:
        return None
    args = argv[argv.index("--") + 1:]
    opts = {}
    i = 0
    while i < len(args):
        if args[i].startswith("--edgeslicer-") and i + 1 < len(args):
            opts[args[i][len("--edgeslicer-"):]] = args[i + 1]
            i += 2
        else:
            i += 1
    if "in" not in opts or "out" not in opts:
        return None
    return opts


def _frame_part():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type != "VIEW_3D":
                continue
            space = area.spaces.active
            space.clip_start = 0.1
            space.clip_end = 100000.0
            region = next((r for r in area.regions if r.type == "WINDOW"), None)
            if region is None:
                continue
            try:
                with bpy.context.temp_override(window=window, area=area, region=region):
                    bpy.ops.view3d.view_selected()
            except Exception:
                pass
    return None  # run once


def start_edit_session(opts):
    global _suppress_send
    scene = bpy.context.scene
    for obj in list(scene.objects):
        bpy.data.objects.remove(obj, do_unlink=True)

    units = scene.unit_settings
    units.system = "METRIC"
    units.scale_length = 0.001
    units.length_unit = "MILLIMETERS"

    name = opts.get("name") or os.path.splitext(os.path.basename(opts["in"]))[0]
    vertices, faces = read_stl(opts["in"])
    obj = bpy.data.objects.new(name, mesh_from_arrays(name, vertices, faces))
    obj[PROP_PART] = True
    scene.collection.objects.link(obj)
    bpy.context.view_layer.objects.active = obj
    obj.select_set(True)

    scene[PROP_OUT] = opts["out"]
    scene[PROP_NAME] = name
    if opts.get("exe"):
        scene[PROP_EXE] = opts["exe"]
        _remember_session_exe(opts["exe"])

    # Save next to the exchange files, so Ctrl+S has somewhere to go without asking.
    # This save fires save_post too. Sending the untouched part back from it would make EdgeSlicer swap
    # in an identical mesh and drop the part's painted supports, seams and colours.
    blend = os.path.join(os.path.dirname(opts["out"]), _safe_filename(name) + ".blend")
    _suppress_send += 1
    try:
        bpy.ops.wm.save_as_mainfile(filepath=blend)
    except Exception as ex:
        print("EdgeSlicer Bridge: could not save %s: %s" % (blend, ex))
    finally:
        _suppress_send -= 1

    if not bpy.app.background:
        bpy.app.timers.register(_frame_part, first_interval=0.2)


def session_objects(scene):
    parts = [o for o in scene.objects if o.type == "MESH" and o.get(PROP_PART)]
    if parts:
        return parts
    # The original was deleted or joined into something else: send every visible mesh.
    return [o for o in scene.objects if o.type == "MESH" and o.visible_get()]


def send_back(scene):
    out = scene.get(PROP_OUT)
    if not out:
        return None, "This file was not opened from EdgeSlicer"
    objects = session_objects(scene)
    vertices, faces = objects_to_arrays(objects)
    if len(faces) == 0:
        return None, "There is no mesh to send back"
    if not os.path.isdir(os.path.dirname(out)):
        return None, "EdgeSlicer is no longer waiting for this part"
    write_stl(out, vertices, faces)
    scene[PROP_SENT] = time.strftime("%H:%M:%S")
    return len(faces), None


@persistent
def _on_save_post(*_args):
    if _suppress_send:
        return
    scene = bpy.context.scene
    if scene is None or not scene.get(PROP_OUT):
        return
    count, error = send_back(scene)
    if error:
        print("EdgeSlicer Bridge: " + error)


# ------------------------------------------------------------------------------------------------
# Operators, panel, preferences
# ------------------------------------------------------------------------------------------------

class EDGESLICER_OT_send_selected(bpy.types.Operator):
    bl_idname = "edgeslicer.send_selected"
    bl_label = "Send to EdgeSlicer"
    bl_description = "Send the selected meshes to EdgeSlicer as separate objects"

    @classmethod
    def poll(cls, context):
        return any(o.type == "MESH" for o in context.selected_objects)

    def execute(self, context):
        exe = find_edgeslicer(context.scene)
        if not exe:
            self.report({"ERROR"}, "EdgeSlicer was not found. Set its location in the EdgeSlicer Bridge add-on preferences.")
            return {"CANCELLED"}
        folder = _send_dir()
        scale = _unit_scale(context.scene)
        files, used = [], set()
        for obj in [o for o in context.selected_objects if o.type == "MESH"]:
            vertices, faces = objects_to_arrays([obj], scale)
            if len(faces) == 0:
                continue
            base = _safe_filename(obj.name)
            stem, n = base, 2
            while stem.lower() in used:
                stem, n = "%s (%d)" % (base, n), n + 1
            used.add(stem.lower())
            path = os.path.join(folder, stem + ".stl")
            write_stl(path, vertices, faces)
            files.append(path)
        if not files:
            self.report({"ERROR"}, "The selected objects have no faces to send")
            return {"CANCELLED"}
        try:
            launch_edgeslicer(exe, files)
        except OSError as ex:
            self.report({"ERROR"}, "Could not start EdgeSlicer: %s" % ex)
            return {"CANCELLED"}
        self.report({"INFO"}, "Sent %d object%s to EdgeSlicer" % (len(files), "" if len(files) == 1 else "s"))
        return {"FINISHED"}


class EDGESLICER_OT_send_back(bpy.types.Operator):
    bl_idname = "edgeslicer.send_back"
    bl_label = "Send back to EdgeSlicer"
    bl_description = "Replace the part in EdgeSlicer with this mesh"

    @classmethod
    def poll(cls, context):
        return bool(context.scene and context.scene.get(PROP_OUT))

    def execute(self, context):
        count, error = send_back(context.scene)
        if error:
            self.report({"ERROR"}, error)
            return {"CANCELLED"}
        self.report({"INFO"}, "Sent %d triangles back to EdgeSlicer" % count)
        return {"FINISHED"}


def _finish_install(script_path, exe):
    # Runs from a timer, after the operator that asked for it has returned: the classes this script
    # registered for the session are swapped for the installed add-on's own.
    try:
        unregister()
    except Exception:
        pass
    try:
        bpy.ops.preferences.addon_install(filepath=script_path, overwrite=True)
        bpy.ops.preferences.addon_enable(module=ADDON_ID)
        prefs = _prefs()
        if prefs is not None and exe and getattr(prefs, "edgeslicer_last_exe", None) is not None:
            prefs.edgeslicer_last_exe = exe
        bpy.ops.wm.save_userpref()
        print("EdgeSlicer Bridge: add-on installed")
    except Exception as ex:
        print("EdgeSlicer Bridge: add-on install failed: %s" % ex)
        register()
    return None


class EDGESLICER_OT_install_addon(bpy.types.Operator):
    bl_idname = "edgeslicer.install_addon"
    bl_label = "Install Send to EdgeSlicer"
    bl_description = "Install this bridge as a Blender add-on, so Send to EdgeSlicer is always available"

    def execute(self, context):
        path = os.path.abspath(__file__)
        exe = context.scene.get(PROP_EXE, "")
        bpy.app.timers.register(lambda: _finish_install(path, exe), first_interval=0.1)
        self.report({"INFO"}, "Installing the EdgeSlicer Bridge add-on")
        return {"FINISHED"}


class EDGESLICER_PT_panel(bpy.types.Panel):
    bl_label = "EdgeSlicer"
    bl_idname = "EDGESLICER_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "EdgeSlicer"

    def draw(self, context):
        layout = self.layout
        scene = context.scene
        if scene.get(PROP_OUT):
            box = layout.box()
            box.label(text="Editing %s" % scene.get(PROP_NAME, "part"), icon="LINKED")
            box.operator(EDGESLICER_OT_send_back.bl_idname, icon="EXPORT")
            box.label(text="Saving (Ctrl+S) also sends it back.")
            if scene.get(PROP_SENT):
                box.label(text="Last sent at %s" % scene[PROP_SENT])
        layout.operator(EDGESLICER_OT_send_selected.bl_idname, icon="EXPORT")
        if ADDON_ID not in context.preferences.addons:
            layout.separator()
            layout.label(text="Keep Send to EdgeSlicer in every file:")
            layout.operator(EDGESLICER_OT_install_addon.bl_idname, icon="PLUGIN")


def _menu_send(self, context):
    self.layout.operator(EDGESLICER_OT_send_selected.bl_idname, text="Send to EdgeSlicer")


def _menu_export(self, context):
    self.layout.operator(EDGESLICER_OT_send_selected.bl_idname, text="Send to EdgeSlicer (.stl)")


_classes = [EDGESLICER_OT_send_selected, EDGESLICER_OT_send_back, EDGESLICER_OT_install_addon, EDGESLICER_PT_panel]

# Preferences only exist when loaded as an add-on; bl_idname must be the module name.
if __name__ != "__main__":
    class EDGESLICER_AddonPreferences(bpy.types.AddonPreferences):
        bl_idname = __name__

        edgeslicer_path: bpy.props.StringProperty(
            name="EdgeSlicer location",
            description="EdgeSlicer.exe, EdgeSlicer.app or the EdgeSlicer executable. Leave empty to look in the usual install location",
            subtype="FILE_PATH",
        )
        edgeslicer_last_exe: bpy.props.StringProperty(
            name="Last EdgeSlicer",
            description="The EdgeSlicer that most recently opened a part in Blender. Used when the current file is not an edit session and the location above is empty",
            subtype="FILE_PATH",
        )
        use_scene_units: bpy.props.BoolProperty(
            name="Use scene units",
            description="Convert from the scene's unit scale to millimetres. Off sends one Blender unit as one millimetre, like Blender's STL export",
            default=False,
        )

        def draw(self, context):
            self.layout.prop(self, "edgeslicer_path")
            if self.edgeslicer_last_exe:
                self.layout.label(text="Last EdgeSlicer used: %s" % self.edgeslicer_last_exe)
            self.layout.prop(self, "use_scene_units")

    _classes.insert(0, EDGESLICER_AddonPreferences)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.VIEW3D_MT_object.append(_menu_send)
    bpy.types.TOPBAR_MT_file_export.append(_menu_export)
    if _on_save_post not in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.append(_on_save_post)


def unregister():
    if _on_save_post in bpy.app.handlers.save_post:
        bpy.app.handlers.save_post.remove(_on_save_post)
    bpy.types.TOPBAR_MT_file_export.remove(_menu_export)
    bpy.types.VIEW3D_MT_object.remove(_menu_send)
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)


def _run_as_script():
    opts = parse_session_args(sys.argv)
    installed = sys.modules.get(ADDON_ID) if ADDON_ID in bpy.context.preferences.addons else None
    if installed is not None and hasattr(installed, "start_edit_session"):
        # The add-on is installed: let it own the session instead of registering a second copy.
        if opts:
            installed_version = tuple(getattr(installed, "bl_info", {}).get("version", (0, 0, 0)))
            if installed_version < tuple(bl_info["version"]) and opts.get("exe"):
                # An older installed copy prefers the location in the preferences over the EdgeSlicer
                # that opened this session, so a send could reach a different install. Point it at this
                # one for now; reinstalling the add-on from this session replaces the old copy.
                prefs = _prefs()
                if prefs is not None and getattr(prefs, "edgeslicer_path", None) is not None:
                    _log("the installed add-on is older (%s); using %s for this session" % (installed_version, opts["exe"]))
                    prefs.edgeslicer_path = opts["exe"]
            # Copies older than 1.0.2 send the untouched part back when the session saves its .blend
            # (see start_edit_session); keep their save handler out of that first save.
            old_handler = None if hasattr(installed, "_suppress_send") else getattr(installed, "_on_save_post", None)
            handlers = bpy.app.handlers.save_post
            muted = old_handler is not None and old_handler in handlers
            if muted:
                handlers.remove(old_handler)
            try:
                installed.start_edit_session(opts)
            finally:
                if muted:
                    handlers.append(old_handler)
        return
    register()
    if opts:
        start_edit_session(opts)


if __name__ == "__main__":
    _run_as_script()
