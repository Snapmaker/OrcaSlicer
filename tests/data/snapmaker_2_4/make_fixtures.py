#!/usr/bin/env python3
"""Builds Snapmaker Orca 2.4 style configs from the presets of upstream/main.

project: the project_settings.config 2.4 composes for a U1 0.4 plate (PresetBundle::full_fff_config
of upstream/main): machine + process + filaments resolved through their inherits chains, the 46
process keys of process_flow_variant_options() as one value per declared flow type (front padded, as
Preset::normalize does there), the 19 filament keys of filament_flow_variant_options() concatenated
with filament_flow_step_size values per filament, every other filament key with one value per filament.
user presets: what Preset::save writes there for a preset that differs from its system parent
(differing keys + the *_flow_support key).
"""
import json, subprocess, sys, os

REPO = sys.argv[1]
OUT = sys.argv[2]
REF = "upstream/main"
VENDOR = "resources/profiles/Snapmaker"

FILAMENT_FLOW_KEYS = ["filament_flow_ratio", "enable_pressure_advance", "pressure_advance", "nozzle_temperature_initial_layer",
    "nozzle_temperature", "filament_max_volumetric_speed", "fan_min_speed", "fan_max_speed", "additional_cooling_fan_speed",
    "filament_retraction_length", "filament_retraction_speed", "filament_deretraction_speed", "filament_z_hop_types",
    "filament_wipe_distance", "filament_retract_length_toolchange", "filament_multitool_ramming",
    "filament_multitool_ramming_volume", "filament_multitool_ramming_flow", "filament_minimal_purge_on_wipe_tower"]
PROCESS_FLOW_KEYS = ["outer_wall_speed", "inner_wall_speed", "sparse_infill_speed", "internal_solid_infill_speed", "top_surface_speed",
    "gap_infill_speed", "bridge_speed", "internal_bridge_speed", "ironing_speed", "small_perimeter_speed", "small_perimeter_threshold",
    "support_speed", "support_interface_speed", "initial_layer_speed", "initial_layer_infill_speed", "initial_layer_travel_speed",
    "travel_speed", "slow_down_layers", "overhang_1_4_speed", "overhang_2_4_speed", "overhang_3_4_speed", "overhang_4_4_speed",
    "enable_overhang_speed", "slowdown_for_curled_perimeters", "default_acceleration", "outer_wall_acceleration",
    "inner_wall_acceleration", "top_surface_acceleration", "initial_layer_acceleration", "bridge_acceleration", "travel_acceleration",
    "sparse_infill_acceleration", "internal_solid_infill_acceleration", "accel_to_decel_enable", "accel_to_decel_factor", "default_jerk",
    "outer_wall_jerk", "inner_wall_jerk", "infill_jerk", "top_surface_jerk", "initial_layer_jerk", "travel_jerk",
    "default_junction_deviation", "max_volumetric_extrusion_rate_slope", "max_volumetric_extrusion_rate_slope_segment_length",
    "extrusion_rate_smoothing_external_perimeter_only"]
META = {"type", "name", "from", "instantiation", "setting_id", "filament_id", "inherits", "compatible_printers",
        "compatible_printers_condition", "compatible_prints", "compatible_prints_condition", "description", "renamed_from", "version"}

def show(path):
    return subprocess.run(["git", "-C", REPO, "show", f"{REF}:{path}"], capture_output=True, text=True, check=True).stdout

index = json.loads(show(f"{VENDOR}.json"))
paths = {}
for group in ("machine_list", "process_list", "filament_list", "machine_model_list"):
    for entry in index.get(group, []):
        paths[entry["name"]] = entry["sub_path"]

def resolve(name):
    preset = json.loads(show(f"{VENDOR}/{paths[name]}"))
    parent = preset.get("inherits", "")
    merged = resolve(parent) if parent else {}
    merged.update(preset)
    return merged

def as_list(value):
    return list(value) if isinstance(value, list) else [value]

def pad(values, n):
    values = as_list(values)
    return (values + [values[0]] * n)[:n]

