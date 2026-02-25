#!/usr/bin/env python3
"""
G-code Parameter Analyzer for OrcaSlicer Filament-Extruder Mapping Verification

Enhanced version that:
- Parses configuration from G-code header comments (accurate)
- Verifies mapping and override logic
- No longer relies on unreliable G-code operation analysis

Usage:
    python analyze_gcode_params.py <gcode_file>
    python analyze_gcode_params.py <gcode_file> --map "0:0,1:1,2:2,3:3,4:0,5:1,6:2,7:3"

Example:
    python analyze_gcode_params.py output.gcode
    python analyze_gcode_params.py output.gcode --map "4:0,5:1,6:2,7:3"
"""

import re
import sys
import argparse
import json
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple, Any


@dataclass
class AnalysisResult:
    """Stores analysis results"""
    filename: str
    extruder_count: int = 4
    filament_count: int = 8

    # Filament to extruder mapping (from G-code comments)
    filament_extruder_map: List[int] = field(default_factory=list)

    # Extruder-level params (N values for N extruders)
    extruder_retraction_length: List[float] = field(default_factory=list)
    extruder_z_hop: List[float] = field(default_factory=list)
    extruder_wipe_distance: List[float] = field(default_factory=list)
    extruder_z_hop_types: List[str] = field(default_factory=list)

    # Filament override params (raw values from config, may contain 'nil')
    filament_retraction_length_raw: List[str] = field(default_factory=list)
    filament_z_hop_raw: List[str] = field(default_factory=list)
    filament_wipe_distance_raw: List[str] = field(default_factory=list)
    filament_z_hop_types_raw: List[str] = field(default_factory=list)

    # Computed effective values (after inheritance)
    effective_retraction_length: List[float] = field(default_factory=list)
    effective_z_hop: List[float] = field(default_factory=list)
    effective_wipe_distance: List[float] = field(default_factory=list)
    effective_z_hop_types: List[str] = field(default_factory=list)

    # Override flags (True = has override, False = inherits)
    has_retraction_override: List[bool] = field(default_factory=list)
    has_z_hop_override: List[bool] = field(default_factory=list)
    has_wipe_override: List[bool] = field(default_factory=list)
    has_z_hop_types_override: List[bool] = field(default_factory=list)

    # G-code statistics (for reference)
    total_layers: int = 0
    total_tool_changes: int = 0
    tools_used: List[int] = field(default_factory=list)


def parse_list_value(value_str: str) -> List[str]:
    """Parse a comma-separated list value, handling quoted strings"""
    values = []
    current = ""
    in_quotes = False

    for char in value_str:
        if char == '"':
            in_quotes = not in_quotes
            current += char
        elif char == ',' and not in_quotes:
            values.append(current.strip().strip('"'))
            current = ""
        else:
            current += char

    if current.strip():
        values.append(current.strip().strip('"'))

    return values


def is_nil_value(value: str) -> bool:
    """Check if a value represents nil/inherit"""
    return value.lower() in ('nil', 'none', '', 'null')


def parse_float_or_nil(value: str) -> Tuple[bool, Optional[float]]:
    """Parse a float value or detect nil. Returns (is_nil, value)"""
    if is_nil_value(value):
        return True, None
    try:
        return False, float(value)
    except ValueError:
        return True, None


