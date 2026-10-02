# EdgeSlicer Bridge add-on for FreeCAD: registers "Send to EdgeSlicer", "Update EdgeSlicer",
# "Mesh to solid" and "EdgeSlicer location" and puts them on an "EdgeSlicer" toolbar shown in every
# workbench. Installed by EdgeSlicer (Preferences > Install FreeCAD add-on); the commands live in
# edgeslicer_bridge.py next to this file.


def _edgeslicer_bridge_setup():
    import FreeCAD
    try:
        import edgeslicer_bridge
        edgeslicer_bridge.setup_installed()
    except Exception as ex:
        FreeCAD.Console.PrintError("EdgeSlicer Bridge: could not start the add-on: %s\n" % ex)


_edgeslicer_bridge_setup()
