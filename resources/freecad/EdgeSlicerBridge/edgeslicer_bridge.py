# EdgeSlicer Bridge for FreeCAD (0.21 and 1.x).
#
# One module, two uses:
#
#  * Installed as a FreeCAD add-on (EdgeSlicer: Preferences > Install FreeCAD add-on copies this
#    folder into FreeCAD's user Mod folder), InitGui.py registers the commands and an "EdgeSlicer"
#    toolbar that is shown in every workbench:
#      - Send to EdgeSlicer: the selected bodies (or every visible one) go to EdgeSlicer as STEP
#        files (meshes as STL), which adds them to the open project or starts EdgeSlicer.
#      - Update EdgeSlicer: sends a part opened from EdgeSlicer back to it.
#      - Mesh to solid: turns a mesh opened from EdgeSlicer into a refined solid to model on.
#      - EdgeSlicer location: which EdgeSlicer "Send to EdgeSlicer" starts.
#
#  * Run by EdgeSlicer's "Edit in FreeCAD" through the per-session macro edgeslicer_edit.FCMacro,
#    start_edit_session() opens one part. Saving the document (Ctrl+S) or pressing "Update
#    EdgeSlicer" writes the visible result to the file EdgeSlicer is watching, and EdgeSlicer swaps
#    it into the part, keeping the part's placement and settings.
#
# Exchange formats: the part arrives as STEP when it is an unedited STEP import in EdgeSlicer
# (exact B-rep) and as STL otherwise. It goes back as STEP when every visible result is a solid or
# shape (EdgeSlicer tessellates it like a normal STEP import) and as STL when a result is still a
# mesh. Coordinates are the part's own, in millimetres; nothing is moved or centred here.

import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

import FreeCAD as App

if App.GuiUp:
    import FreeCADGui as Gui
else:  # FreeCADCmd: FreeCADGui imports there too, but has no GUI behind it
    Gui = None

VERSION = (1, 0, 0)
MODULE = "edgeslicer_bridge"

# Document metadata (doc.Meta, saved in the .FCStd) that marks a document as an EdgeSlicer edit
# session. Reopening that file later still sends back to the same part while EdgeSlicer watches it.
META_OUT_STEP = "EdgeSlicerOutStep"
META_OUT_STL = "EdgeSlicerOutStl"
META_EXE = "EdgeSlicerExe"
META_NAME = "EdgeSlicerName"

PARAMS = "User parameter:BaseApp/Preferences/Mod/EdgeSlicerBridge"
LOCATION_FILE = "edgeslicer_location.txt"

COMMANDS = ["EdgeSlicer_Send", "EdgeSlicer_Update", "EdgeSlicer_MeshToSolid", "EdgeSlicer_Location"]

# Set while start_edit_session() saves the new document, so that first save does not send the
# unchanged part straight back (which would only re-tessellate it and drop its paint).
_suppress_send = 0


def _log(message):
    App.Console.PrintMessage("EdgeSlicer Bridge: %s\n" % message)


def _error(message):
    App.Console.PrintError("EdgeSlicer Bridge: %s\n" % message)


def _status(message):
    if Gui is None:
        return
    try:
        Gui.getMainWindow().statusBar().showMessage("EdgeSlicer: " + message, 8000)
    except Exception:
        pass


def _message_box(title, text, error=False):
    if Gui is None:
        (_error if error else _log)(text)
        return
    try:
        from PySide import QtGui
        box = QtGui.QMessageBox.critical if error else QtGui.QMessageBox.information
        box(Gui.getMainWindow(), title, text)
    except Exception:
        (_error if error else _log)(text)


def _bridge():
    # The commands look the module up when they run, so a newer copy loaded by an edit session
    # (see edgeslicer_edit.FCMacro) takes over from an older installed one.
    return sys.modules.get(MODULE) or sys.modules[__name__]


def _icon(name):
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "Resources", "icons", name)


# ------------------------------------------------------------------------------------------------
# Writing shapes and meshes
# ------------------------------------------------------------------------------------------------

def _safe_filename(name):
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", name).strip(" .")
    return name or "part"


def _global_placement(obj):
    try:
        return obj.getGlobalPlacement()
    except Exception:
        return obj.Placement


def _is_mesh(obj):
    return obj.isDerivedFrom("Mesh::Feature")


def _has_faces(shape):
    try:
        return shape is not None and not shape.isNull() and len(shape.Faces) > 0
    except Exception:
        return False