def parse_header_config(content: str, result: AnalysisResult):
    """Parse configuration from G-code header block"""
    lines = content.split('\n')
    in_mapping_section = False
    in_extruder_section = False
    in_filament_section = False

    for line in lines:
        line = line.strip()

        # Detect sections
        if '[Filament-Extruder Mapping Configuration]' in line:
            in_mapping_section = True
            in_extruder_section = False
            in_filament_section = False
            continue
        elif '[Extruder Parameters]' in line:
            in_mapping_section = False
            in_extruder_section = True
            in_filament_section = False
            continue
        elif '[Filament Overrides]' in line:
            in_mapping_section = False
            in_extruder_section = False
            in_filament_section = True
            continue
        elif 'HEADER_BLOCK_END' in line:
            break

        # Parse mapping section
        if in_mapping_section:
            match = re.match(r'^;\s*filament_extruder_map\s*:\s*(.+)$', line)
            if match:
                values = parse_list_value(match.group(1))
                result.filament_extruder_map = [int(v) for v in values if v.isdigit()]

        # Parse extruder parameters
        if in_extruder_section:
            match = re.match(r'^;\s*retraction_length\s*:\s*(.+)$', line)
            if match:
                values = parse_list_value(match.group(1))
                result.extruder_retraction_length = [float(v) for v in values if v]

            match = re.match(r'^;\s*z_hop\s*:\s*(.+)$', line)
            if match:
                values = parse_list_value(match.group(1))
                result.extruder_z_hop = [float(v) for v in values if v]

            match = re.match(r'^;\s*wipe_distance\s*:\s*(.+)$', line)
            if match:
                values = parse_list_value(match.group(1))
                result.extruder_wipe_distance = [float(v) for v in values if v]

            match = re.match(r'^;\s*z_hop_types\s*:\s*(.+)$', line)
            if match:
                result.extruder_z_hop_types = parse_list_value(match.group(1))

        # Parse filament overrides
        if in_filament_section:
            match = re.match(r'^;\s*filament_retraction_length\s*:\s*(.+)$', line)
            if match:
                result.filament_retraction_length_raw = parse_list_value(match.group(1))

            match = re.match(r'^;\s*filament_z_hop\s*:\s*(.+)$', line)
            if match:
                result.filament_z_hop_raw = parse_list_value(match.group(1))

            match = re.match(r'^;\s*filament_wipe_distance\s*:\s*(.+)$', line)
            if match:
                result.filament_wipe_distance_raw = parse_list_value(match.group(1))

            match = re.match(r'^;\s*filament_z_hop_types\s*:\s*(.+)$', line)
            if match:
                result.filament_z_hop_types_raw = parse_list_value(match.group(1))

    # Set filament count based on mapping
    if result.filament_extruder_map:
        result.filament_count = len(result.filament_extruder_map)

    # Set extruder count based on max mapping value + 1
    if result.filament_extruder_map:
        result.extruder_count = max(result.filament_extruder_map) + 1

    # Compute effective values
    compute_effective_values(result)


def compute_effective_values(result: AnalysisResult):
    """Compute effective parameter values after applying overrides and inheritance"""
    filament_count = result.filament_count

    # Initialize lists
    result.effective_retraction_length = []
    result.effective_z_hop = []
    result.effective_wipe_distance = []
    result.effective_z_hop_types = []
    result.has_retraction_override = []
    result.has_z_hop_override = []
    result.has_wipe_override = []
    result.has_z_hop_types_override = []

    for f in range(filament_count):
        # Get mapped extruder
        if f < len(result.filament_extruder_map):
            e = result.filament_extruder_map[f]
        else:
            e = f % result.extruder_count

        # Process retraction_length
        if f < len(result.filament_retraction_length_raw):
            raw_val = result.filament_retraction_length_raw[f]
            is_nil, val = parse_float_or_nil(raw_val)
            result.has_retraction_override.append(not is_nil)
            if is_nil and e < len(result.extruder_retraction_length):
                result.effective_retraction_length.append(result.extruder_retraction_length[e])
            else:
                result.effective_retraction_length.append(val if val is not None else 0.0)
        elif e < len(result.extruder_retraction_length):
            result.has_retraction_override.append(False)
            result.effective_retraction_length.append(result.extruder_retraction_length[e])
        else:
            result.has_retraction_override.append(False)
            result.effective_retraction_length.append(0.0)

        # Process z_hop
        if f < len(result.filament_z_hop_raw):
            raw_val = result.filament_z_hop_raw[f]
            is_nil, val = parse_float_or_nil(raw_val)
            result.has_z_hop_override.append(not is_nil)
            if is_nil and e < len(result.extruder_z_hop):
                result.effective_z_hop.append(result.extruder_z_hop[e])
            else:
                result.effective_z_hop.append(val if val is not None else 0.0)
        elif e < len(result.extruder_z_hop):
            result.has_z_hop_override.append(False)
            result.effective_z_hop.append(result.extruder_z_hop[e])
        else:
            result.has_z_hop_override.append(False)
            result.effective_z_hop.append(0.0)

        # Process wipe_distance
        if f < len(result.filament_wipe_distance_raw):
            raw_val = result.filament_wipe_distance_raw[f]
            is_nil, val = parse_float_or_nil(raw_val)
            result.has_wipe_override.append(not is_nil)
            if is_nil and e < len(result.extruder_wipe_distance):
                result.effective_wipe_distance.append(result.extruder_wipe_distance[e])
            else:
                result.effective_wipe_distance.append(val if val is not None else 0.0)
        elif e < len(result.extruder_wipe_distance):
            result.has_wipe_override.append(False)
            result.effective_wipe_distance.append(result.extruder_wipe_distance[e])
        else:
            result.has_wipe_override.append(False)
            result.effective_wipe_distance.append(0.0)

        # Process z_hop_types
        if f < len(result.filament_z_hop_types_raw):
            raw_val = result.filament_z_hop_types_raw[f]
            is_nil = is_nil_value(raw_val)
            result.has_z_hop_types_override.append(not is_nil)
            if is_nil and e < len(result.extruder_z_hop_types):
                result.effective_z_hop_types.append(result.extruder_z_hop_types[e])
            else:
                result.effective_z_hop_types.append(raw_val if raw_val else "Normal Lift")
        elif e < len(result.extruder_z_hop_types):
            result.has_z_hop_types_override.append(False)
            result.effective_z_hop_types.append(result.extruder_z_hop_types[e])
        else:
            result.has_z_hop_types_override.append(False)
            result.effective_z_hop_types.append("Normal Lift")