MACHINE = "Snapmaker U1 (0.4 nozzle)"
PROCESS = "0.20mm Standard @Snapmaker U1 (0.4 nozzle)"
FILAMENTS = ["Snapmaker PLA SnapSpeed @U1", "Snapmaker PETG HF", "Snapmaker PLA Basic @U1", "Snapmaker PLA Matte @U1"]

def project():
    machine, process = resolve(MACHINE), resolve(PROCESS)
    filaments = [resolve(name) for name in FILAMENTS]
    out = {}
    for key, value in machine.items():
        if key not in META and key != "printer_flow_support":
            out[key] = value
    flows = as_list(process.get("process_flow_support", ["standard"]))
    for key, value in process.items():
        if key in META:
            continue
        out[key] = pad(value, len(flows)) if key in PROCESS_FLOW_KEYS else value
    out["printer_flow_support"] = machine["printer_flow_support"]
    steps = [len(as_list(f.get("filament_flow_support", ["standard"]))) for f in filaments]
    keys = set(filaments[0])
    for f in filaments[1:]:
        keys &= set(f)
    for key in sorted(keys - META):
        if key == "filament_flow_support" or key in FILAMENT_FLOW_KEYS:
            out[key] = [v for f, step in zip(filaments, steps) for v in pad(f.get(key, ["standard"]), step)]
        elif isinstance(filaments[0][key], list):
            out[key] = [as_list(f[key])[0] for f in filaments]
        else:
            out[key] = filaments[0][key]
    out["filament_flow_support"] = [v for f, step in zip(filaments, steps) for v in pad(f.get("filament_flow_support", ["standard"]), step)]
    out["filament_flow_step_size"] = [str(s) for s in steps]
    out["filament_settings_id"] = FILAMENTS
    out["filament_ids"] = [f.get("filament_id", "") for f in filaments]
    out["filament_colour"] = ["#FFFFFF", "#F2754E", "#2850E0", "#3CB371"]
    out["print_settings_id"] = PROCESS
    out["printer_settings_id"] = MACHINE
    out["inherits_group"] = [""] * (len(FILAMENTS) + 2)
    out["different_settings_to_system"] = [""] * (len(FILAMENTS) + 2)
    # Tool head 2 carries a High Flow nozzle and its filament was given that flow type.
    out["nozzle_volume_type"] = ["standard", "high_flow", "standard", "standard"]
    out["filament_volume_type"] = ["standard", "high_flow", "standard", "standard"]
    out["filament_grouping_mode"] = "auto"
    out["filament_map_mode"] = "Auto For Flush"
    out["filament_map"] = ["1"] * len(FILAMENTS)
    out["project_schema_version"] = "1"
    out.update({"name": "project_settings", "from": "project", "version": "2.4.0"})
    return out

def user_filament():
    parent = resolve("Snapmaker PLA SnapSpeed @U1")
    return {"type": "filament", "name": "My SnapSpeed 2.4", "from": "User", "inherits": "Snapmaker PLA SnapSpeed @U1",
            "version": "2.4.0", "filament_settings_id": ["My SnapSpeed 2.4"],
            "filament_flow_support": parent["filament_flow_support"],
            "filament_max_volumetric_speed": ["19", "33"], "nozzle_temperature": ["222", "233"]}

def user_process():
    parent = resolve(PROCESS)
    return {"type": "process", "name": "My 0.20 2.4", "from": "User", "inherits": PROCESS, "version": "2.4.0",
            "print_settings_id": "My 0.20 2.4", "process_flow_support": parent["process_flow_support"],
            "outer_wall_speed": ["150", "420"], "ironing_speed": ["30", "45"]}

os.makedirs(OUT, exist_ok=True)
for name, data in (("project_settings_u1_head2_high_flow.config", project()), ("user_filament.json", user_filament()),
                   ("user_process.json", user_process())):
    with open(os.path.join(OUT, name), "w", encoding="utf-8") as f:
        json.dump(data, f, indent=4, ensure_ascii=False)
        f.write("\n")
    print(name, len(data), "keys")
