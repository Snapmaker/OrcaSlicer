#!/usr/bin/env python3
"""
G-code Parameter Analyzer for OrcaSlicer Filament-Extruder Mapping Verification

This script extracts and displays retraction-related parameters from G-code files
to verify that filament-extruder mapping is working correctly.

OrcaSlicer G-code format:
- Configuration at END of file (not beginning)
- Extruder parameters: 4 values (one per physical extruder)
- Filament parameters: 8 values (one per logical filament)
- Actual G-code uses T0-T7 for 8 filaments

Usage:
    python analyze_gcode_params.py <gcode_file> [--extruders N] [--filaments N]

Example:
    python analyze_gcode_params.py output.gcode --extruders 4 --filaments 8
"""

import re
import sys
import argparse
from collections import defaultdict
from dataclasses import dataclass, field
from typing import Dict, List, Optional, Tuple

# Z-hop type mapping
Z_HOP_TYPES = {
    0: 'Auto',
    1: 'Normal',
    2: 'Slope',
    3: 'Spiral'
}

@dataclass
class AnalysisResult:
    """Stores analysis results"""
    filename: str
    extruder_count: int = 4
    filament_count: int = 8

    # Config from G-code comments (end of file)
    # Extruder-level params (4 values)
    extruder_retraction_length: List[float] = field(default_factory=list)
    extruder_z_hop: List[float] = field(default_factory=list)
    extruder_wipe_distance: List[float] = field(default_factory=list)
    extruder_retraction_speed: List[float] = field(default_factory=list)
    extruder_deretraction_speed: List[float] = field(default_factory=list)

    # Filament-level params (8 values)
    filament_z_hop_types: List[str] = field(default_factory=list)

    # Detected G-code operations
    total_layers: int = 0
    total_tool_changes: int = 0
    tool_usage: Dict[int, int] = field(default_factory=dict)

    # Per-tool detected operations
    z_hop_events: Dict[int, List[Tuple[int, float]]] = field(default_factory=dict)
    retraction_events: Dict[int, List[Tuple[int, float, float]]] = field(default_factory=dict)
    wipe_events: Dict[int, List[Tuple[int, float]]] = field(default_factory=dict)


def parse_config_section(content: str, result: AnalysisResult):
    """Parse configuration from end of G-code file"""
    lines = content.split('\n')

    # Find config section (usually at end)
    # Look for lines like: ; retraction_length = 1.51,1.52,1.53,1.54
    for line in lines:
        line = line.strip()

        # Extruder-level parameters (4 values)
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

        match = re.match(r'^; deretraction_speed = ([\d.,]+)$', line)
        if match:
            result.extruder_deretraction_speed = [float(v) for v in match.group(1).split(',')]

        # Filament-level parameters (8 values)
        match = re.match(r'^; z_hop_types = (.+)$', line)
        if match:
            values = [v.strip() for v in match.group(1).split(',')]
            result.filament_z_hop_types = values


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
    prev_z = 0.0

    for line_num, line in enumerate(lines, 1):
        original_line = line
        line = line.strip()

        # Skip empty lines
        if not line:
            continue

        # Check for layer markers
        if line.startswith(';LAYER:'):
            try:
                layer_num = int(line.split(':')[1])
                if layer_num > result.total_layers:
                    result.total_layers = layer_num
            except:
                pass
            continue

        # Skip pure comments
        if line.startswith(';'):
            continue

        # Parse tool changes
        # Format: T0, T1, etc. or M900 K0.02 T1
        tool_match = re.match(r'^T(\d+)', line)
        if tool_match:
            new_tool = int(tool_match.group(1))
            if new_tool != current_tool:
                result.total_tool_changes += 1
            current_tool = new_tool
            result.tool_usage[current_tool] = result.tool_usage.get(current_tool, 0) + 1
            continue

        # Also check for tool in M900 command
        m900_match = re.search(r'T(\d+)', line)
        if line.startswith('M900') and m900_match:
            new_tool = int(m900_match.group(1))
            if new_tool != current_tool:
                result.total_tool_changes += 1
            current_tool = new_tool
            result.tool_usage[current_tool] = result.tool_usage.get(current_tool, 0) + 1

        # Parse G1 moves
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

            # Detect Z-hop (Z increase during retraction state - true z-hop)
            z_delta = new_z - current_z
            e_delta = new_e - current_e

            # Z-hop is a Z lift during/after retraction
            # We detect it when we're in retraction state (after E decrease, before E increase)
            if 0.05 < z_delta < 3.0 and in_retraction:
                if current_tool not in result.z_hop_events:
                    result.z_hop_events[current_tool] = []
                result.z_hop_events[current_tool].append((line_num, z_delta))

            # Detect retraction (E decrease)
            if e_delta < -0.01:
                if current_tool not in result.retraction_events:
                    result.retraction_events[current_tool] = []
                result.retraction_events[current_tool].append((line_num, e_delta, feedrate or 0))
                in_retraction = True
                wipe_start_x, wipe_start_y = current_x, current_y

            # Detect deretraction (E increase after retraction)
            elif in_retraction and e_delta > 0.01:
                in_retraction = False

            # Detect wipe (XY move during retraction)
            if in_retraction:
                xy_dist = ((new_x - wipe_start_x)**2 + (new_y - wipe_start_y)**2)**0.5
                if xy_dist > 0.5:
                    if current_tool not in result.wipe_events:
                        result.wipe_events[current_tool] = []
                    result.wipe_events[current_tool].append((line_num, xy_dist))

            current_x, current_y, current_z, current_e = new_x, new_y, new_z, new_e