def _body_of(obj):
    try:
        parent = obj.getParentGeoFeatureGroup()
    except Exception:
        return None
    return parent if parent is not None and parent.TypeId == "PartDesign::Body" else None


def _exportable(obj):
    """A body, a solid or shape with faces, or a mesh. Sketches, datums and containers are not."""
    if obj.TypeId == "PartDesign::Body":
        return _has_faces(obj.Shape)
    if _is_mesh(obj):
        return obj.Mesh.CountFacets > 0
    if obj.isDerivedFrom("Part::Datum") or not obj.isDerivedFrom("Part::Feature"):
        return False
    if _body_of(obj) is not None:
        return False  # the body stands for its features
    return _has_faces(obj.Shape)


def result_objects(doc):
    """What a document's model is: every visible body, solid, shape or mesh."""
    return [o for o in doc.Objects if getattr(o, "Visibility", False) and _exportable(o)]


def placed_shape(obj):
    shape = obj.Shape.copy()
    shape.Placement = _global_placement(obj)
    return shape


def write_step(path, objects):
    import Part
    compound = Part.makeCompound([placed_shape(o) for o in objects])
    tmp = path + ".sending.step"
    compound.exportStep(tmp)
    os.replace(tmp, path)


def _mesh_triangles(obj):
    """Triangles of a mesh or a shape in global coordinates: (points, facets)."""
    if _is_mesh(obj):
        mesh = obj.Mesh.copy()
        mesh.Placement = _global_placement(obj)
        return mesh.Topology
    shape = placed_shape(obj)
    box = shape.BoundBox
    tolerance = max(0.005, 2e-4 * box.DiagonalLength) if box.isValid() else 0.01
    return shape.tessellate(tolerance)


def write_stl(path, objects):
    """Binary STL written atomically, so EdgeSlicer never reads a half-written file."""
    records = []
    for obj in objects:
        points, facets = _mesh_triangles(obj)
        for facet in facets:
            a, b, c = (points[i] for i in facet)
            n = (b - a).cross(c - a)
            length = n.Length
            if length > 0:
                n = n * (1.0 / length)
            records.append(struct.pack("<12fH", n.x, n.y, n.z, a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z, 0))
    tmp = path + ".sending.stl"
    with open(tmp, "wb") as f:
        f.write(b"EdgeSlicer Bridge (FreeCAD)".ljust(80, b" "))
        f.write(struct.pack("<I", len(records)))
        f.write(b"".join(records))
    os.replace(tmp, path)
    return len(records)


# ------------------------------------------------------------------------------------------------
# Finding and starting EdgeSlicer
# ------------------------------------------------------------------------------------------------

def _params():
    return App.ParamGet(PARAMS)


def _installed_location():
    # Written by EdgeSlicer's "Install FreeCAD add-on": the install that put this add-on here.
    try:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), LOCATION_FILE), encoding="utf-8") as f:
            return f.read().strip()
    except OSError:
        return ""


def default_edgeslicer_paths():
    if sys.platform == "win32":
        roots = [os.environ.get("ProgramW6432"), os.environ.get("ProgramFiles"), r"C:\Program Files"]
        return [os.path.join(r, "EdgeSlicer", "EdgeSlicer.exe") for r in roots if r]
    if sys.platform == "darwin":
        return ["/Applications/EdgeSlicer.app", os.path.expanduser("~/Applications/EdgeSlicer.app")]
    found = [shutil.which(n) for n in ("EdgeSlicer", "edgeslicer")]
    return [p for p in found if p]


def find_edgeslicer(doc=None):
    # Precedence: the EdgeSlicer that started this edit session (saved in the document), the
    # location chosen with "EdgeSlicer location", the EdgeSlicer that installed this add-on, the
    # EdgeSlicer that started the last edit session, then the usual install location. A hand-off
    # only reaches an EdgeSlicer of the very same executable, so the one the user is working in
    # must come first.
    candidates = []
    if doc is not None and doc.Meta.get(META_EXE):
        candidates.append(("the edit session", doc.Meta[META_EXE]))
    params = _params()
    if params.GetString("EdgeSlicerPath", ""):
        candidates.append(("the EdgeSlicer location setting", params.GetString("EdgeSlicerPath", "")))
    if _installed_location():
        candidates.append(("the EdgeSlicer that installed this add-on", _installed_location()))
    if params.GetString("LastEdgeSlicer", ""):
        candidates.append(("the last edit session", params.GetString("LastEdgeSlicer", "")))
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
    # --single-instance hands the files to an EdgeSlicer that is already open instead of starting
    # a second window. On macOS an .app bundle goes through `open`, which does the same thing.
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


