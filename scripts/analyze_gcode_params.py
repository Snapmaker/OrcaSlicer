#!/usr/bin/env python3
"""
G-code Parameter Analyzer for OrcaSlicer Filament-Extruder Mapping Verification

Enhanced version that:
- Reads actual filament_extruder_map from G-code
- Supports custom mapping verification
- Better parameter comparison

Usage:
    python analyze_gcode_params.py <gcode_file> [--extruders N] [--filaments N]
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
from typing import Dict, List, Optional, Tuple

@dataclass
class AnalysisResult:
    """Stores analysis results"""
    filename: str
    extruder_count: int = 4
    filament_count: int = 8

    # Filament to extruder mapping (from G-code or user-specified)
    filament_extruder_map: Dict[int, int] = field(default_factory=dict)

    # Config from G-code comments
    # Extruder-level params (N values for N extruders)
    extruder_retraction_length: List[float] = field(default_factory=list)
    extruder_z_hop: List[float] = field(default_factory=list)
    extruder_wipe_distance: List[float] = field(default_factory=list)
    extruder_retraction_speed: List[float] = field(default_factory=list)
    extruder_z_hop_types: List[str] = field(default_factory=list)

    # Filament-level params (8 values) - these show ACTUAL values used
    filament_retraction_length: List[float] = field(default_factory=list)
    filament_z_hop: List[float] = field(default_factory=list)
    filament_wipe_distance: List[float] = field(default_factory=list)
    filament_z_hop_types: List[str] = field(default_factory=list)

    # Detected G-code operations
    total_layers: int = 0
    total_tool_changes: int = 0
    tool_usage: Dict[int, int] = field(default_factory=dict)

    # Per-tool detected operations
    z_hop_events: Dict[int, List[Tuple[int, float]]] = field(default_factory=dict)
    retraction_events: Dict[int, List[Tuple[int, float, float]]] = field(default_factory=dict)
    wipe_events: Dict[int, List[Tuple[int, float]]] = field(default_factory=dict)


def parse_filament_extruder_map(content: str) -> Dict[int, int]:
    """Parse filament_extruder_map from G-code comments"""
    mapping = {}
    lines = content.split('\n')

    for line in lines:
        line = line.strip()
        # Try different formats
        # Format 1: ; filament_extruder_map = {0:0, 1:1, 2:2, 3:3, 4:0, 5:1, 6:2, 7:3}
        match = re.search(r'filament_extruder_map\s*[=:]\s*\{([^}]+)\}', line, re.IGNORECASE)
        if match:
            pairs = re.findall(r'(\d+)\s*:\s*(\d+)', match.group(1))
            for f, e in pairs:
                mapping[int(f)] = int(e)
            return mapping

        # Format 2: ; filament_map = 0,1,2,3,0,1,2,3 (comma-separated)
        match = re.search(r'filament_map\s*[=:]\s*([\d,]+)', line, re.IGNORECASE)
        if match:
            values = [int(v.strip()) for v in match.group(1).split(',')]
            for i, e in enumerate(values):
                mapping[i] = e
            return mapping

    return mapping


def parse_config_section(content: str, result: AnalysisResult):
    """Parse configuration from end of G-code file"""
    lines = content.split('\n')

    for line in lines:
        line = line.strip()

        # Extruder-level parameters (N values)
        match = re.match(r'^; retraction_length = ([\d.,]+)$', line)
        if match:
            result.extruder_retraction_length = [float(v) for v in match.group(1).split(',')]

        match = re.match(r'^; z_hop = ([\d.,]+)$', line)
        if match:
            result.extruder_z_hop = [float(v) for v in match.group(1).split(',')]

        match = re.match(r'^; wipe_distance = ([\d.,]+)$', line)
        if match:
            result.extruder_wipe_distance = [float(v) for v in match.group(1).split(',')]

        match = re.match(r'^; retraction_speed = ([\d.,]+)$', line)
        if match:
            result.extruder_retraction_speed = [float(v) for v in match.group(1).split(',')]

        # z_hop_types at extruder level
        match = re.match(r'^; z_hop_types = (.+)$', line)
        if match:
            values = [v.strip() for v in match.group(1).split(',')]
            # Determine if this is extruder-level (4 values) or filament-level (8 values)
            if len(values) <= result.extruder_count:
                result.extruder_z_hop_types = values
            else:
                result.filament_z_hop_types = values

        # Filament-level parameters (try to parse filament_ prefixed versions)
        match = re.match(r'^; filament_retraction_length = ([\d.,]+)$', line)
        if match:
            result.filament_retraction_length = [float(v) for v in match.group(1).split(',')]

        match = re.match(r'^; filament_z_hop = ([\d.,]+)$', line)
        if match:
            result.filament_z_hop = [float(v) for v in match.group(1).split(',')]

        match = re.match(r'^; filament_wipe_distance = ([\d.,]+)$', line)
        if match:
            result.filament_wipe_distance = [float(v) for v in match.group(1).split(',')]


def parse_gcode_operations(content: str, result: AnalysisResult):
    """Parse actual G-code operations to detect retraction patterns"""
    lines = content.split('\n')
    current_tool = 0
    current_z = 0.0
    current_e = 0.0
    current_x = 0.0
    current_y = 0.0
    in_retraction = False
    wipe_start_x = 0.0
    wipe_start_y = 0.0

    for line_num, line in enumerate(lines, 1):
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
            result.tool_usage[current_tool] = result.tool_usage.get(current_tool, 0) + 1
            continue

        # Check for tool in other commands
        m900_match = re.search(r'T(\d+)', line)
        if line.startswith('M900') and m900_match:
            new_tool = int(m900_match.group(1))
            if new_tool != current_tool:
                result.total_tool_changes += 1
            current_tool = new_tool
            result.tool_usage[current_tool] = result.tool_usage.get(current_tool, 0) + 1

        # Parse G1/G0 moves
        if line.startswith('G1') or line.startswith('G0'):
            parts = line.split(';')[0].split()
            new_x, new_y, new_z, new_e = current_x, current_y, current_z, current_e
            feedrate = None

            for part in parts[1:]:
                if part.startswith('X'):
                    try: new_x = float(part[1:])
                    except: pass
                elif part.startswith('Y'):
                    try: new_y = float(part[1:])
                    except: pass
                elif part.startswith('Z'):
                    try: new_z = float(part[1:])
                    except: pass
                elif part.startswith('E'):
                    try: new_e = float(part[1:])
                    except: pass
                elif part.startswith('F'):
                    try: feedrate = float(part[1:])
                    except: pass

            z_delta = new_z - current_z
            e_delta = new_e - current_e

            # Z-hop detection
            if 0.05 < z_delta < 3.0 and in_retraction:
                if current_tool not in result.z_hop_events:
                    result.z_hop_events[current_tool] = []
                result.z_hop_events[current_tool].append((line_num, z_delta))

            # Retraction detection
            if e_delta < -0.01:
                if current_tool not in result.retraction_events:
                    result.retraction_events[current_tool] = []
                result.retraction_events[current_tool].append((line_num, e_delta, feedrate or 0))
                in_retraction = True
                wipe_start_x, wipe_start_y = current_x, current_y

            # Deretraction detection
            elif in_retraction and e_delta > 0.01:
                in_retraction = False

            # Wipe detection
            if in_retraction:
                xy_dist = ((new_x - wipe_start_x)**2 + (new_y - wipe_start_y)**2)**0.5
                if xy_dist > 0.5:
                    if current_tool not in result.wipe_events:
                        result.wipe_events[current_tool] = []
                    result.wipe_events[current_tool].append((line_num, xy_dist))

            current_x, current_y, current_z, current_e = new_x, new_y, new_z, new_e


def get_mapped_extruder(filament_idx: int, mapping: Dict[int, int], extruder_count: int) -> int:
    """Get the extruder that a filament maps to"""
    if filament_idx in mapping:
        return mapping[filament_idx]
    # Fallback to modulo
    return filament_idx % extruder_count


def print_report(result: AnalysisResult, user_mapping: Dict[int, int] = None):
    """Print analysis report"""
    # Use user-provided mapping if available, otherwise use detected mapping
    mapping = user_mapping if user_mapping else result.filament_extruder_map

    print("=" * 80)
    print(f"OrcaSlicer G-code Analysis: {result.filename}")
    print("=" * 80)

    # Mapping info
    print("\n[Filament → Extruder Mapping]")
    print("-" * 60)
    if mapping:
        print(f"Mapping source: {'User-specified' if user_mapping else 'Detected from G-code'}")
        for f in sorted(mapping.keys()):
            print(f"  F{f+1} → E{mapping[f]+1}")
    else:
        print("No mapping detected, using modulo fallback:")
        for f in range(result.filament_count):
            print(f"  F{f+1} → E{(f % result.extruder_count)+1}")

    # Configuration summary
    print("\n[Extruder Configuration]")
    print("-" * 60)

    if result.extruder_retraction_length:
        print(f"retraction_length: {result.extruder_retraction_length}")
    if result.extruder_z_hop:
        print(f"z_hop: {result.extruder_z_hop}")
    if result.extruder_wipe_distance:
        print(f"wipe_distance: {result.extruder_wipe_distance}")
    if result.extruder_z_hop_types:
        print(f"z_hop_types: {result.extruder_z_hop_types}")

    # Filament configuration (if available)
    if result.filament_z_hop_types:
        print(f"\n[Filament z_hop_types ({len(result.filament_z_hop_types)} values)]")
        for i, v in enumerate(result.filament_z_hop_types):
            mapped_e = get_mapped_extruder(i, mapping, result.extruder_count)
            print(f"  F{i+1}: {v} (maps to E{mapped_e+1})")

    # G-code statistics
    print("\n[G-code Statistics]")
    print("-" * 60)
    print(f"Total layers: {result.total_layers}")
    print(f"Total tool changes: {result.total_tool_changes}")
    print(f"Tools used: {sorted(result.tool_usage.keys())}")

    # Per-tool analysis
    print("\n[Detected Operations per Tool]")
    print("-" * 90)
    print(f"{'Tool':<6} {'Filament':<10} {'MapsTo':<8} {'RetractAvg':<12} {'ZHopAvg':<10} {'WipeCount':<10}")
    print("-" * 90)

    all_tools = set(result.z_hop_events.keys()) | set(result.retraction_events.keys()) | set(result.wipe_events.keys())
    for tool in sorted(all_tools):
        retractions = result.retraction_events.get(tool, [])
        z_hops = result.z_hop_events.get(tool, [])
        wipes = result.wipe_events.get(tool, [])

        avg_retract = sum(r for _, r, _ in retractions) / len(retractions) if retractions else 0
        avg_z_hop = sum(z for _, z in z_hops) / len(z_hops) if z_hops else 0
        wipe_count = len(wipes)

        mapped_e = get_mapped_extruder(tool, mapping, result.extruder_count)

        print(f"T{tool}     F{tool+1}       E{mapped_e+1}       {abs(avg_retract):<12.3f} {avg_z_hop:<10.3f} {wipe_count:<10}")

    # Verification summary
    print("\n" + "=" * 80)
    print("[VERIFICATION RESULTS]")
    print("=" * 80)

    if not result.extruder_retraction_length:
        print("ERROR: Could not find extruder configuration in G-code")
        return

    all_passed = True

    # Verify each filament (especially F5-F8 for 4-extruder system)
    for f in range(result.filament_count):
        mapped_e = get_mapped_extruder(f, mapping, result.extruder_count)
        tool = f  # Tool number equals filament index

        # Skip if no data for this tool
        if tool not in result.retraction_events:
            continue

        print(f"\nF{f+1} (T{tool}) → E{mapped_e+1}:")

        # Check retraction_length
        if tool in result.retraction_events and mapped_e < len(result.extruder_retraction_length):
            detected = abs(sum(r for _, r, _ in result.retraction_events[tool]) / len(result.retraction_events[tool]))
            expected = result.extruder_retraction_length[mapped_e]
            tolerance = max(0.3, expected * 0.3)  # 30% or 0.3mm tolerance
            passed = abs(detected - expected) < tolerance
            status = "PASS" if passed else "FAIL"
            if not passed:
                all_passed = False
            print(f"  retraction_length: detected={detected:.3f}mm, expected={expected:.3f}mm [{status}]")

        # Check z_hop
        if tool in result.z_hop_events and mapped_e < len(result.extruder_z_hop):
            detected = sum(z for _, z in result.z_hop_events[tool]) / len(result.z_hop_events[tool])
            expected = result.extruder_z_hop[mapped_e]
            tolerance = max(0.2, expected * 0.5)  # 50% or 0.2mm tolerance
            passed = abs(detected - expected) < tolerance
            status = "PASS" if passed else "FAIL"
            if not passed:
                all_passed = False
            print(f"  z_hop: detected={detected:.3f}mm, expected={expected:.3f}mm [{status}]")

    # Final summary
    print("\n" + "=" * 80)
    if all_passed:
        print("OVERALL: ALL TESTS PASSED")
    else:
        print("OVERALL: SOME TESTS FAILED - Check results above")
    print("=" * 80)


def main():
    parser = argparse.ArgumentParser(
        description='Analyze OrcaSlicer G-code for filament-extruder mapping verification',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python analyze_gcode_params.py output.gcode
  python analyze_gcode_params.py output.gcode --extruders 4 --filaments 8
  python analyze_gcode_params.py output.gcode --map "4:0,5:1,6:2,7:3"
        """
    )
    parser.add_argument('gcode_file', help='Path to G-code file to analyze')
    parser.add_argument('--extruders', '-e', type=int, default=4,
                        help='Number of physical extruders (default: 4)')
    parser.add_argument('--filaments', '-f', type=int, default=8,
                        help='Number of filaments (default: 8)')
    parser.add_argument('--map', '-m', type=str, default=None,
                        help='Custom mapping as "f1:e1,f2:e2,..." (e.g., "4:0,5:1,6:2,7:3")')

    args = parser.parse_args()

    # Parse user-provided mapping
    user_mapping = None
    if args.map:
        user_mapping = {}
        pairs = args.map.split(',')
        for pair in pairs:
            if ':' in pair:
                f, e = pair.split(':')
                user_mapping[int(f.strip())] = int(e.strip())

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

    # Parse mapping from G-code first
    result.filament_extruder_map = parse_filament_extruder_map(content)

    parse_config_section(content, result)
    parse_gcode_operations(content, result)
    print_report(result, user_mapping)


if __name__ == '__main__':
    main()
