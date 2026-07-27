# Snapmaker CLI Slicing Test Tool
Zero-dependency local web app for batch-testing 3MF files with the Snapmaker CLI.
## Quick Start
1. Ensure Python 3.8+ is installed
2. Double-click start.bat, or run python app.py in this directory
3. Browser opens automatically at http://127.0.0.1:18964
## Configuration
- CLI Path: auto-detected from default build location
- Data Directory: auto-detected from resources/
- Output Directory: where G-code and reports are stored
## Advanced Options
- Slice Mode: 0 = all plates, N = plate N
- Max Time/Plate (--mstpp): 0 = disabled
- Max Triangles (--mtcpp): 0 = disabled
- Timeout: wall-clock limit per file
- --allow-newer-file and --use-relative-e-distances=0 enabled by default
## Workflow
1. Enter or browse for a 3MF file or directory
2. Click Scan to find all .3mf files
3. Click Start Slicing
4. Review results dashboard with per-file logs and failure analysis
5. Click Export Report for a standalone HTML report
## Output Structure
output_dir/_reports/report_TIMESTAMP.json
output_dir/3mf_name/plate_N.gcode
## Tech Stack
- Backend: Python 3.8+ standard library (zero dependencies)
- Frontend: Vanilla HTML/CSS/JS
- Real-time: SSE (Server-Sent Events)