def _send_dir():
    base = os.path.join(tempfile.gettempdir(), "EdgeSlicer-from-FreeCAD")
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


def objects_to_send(doc):
    """The selection (a feature stands for its body), or every visible result when nothing is."""
    chosen = []
    if Gui is not None:
        for obj in Gui.Selection.getSelection(doc.Name):
            obj = _body_of(obj) or obj
            if obj not in chosen and _exportable(obj):
                chosen.append(obj)
    return chosen or result_objects(doc)


def send_to_edgeslicer(doc):
    """Returns (number of objects sent, error message or None)."""
    objects = objects_to_send(doc)
    if not objects:
        return 0, "There is no body, solid or mesh to send"
    exe = find_edgeslicer(doc)
    if not exe:
        return 0, "EdgeSlicer was not found. Choose it with \"EdgeSlicer location\" on the EdgeSlicer toolbar."
    folder = _send_dir()
    files, used = [], set()
    for obj in objects:
        base = _safe_filename(obj.Label)
        stem, n = base, 2
        while stem.lower() in used:
            stem, n = "%s (%d)" % (base, n), n + 1
        used.add(stem.lower())
        if _is_mesh(obj):
            path = os.path.join(folder, stem + ".stl")
            write_stl(path, [obj])
        else:
            path = os.path.join(folder, stem + ".step")
            write_step(path, [obj])
        files.append(path)
    launch_edgeslicer(exe, files)
    return len(files), None


# ------------------------------------------------------------------------------------------------
# Edit session started by EdgeSlicer
# ------------------------------------------------------------------------------------------------

def is_session(doc):
    return doc is not None and bool(doc.Meta.get(META_OUT_STEP))


def send_back(doc):
    """Writes the document's visible result where EdgeSlicer is watching. Returns (format, error)."""
    meta = doc.Meta
    out_step, out_stl = meta.get(META_OUT_STEP), meta.get(META_OUT_STL)
    if not out_step:
        return None, "This document was not opened from EdgeSlicer"
    if not os.path.isdir(os.path.dirname(out_step)):
        return None, "EdgeSlicer is no longer waiting for this part"
    doc.recompute()
    objects = result_objects(doc)
    if not objects:
        return None, "There is no visible body, solid or mesh to send back"
    if any(_is_mesh(o) for o in objects):
        write_stl(out_stl, objects)
        return "STL", None
    write_step(out_step, objects)
    return "STEP", None


def _find_session_document(out_step):
    for doc in App.listDocuments().values():
        if doc.Meta.get(META_OUT_STEP) == out_step:
            return doc
    return None


def _new_session_document(session):
    name = session.get("name") or "part"
    doc = App.newDocument("EdgeSlicer")
    doc.Label = name
    path, fmt = session["input"], session.get("input_format", "stl")
    if fmt == "step":
        import Part
        shape = Part.Shape()
        shape.read(path)
        obj = doc.addObject("Part::Feature", "Part")
        obj.Shape = shape
    else:
        import Mesh
        obj = doc.addObject("Mesh::Feature", "Mesh")
        obj.Mesh = Mesh.Mesh(path)
    obj.Label = name
    meta = dict(doc.Meta)
    meta.update({
        META_OUT_STEP: session["output_step"],
        META_OUT_STL: session["output_stl"],
        META_EXE: session.get("edgeslicer_exe", ""),
        META_NAME: name,
    })
    doc.Meta = meta
    doc.recompute()
    # Save next to the exchange files, so Ctrl+S has somewhere to go without asking.
    global _suppress_send
    _suppress_send += 1
    try:
        doc.saveAs(session["document"])
    except Exception as ex:
        _error("could not save %s: %s" % (session["document"], ex))
    finally:
        _suppress_send -= 1
    if fmt != "step":
        _log("\"%s\" is a mesh. Use \"Mesh to solid\" on the EdgeSlicer toolbar to model on it as a solid." % name)
    return doc


def _fit_view():
    try:
        view = Gui.activeDocument().activeView()
        view.viewIsometric()
        view.fitAll()
    except Exception:
        pass


