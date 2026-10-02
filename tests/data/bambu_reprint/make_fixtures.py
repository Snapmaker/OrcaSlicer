#!/usr/bin/env python3
"""Writes the small, made-up sliced Bambu jobs the Bambu reprint tests read (tests/slic3rutils/
bambu_reprint_tests.cpp and the fake-printer gate, tests/phone/test_bambu_reprint_gate.py).

Each file is a .gcode.3mf with only what a send reads: _rels, a 3D model with no objects,
Metadata/model_settings.config (which plate carries G-code), Metadata/slice_info.config (the plate's
model, nozzles, filaments and their nozzle groups), Metadata/project_settings.config (filament ids,
physical_extruder_map, filament_map, bed type) and Metadata/filament_sequence.json - the same parts,
spelled the same way, as a file the desktop exports for a send. The G-code itself is a stub: nothing
here is ever printed.

  python make_fixtures.py            (writes next to this script)
"""
import io
import json
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

RELS = """<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel-1" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>
"""
CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
 <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
 <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>
 <Default Extension="gcode" ContentType="text/x.gcode"/>
</Types>
"""
MODEL = """<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:BambuStudio="http://schemas.bambulab.com/package/2021" xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p">
 <metadata name="Application">EdgeSlicer-2.4.0.2</metadata>
 <metadata name="BambuStudio:3mfVersion">1</metadata>
 <resources>
 </resources>
 <build/>