def parse_gcode_statistics(content: str, result: AnalysisResult):
    """Parse G-code for basic statistics"""
    lines = content.split('\n')
    current_tool = 0
    tools_seen = set()

    for line in lines:
        line = line.strip()

        if not line or line.startswith(';'):
            # Check for layer markers
            if line.startswith(';LAYER:'):
                try:
                    layer_num = int(line.split(':')[1])
                    if layer_num > result.total_layers:
                        result.total_layers = layer_num
                except:
                    pass
            continue

        # Parse tool changes
        tool_match = re.match(r'^T(\d+)', line)
        if tool_match:
            new_tool = int(tool_match.group(1))
            if new_tool != current_tool:
                result.total_tool_changes += 1
            current_tool = new_tool
            tools_seen.add(current_tool)

    result.tools_used = sorted(list(tools_seen))


def print_report(result: AnalysisResult):
    """Print analysis report"""
    print("=" * 80)
    print(f"OrcaSlicer G-code Configuration Analysis: {result.filename}")
    print("=" * 80)

    # Mapping info
    print("\n[Filament -> Extruder Mapping]")
    print("-" * 60)
    if result.filament_extruder_map:
        print(f"Mapping detected from G-code header:")
        for f, e in enumerate(result.filament_extruder_map):
            print(f"  F{f+1} -> E{e+1}")
    else:
        print("WARNING: No mapping found in G-code header!")

    # Extruder configuration
    print("\n[Extruder Parameters]")
    print("-" * 60)
    if result.extruder_retraction_length:
        print(f"retraction_length: {result.extruder_retraction_length}")
    if result.extruder_z_hop:
        print(f"z_hop: {result.extruder_z_hop}")
    if result.extruder_wipe_distance:
        print(f"wipe_distance: {result.extruder_wipe_distance}")
    if result.extruder_z_hop_types:
        print(f"z_hop_types: {result.extruder_z_hop_types}")

    # Filament overrides table
    print("\n[Filament Parameter Analysis]")
    print("-" * 100)
    print(f"{'Filament':<10} {'MapsTo':<8} {'retract':<15} {'z_hop':<12} {'wipe':<12} {'Override?':<20}")
    print("-" * 100)

    for f in range(result.filament_count):
        e = result.filament_extruder_map[f] if f < len(result.filament_extruder_map) else f % result.extruder_count

        # Format retraction info
        if f < len(result.effective_retraction_length):
            retract_val = f"{result.effective_retraction_length[f]:.2f}"
            if f < len(result.has_retraction_override) and result.has_retraction_override[f]:
                retract_val += " [OVR]"
        else:
            retract_val = "N/A"

        # Format z_hop info
        if f < len(result.effective_z_hop):
            zhop_val = f"{result.effective_z_hop[f]:.2f}"
            if f < len(result.has_z_hop_override) and result.has_z_hop_override[f]:
                zhop_val += " [OVR]"
        else:
            zhop_val = "N/A"

        # Format wipe info
        if f < len(result.effective_wipe_distance):
            wipe_val = f"{result.effective_wipe_distance[f]:.1f}"
            if f < len(result.has_wipe_override) and result.has_wipe_override[f]:
                wipe_val += " [OVR]"
        else:
            wipe_val = "N/A"

        # Check for any override
        has_override = []
        if f < len(result.has_retraction_override) and result.has_retraction_override[f]:
            has_override.append("retract")
        if f < len(result.has_z_hop_override) and result.has_z_hop_override[f]:
            has_override.append("z_hop")
        if f < len(result.has_wipe_override) and result.has_wipe_override[f]:
            has_override.append("wipe")
        if f < len(result.has_z_hop_types_override) and result.has_z_hop_types_override[f]:
            has_override.append("z_hop_type")

        override_str = ",".join(has_override) if has_override else "inherit"

        print(f"F{f+1:<9} E{e+1:<7} {retract_val:<15} {zhop_val:<12} {wipe_val:<12} {override_str:<20}")

    # Verification summary
    print("\n" + "=" * 80)
    print("[VERIFICATION SUMMARY]")
    print("=" * 80)

    if not result.filament_extruder_map:
        print("ERROR: No filament-extruder mapping found in G-code header!")
        print("       This G-code may have been generated with an older version.")
        return

    if not result.extruder_retraction_length:
        print("ERROR: No extruder parameters found in G-code header!")
        print("       This G-code may have been generated with an older version.")
        return

    # Verify inheritance logic
    issues = []

    for f in range(result.filament_count):
        e = result.filament_extruder_map[f]

        # Check if filament without override correctly inherits from extruder
        if f < len(result.has_retraction_override) and not result.has_retraction_override[f]:
            if e < len(result.extruder_retraction_length):
                expected = result.extruder_retraction_length[e]
                actual = result.effective_retraction_length[f]
                if abs(expected - actual) > 0.001:
                    issues.append(f"F{f+1}: retraction inheritance mismatch (expected {expected}, got {actual})")

    if issues:
        print("ISSUES FOUND:")
        for issue in issues:
            print(f"  - {issue}")
    else:
        print("All inheritance logic verified successfully!")

    # Statistics
    print("\n[G-code Statistics]")
    print("-" * 60)
    print(f"Total layers: {result.total_layers}")
    print(f"Total tool changes: {result.total_tool_changes}")
    print(f"Tools used: {result.tools_used}")

    print("\n" + "=" * 80)
    print("ANALYSIS COMPLETE")
    print("=" * 80)