def start_edit_session(session_json):
    with open(session_json, encoding="utf-8") as f:
        session = json.load(f)
    ensure_observer()
    if Gui is not None:
        ensure_commands()
        ensure_session_toolbar()
    if session.get("edgeslicer_exe"):
        _params().SetString("LastEdgeSlicer", session["edgeslicer_exe"])

    doc = _find_session_document(session["output_step"])
    if doc is None and os.path.exists(session["document"]):
        doc = App.openDocument(session["document"])
    if doc is None:
        doc = _new_session_document(session)
    App.setActiveDocument(doc.Name)
    if Gui is not None:
        try:
            Gui.getDocument(doc.Name).activeView()
            Gui.activateView("Gui::View3DInventor", True)
        except Exception:
            pass
        try:
            from PySide import QtCore
            QtCore.QTimer.singleShot(300, _fit_view)
        except Exception:
            _fit_view()
        _status("editing \"%s\". Save (Ctrl+S) or press Update EdgeSlicer to send it back." % session.get("name", "part"))
    _log("editing \"%s\" for EdgeSlicer (%s)" % (session.get("name", "part"), session_json))
    return doc


class _SaveObserver:
    def slotFinishSaveDocument(self, doc, filename):
        if _suppress_send or not is_session(doc):
            return
        try:
            fmt, error = _bridge().send_back(doc)
        except Exception as ex:
            fmt, error = None, str(ex)
        if error:
            _error(error)
            _status(error)
        else:
            _log("sent \"%s\" back to EdgeSlicer as %s" % (doc.Meta.get(META_NAME, doc.Label), fmt))
            _status("sent back to EdgeSlicer as %s" % fmt)


def ensure_observer():
    # One observer for the whole FreeCAD session, whichever copy of this module registered it.
    old = getattr(App, "_edgeslicer_bridge_observer", None)
    if old is not None:
        try:
            App.removeDocumentObserver(old)
        except Exception:
            pass
    observer = _SaveObserver()
    App.addDocumentObserver(observer)
    App._edgeslicer_bridge_observer = observer


# ------------------------------------------------------------------------------------------------
# Mesh to solid
# ------------------------------------------------------------------------------------------------

def mesh_to_solid(obj, tolerance=0.05):
    """A refined solid (or shell, when the mesh is open) next to `obj`; the mesh is hidden."""
    import Part
    # Built in the mesh's own coordinates; the new feature takes over the mesh's placement.
    local = obj.Mesh.copy()
    local.Placement = App.Placement()
    shape = Part.Shape()
    shape.makeShapeFromMesh(local.Topology, tolerance)
    result = shape
    try:
        solid = Part.Solid(Part.Shell(shape.Faces))
        if solid.isValid() and solid.Volume > 0:
            result = solid
    except Exception:
        pass
    try:
        result = result.removeSplitter()
    except Exception:
        pass
    doc = obj.Document
    feature = doc.addObject("Part::Feature", "Solid")
    feature.Shape = result
    feature.Label = obj.Label + " (solid)"
    feature.Placement = obj.Placement
    obj.Visibility = False
    doc.recompute()
    return feature


# ------------------------------------------------------------------------------------------------
# Commands and toolbar
# ------------------------------------------------------------------------------------------------

class _SendCommand:
    def GetResources(self):
        return {"Pixmap": _icon("EdgeSlicerSend.svg"), "MenuText": "Send to EdgeSlicer",
                "ToolTip": "Send the selected bodies (or every visible one) to EdgeSlicer as STEP files"}

    def IsActive(self):
        return App.ActiveDocument is not None

    def Activated(self):
        count, error = _bridge().send_to_edgeslicer(App.ActiveDocument)
        if error:
            _message_box("Send to EdgeSlicer", error, True)
        else:
            _status("sent %d object%s to EdgeSlicer" % (count, "" if count == 1 else "s"))


class _UpdateCommand:
    def GetResources(self):
        return {"Pixmap": _icon("EdgeSlicerUpdate.svg"), "MenuText": "Update EdgeSlicer",
                "ToolTip": "Replace the part in EdgeSlicer with this document's visible result (saving does the same)"}

    def IsActive(self):
        return is_session(App.ActiveDocument)

    def Activated(self):
        fmt, error = _bridge().send_back(App.ActiveDocument)
        if error:
            _message_box("Update EdgeSlicer", error, True)
        else:
            _status("sent back to EdgeSlicer as %s" % fmt)