</model>
"""


def model_settings(plates, gcode_plate, filament_maps):
    out = ['<?xml version="1.0" encoding="UTF-8"?>', "<config>"]
    for n in range(1, plates + 1):
        out.append("  <plate>")
        out.append('    <metadata key="plater_id" value="%d"/>' % n)
        out.append('    <metadata key="plater_name" value=""/>')
        out.append('    <metadata key="locked" value="false"/>')
        if filament_maps:
            out.append('    <metadata key="filament_map_mode" value="Manual"/>')
            out.append('    <metadata key="filament_maps" value="%s"/>' % " ".join(str(v) for v in filament_maps))
        out.append('    <metadata key="gcode_file" value="%s"/>' % ("Metadata/plate_%d.gcode" % n if n == gcode_plate else ""))
        out.append("  </plate>")
    out.append("</config>")
    return "\n".join(out) + "\n"


def slice_info(plate, model_id, diameters, filament_maps, filaments, nozzles, warnings=()):
    out = ['<?xml version="1.0" encoding="UTF-8"?>', "<config>", "  <header>",
           '    <header_item key="X-BBL-Client-Type" value="slicer"/>',
           '    <header_item key="X-BBL-Client-Version" value="02.04.00.02"/>', "  </header>", "  <plate>"]
    meta = [("index", plate), ("extruder_type", " ".join("0" for _ in diameters)),
            ("nozzle_volume_type", " ".join("0" for _ in diameters)), ("printer_model_id", model_id),
            ("nozzle_diameters", ",".join(diameters)), ("timelapse_type", 0), ("prediction", 3600),
            ("weight", "%.2f" % sum(f["g"] for f in filaments)), ("outside", "false"), ("support_used", "false"),
            ("label_object_enabled", "true")]
    if filament_maps:
        meta += [("enable_filament_dynamic_map", "false"), ("has_filament_switcher", "false"),
                 ("filament_maps", " ".join(str(v) for v in filament_maps))]
    for k, v in meta:
        out.append('    <metadata key="%s" value="%s"/>' % (k, v))
    for f in filaments:
        out.append('    <filament id="%d" tray_info_idx="%s" type="%s" color="%s" used_m="%.2f" used_g="%.2f" group_id="%s" '
                   'nozzle_diameter="0.40" volume_type="%s" used_for_object="true" used_for_support="false" '
                   'total_load_time="0.00" total_unload_time="0.00"/>'
                   % (f["id"], f["tid"], f["type"], f["color"], f["g"] / 3.0, f["g"], f.get("group", ""), f.get("vol", "Standard")))
    for w in warnings:
        out.append('    <warning msg="%s" level="1" error_code ="1000C001"  />' % w)
    for nz in nozzles:
        out.append('    <nozzle id="%d" extruder_id="%d" nozzle_diameter="%s" volume_type="%s"/>' % nz)
    out += ["  </plate>", "</config>"]
    return "\n".join(out) + "\n"


def write(name, *, model_id, printer_model, plate, plates, diameters, volumes, pem, filament_maps, colours, types, ids,
          filaments, nozzles, sequence, bed="Textured PEI Plate", warnings=()):
    settings = {
        "name": "project_settings",
        "from": "project",
        "printer_model": printer_model,
        "printer_settings_id": printer_model + " " + diameters[0] + " nozzle",
        "nozzle_diameter": diameters,
        "nozzle_volume_type": volumes,
        "physical_extruder_map": [str(v) for v in pem],
        "filament_colour": colours,
        "filament_type": types,
        "filament_ids": ids,
        "curr_bed_type": bed,
    }
    if filament_maps:
        settings["filament_map"] = [str(v) for v in filament_maps]
    seq = {"plate_%d" % n: {"nozzle_sequence": [], "optimal_assignment": [], "sequence": sequence if n == plate else []}
           for n in range(1, plates + 1)}
    gcode = "; HEADER_BLOCK_START\n; generated by EdgeSlicer 2.4.0.2 (made-up reprint fixture)\n; HEADER_BLOCK_END\nG28\n"
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("[Content_Types].xml", CONTENT_TYPES)
        z.writestr("_rels/.rels", RELS)
        z.writestr("3D/3dmodel.model", MODEL)
        z.writestr("Metadata/model_settings.config", model_settings(plates, plate, filament_maps))
        z.writestr("Metadata/project_settings.config", json.dumps(settings, indent=4))
        z.writestr("Metadata/slice_info.config", slice_info(plate, model_id, diameters, filament_maps, filaments, nozzles, warnings))
        z.writestr("Metadata/filament_sequence.json", json.dumps(seq))
        z.writestr("Metadata/plate_%d.gcode" % plate, gcode)
    with open(os.path.join(HERE, name), "wb") as f:
        f.write(buf.getvalue())
    print("wrote", name, len(buf.getvalue()), "bytes")


def main():
    # An X1 Carbon job: one nozzle, two PLA colours, plate 1 of 1.
    write("x1c_two_colour.gcode.3mf", model_id="BL-P001", printer_model="Bambu Lab X1 Carbon", plate=1, plates=1,
          diameters=["0.4"], volumes=["Standard"], pem=[0], filament_maps=None,
          colours=["#FFFFFF", "#000000"], types=["PLA", "PLA"], ids=["GFA00", "GFA00"],
          filaments=[{"id": 1, "tid": "GFA00", "type": "PLA", "color": "#FFFFFF", "g": 12.5},
                     {"id": 2, "tid": "GFA00", "type": "PLA", "color": "#000000", "g": 3.25}],
          nozzles=[], sequence=[1, 2, 1, 2])
    # An H2D job, plate 2 of 3: filament 1 (white PLA) on the left, filaments 2 (black PETG) and 4
    # (red PLA) on the right, filament 3 unused. physical_extruder_map [1, 0]: the right extruder is
    # the main (physical 0) one.
    write("h2d_two_sided.gcode.3mf", model_id="O1D", printer_model="Bambu Lab H2D", plate=2, plates=3,
          diameters=["0.4", "0.4"], volumes=["Standard", "High Flow"], pem=[1, 0], filament_maps=[1, 2, 1, 2],
          colours=["#FFFFFF", "#000000", "#00AE42", "#C12E1F"], types=["PLA", "PETG", "PLA", "PLA"],
          ids=["GFA00", "GFG00", "GFA00", "GFA00"],
          filaments=[{"id": 1, "tid": "GFA00", "type": "PLA", "color": "#FFFFFF", "g": 20.0, "group": "0"},
                     {"id": 2, "tid": "GFG00", "type": "PETG", "color": "#000000", "g": 8.0, "group": "1", "vol": "High Flow"},
                     {"id": 4, "tid": "GFA00", "type": "PLA", "color": "#C12E1F", "g": 4.0, "group": "1", "vol": "High Flow"}],
          nozzles=[(0, 1, "0.4", "Standard"), (1, 2, "0.4", "High Flow")], sequence=[1, 2, 4, 1])
    # An H2C job (nozzle rack on the right): filament 1 on the left, 2 and 3 on the right (rack).
    write("h2c_rack.gcode.3mf", model_id="O1C2", printer_model="Bambu Lab H2C", plate=1, plates=1,
          diameters=["0.4", "0.4"], volumes=["Standard", "Standard"], pem=[1, 0], filament_maps=[1, 2, 2],
          colours=["#F7D959", "#042F56", "#5D989E"], types=["PLA", "PLA", "PLA"], ids=["GFA00", "GFA00", "GFA00"],
          filaments=[{"id": 1, "tid": "GFA00", "type": "PLA", "color": "#F7D959", "g": 6.0, "group": "0"},
                     {"id": 2, "tid": "GFA00", "type": "PLA", "color": "#042F56", "g": 30.0, "group": "1"},
                     {"id": 3, "tid": "GFA00", "type": "PLA", "color": "#5D989E", "g": 120.0, "group": "1"}],
          nozzles=[(0, 1, "0.4", "Standard"), (1, 2, "0.4", "Standard")], sequence=[1, 2, 3],
          warnings=("not_generate_timelapse",))


if __name__ == "__main__":
    main()