def main():
    parser = argparse.ArgumentParser(
        description='Analyze OrcaSlicer G-code for filament-extruder mapping verification',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python analyze_gcode_params.py output.gcode
  python analyze_gcode_params.py output.gcode --extruders 4 --filaments 8
        """
    )
    parser.add_argument('gcode_file', help='Path to G-code file to analyze')
    parser.add_argument('--extruders', '-e', type=int, default=4,
                        help='Number of physical extruders (default: 4, auto-detected from G-code)')
    parser.add_argument('--filaments', '-f', type=int, default=8,
                        help='Number of filaments (default: 8, auto-detected from G-code)')

    args = parser.parse_args()

    try:
        with open(args.gcode_file, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
    except FileNotFoundError:
        print(f"Error: File not found: {args.gcode_file}")
        sys.exit(1)
    except Exception as e:
        print(f"Error reading file: {e}")
        sys.exit(1)

    result = AnalysisResult(
        filename=args.gcode_file,
        extruder_count=args.extruders,
        filament_count=args.filaments
    )

    print(f"Analyzing {args.gcode_file}...")

    # Parse configuration from header (primary method)
    parse_header_config(content, result)

    # Parse statistics
    parse_gcode_statistics(content, result)

    # Print report
    print_report(result)


if __name__ == '__main__':
    main()