def print_report(result: AnalysisResult):
    """Print analysis report"""
    print("=" * 80)
    print(f"OrcaSlicer G-code Analysis: {result.filename}")
    print("=" * 80)

    # Configuration summary
    print("\n[Configuration from G-code]")
    print("-" * 60)

    if result.extruder_retraction_length:
        print(f"Extruder retraction_length: {result.extruder_retraction_length}")
    if result.extruder_z_hop:
        print(f"Extruder z_hop: {result.extruder_z_hop}")
    if result.extruder_wipe_distance:
        print(f"Extruder wipe_distance: {result.extruder_wipe_distance}")
    if result.extruder_retraction_speed:
        print(f"Extruder retraction_speed: {result.extruder_retraction_speed}")
    if result.extruder_deretraction_speed:
        print(f"Extruder deretraction_speed: {result.extruder_deretraction_speed}")

    if result.filament_z_hop_types:
        print(f"\nFilament z_hop_types ({len(result.filament_z_hop_types)} values):")
        for i, v in enumerate(result.filament_z_hop_types):
            # Calculate which extruder this filament maps to (default: modulo)
            mapped_extruder = i % result.extruder_count
            print(f"  F{i+1}: {v} (maps to E{mapped_extruder+1})")

    # G-code statistics
    print("\n[G-code Statistics]")
    print("-" * 60)
    print(f"Total layers: {result.total_layers}")
    print(f"Total tool changes: {result.total_tool_changes}")
    print(f"Tools used: {sorted(result.tool_usage.keys())}")

    # Per-tool analysis
    print("\n[Detected Operations per Tool (Filament)]")
    print("-" * 80)
    print(f"{'Tool':<6} {'Z-Hops':<12} {'Avg Z-Hop':<12} {'Retractions':<14} {'Avg Retract':<14} {'Wipes':<10}")
    print("-" * 80)

    all_tools = set(result.z_hop_events.keys()) | set(result.retraction_events.keys()) | set(result.wipe_events.keys())
    for tool in sorted(all_tools):
        z_hops = result.z_hop_events.get(tool, [])
        retractions = result.retraction_events.get(tool, [])
        wipes = result.wipe_events.get(tool, [])

        z_hop_count = len(z_hops)
        avg_z_hop = sum(z for _, z in z_hops) / z_hop_count if z_hops else 0

        retraction_count = len(retractions)
        avg_retract = sum(r for _, r, _ in retractions) / retraction_count if retractions else 0

        wipe_count = len(wipes)
        avg_wipe = sum(w for _, w in wipes) / wipe_count if wipes else 0

        # Determine mapped extruder
        mapped_e = tool % result.extruder_count

        print(f"T{tool}     {z_hop_count:<12} {avg_z_hop:<12.3f} {retraction_count:<14} {avg_retract:<14.3f} {wipe_count:<10} (->E{mapped_e+1})")

    # Verification summary
    print("\n[Mapping Verification]")
    print("-" * 60)

    if not result.extruder_retraction_length:
        print("WARNING: Could not find extruder configuration in G-code")
        print("This may indicate an older G-code format or parsing issue")
    else:
        print("Checking if filament parameters inherit correctly from mapped extruders...")
        print()

        # For tools 4-7 (filaments 5-8), check if values match mapped extruder
        for tool in range(result.extruder_count, result.filament_count):
            mapped_e = tool % result.extruder_count

            # Check Z-hop
            if tool in result.z_hop_events and mapped_e < len(result.extruder_z_hop):
                tool_avg_z_hop = sum(z for _, z in result.z_hop_events[tool]) / len(result.z_hop_events[tool])
                expected_z_hop = result.extruder_z_hop[mapped_e]
                z_hop_match = "OK" if abs(tool_avg_z_hop - expected_z_hop) < 1.0 else "MISMATCH"
                print(f"  T{tool} (F{tool+1} -> E{mapped_e+1}): Z-hop avg={tool_avg_z_hop:.3f}mm, expected~{expected_z_hop}mm [{z_hop_match}]")

            # Check retraction
            if tool in result.retraction_events and mapped_e < len(result.extruder_retraction_length):
                tool_avg_retract = sum(r for _, r, _ in result.retraction_events[tool]) / len(result.retraction_events[tool])
                expected_retract = result.extruder_retraction_length[mapped_e]
                retract_match = "OK" if abs(abs(tool_avg_retract) - expected_retract) < 0.5 else "MISMATCH"
                print(f"  T{tool} (F{tool+1} -> E{mapped_e+1}): Retract avg={abs(tool_avg_retract):.3f}mm, expected~{expected_retract}mm [{retract_match}]")

    print("\n" + "=" * 80)


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
                        help='Number of physical extruders (default: 4)')
    parser.add_argument('--filaments', '-f', type=int, default=8,
                        help='Number of filaments (default: 8)')

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
    print(f"Expecting {args.extruders} extruders, {args.filaments} filaments")
    print()

    parse_config_section(content, result)
    parse_gcode_operations(content, result)
    print_report(result)


if __name__ == '__main__':
    main()