class _MeshToSolidCommand:
    def GetResources(self):
        return {"Pixmap": _icon("EdgeSlicerMeshToSolid.svg"), "MenuText": "Mesh to solid",
                "ToolTip": "Turn the selected mesh (or the mesh opened from EdgeSlicer) into a refined solid to model on"}

    def _meshes(self):
        doc = App.ActiveDocument
        if doc is None:
            return []
        selected = [o for o in Gui.Selection.getSelection(doc.Name) if _is_mesh(o)]
        return selected or [o for o in result_objects(doc) if _is_mesh(o)]

    def IsActive(self):
        return bool(self._meshes())

    def Activated(self):
        for obj in self._meshes():
            try:
                _bridge().mesh_to_solid(obj)
            except Exception as ex:
                _message_box("Mesh to solid", "%s: %s" % (obj.Label, ex), True)


class _LocationCommand:
    def GetResources(self):
        return {"Pixmap": _icon("EdgeSlicerLocation.svg"), "MenuText": "EdgeSlicer location...",
                "ToolTip": "Choose the EdgeSlicer that Send to EdgeSlicer starts"}

    def IsActive(self):
        return True

    def Activated(self):
        from PySide import QtGui
        current = _params().GetString("EdgeSlicerPath", "") or (find_edgeslicer() or "")
        if sys.platform == "win32":
            pattern = "EdgeSlicer (EdgeSlicer.exe);;Programs (*.exe)"
        else:
            pattern = "All files (*)"
        result = QtGui.QFileDialog.getOpenFileName(Gui.getMainWindow(), "EdgeSlicer location", current, pattern)
        path = result[0] if isinstance(result, tuple) else result
        if path:
            _params().SetString("EdgeSlicerPath", path)
            _status("Send to EdgeSlicer uses %s" % path)


_COMMAND_CLASSES = {
    "EdgeSlicer_Send": _SendCommand,
    "EdgeSlicer_Update": _UpdateCommand,
    "EdgeSlicer_MeshToSolid": _MeshToSolidCommand,
    "EdgeSlicer_Location": _LocationCommand,
}


def ensure_commands():
    existing = set(Gui.listCommands())
    for name, cls in _COMMAND_CLASSES.items():
        if name not in existing:
            Gui.addCommand(name, cls())


def install_global_toolbar():
    """An "EdgeSlicer" toolbar in every workbench, the way FreeCAD stores custom global toolbars."""
    root = App.ParamGet("User parameter:BaseApp/Workbench/Global/Toolbar")
    group = None
    for name in root.GetGroups():
        if root.GetGroup(name).GetString("Name", "") == "EdgeSlicer":
            group = root.GetGroup(name)
            break
    if group is None:
        group = root.GetGroup("EdgeSlicerBridge")
    group.SetString("Name", "EdgeSlicer")
    group.SetBool("Active", True)
    for name in COMMANDS:
        group.SetString(name, MODULE)


_session_toolbar = None
_session_workbench = None


def setup_installed():
    """Called by InitGui.py when the add-on is installed in FreeCAD's Mod folder."""
    # Kept on the FreeCAD module, not here: an edit session may load a newer copy of this module.
    App._edgeslicer_bridge_installed = True
    ensure_commands()
    install_global_toolbar()
    ensure_observer()


def _session_toolbar_tick():
    # FreeCAD hides toolbars that are not part of a workbench when the workbench changes; bring the
    # session toolbar back then (but leave it closed if the user closed it).
    global _session_workbench
    try:
        current = Gui.activeWorkbench().name()
    except Exception:
        return
    if current != _session_workbench:
        _session_workbench = current
        if _session_toolbar is not None:
            _session_toolbar.show()


def ensure_session_toolbar():
    """Without the add-on installed, an edit session still gets the EdgeSlicer buttons."""
    global _session_toolbar
    if getattr(App, "_edgeslicer_bridge_installed", False) or _session_toolbar is not None:
        return
    try:
        from PySide import QtCore, QtGui
        mw = Gui.getMainWindow()
        bar = mw.addToolBar("EdgeSlicer")
        bar.setObjectName("EdgeSlicerSession")
        for name in COMMANDS:
            cls = _COMMAND_CLASSES[name]
            res = cls().GetResources()
            action = bar.addAction(QtGui.QIcon(res["Pixmap"]), res["MenuText"])
            action.setToolTip(res["ToolTip"])
            action.triggered.connect(lambda checked=False, n=name: Gui.runCommand(n))
        bar.show()
        _session_toolbar = bar
        timer = QtCore.QTimer(mw)
        timer.timeout.connect(_session_toolbar_tick)
        timer.start(1500)
        App._edgeslicer_bridge_toolbar_timer = timer
    except Exception as ex:
        _log("no session toolbar (%s); saving still sends the part back" % ex)
