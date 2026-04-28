# OrcaSlicer Cloud Slicing Engine

Headless slicing engine for cloud deployment and automation.

## Overview

`orca-slice-engine` is a command-line slicing tool that provides the same slicing capabilities as OrcaSlicer GUI without requiring a graphical environment. It's designed for:

- Cloud slicing services
- CI/CD automation
- Batch processing
- Server-side integration

## Quick Start for Cloud Services

### 1. Build (One-time Setup)

```bash
# Install system dependencies
./build_linux.sh -u

# Build library dependencies (first time only, ~30-60 min)
./build_linux.sh -d

# Build and package the engine
./build_slice_engine.sh -c -p
```

### 2. Deploy to Server

```bash
# Copy package to server
scp orca-slice-engine-linux-x64-*.tar.gz user@server:/opt/

# On server: extract and test
ssh user@server
cd /opt
tar -xzf orca-slice-engine-linux-x64-*.tar.gz
cd orca-slice-engine-linux-x64-*
./orca-slice-engine --help
```

### 3. Execute Slicing Task

```bash
# Basic slicing
./orca-slice-engine model.3mf -o output

# With specific plate
./orca-slice-engine model.3mf -p 1 -o output

# Plain G-code output
./orca-slice-engine model.3mf -f gcode -o output
```

## Building

### Prerequisites

1. **System Dependencies** (Ubuntu/Debian):
   ```bash
   ./build_linux.sh -u
   ```

2. **Library Dependencies**:
   ```bash
   ./build_linux.sh -d
   ```

### Build Commands

**Option 1: Using the dedicated build script**
```bash
./build_slice_engine.sh -c -p
```

**Option 2: Manual build**
```bash
cmake -S . -B build-engine -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$(pwd)/deps/build/destdir/usr/local" \
    -DSLIC3R_STATIC=ON \
    -DSLIC3R_GUI=OFF \
    -DSLIC3R_PCH=OFF

cmake --build build-engine --target orca-slice-engine
```

### Docker Build

```bash
docker build -f docker/Dockerfile.slice-engine -t orca-slice-engine .
docker run --rm -v $(pwd)/output:/output orca-slice-engine
```

## Usage

### Basic Usage

```bash
# Slice all plates
./orca-slice-engine model.3mf

# Slice specific plate
./orca-slice-engine model.3mf -p 1

# Custom output path
./orca-slice-engine model.3mf -o /path/to/output

# Plain G-code output
./orca-slice-engine model.3mf -f gcode
```

### Command Line Options

| Option | Description |
|--------|-------------|
| `-o, --output <file>` | Output file path (without extension) |
| `-p, --plate <id>` | Plate number (1, 2, 3...) or "all" |
| `-f, --format <fmt>` | Output format: `gcode` or `gcode.3mf` |
| `-r, --resources <dir>` | Resources directory path |
| `-v, --verbose` | Enable verbose logging |
| `-h, --help` | Show help message |

### Exit Codes

| Code | Description |
|------|-------------|
| 0 | Success |
| 1 | Invalid arguments |
| 2 | Input file not found |
| 3 | 3MF load error |
| 4 | Slicing error |
| 5 | G-code export error |
| 6 | Pre-processing validation error |
| 7 | Post-processing warning |

## Cloud Service Integration

### REST API Example (Python/Flask)

```python
from flask import Flask, request, jsonify, send_file
import subprocess
import tempfile
import os
import uuid

app = Flask(__name__)

SLICE_ENGINE = '/opt/orca-slice-engine/orca-slice-engine'
RESOURCES_DIR = '/opt/orca-slice-engine/resources'
WORK_DIR = '/var/slice_jobs'

@app.route('/slice', methods=['POST'])
def slice_model():
    """Slice a 3MF model and return the G-code."""
    
    # Save uploaded file
    job_id = str(uuid.uuid4())
    input_file = os.path.join(WORK_DIR, f'{job_id}.3mf')
    output_base = os.path.join(WORK_DIR, f'{job_id}')
    
    request.files['model'].save(input_file)
    
    # Build command
    cmd = [
        SLICE_ENGINE,
        input_file,
        '-o', output_base,
        '-r', RESOURCES_DIR,
        '-f', 'gcode.3mf'
    ]
    
    # Optional: specify plate
    plate_id = request.form.get('plate')
    if plate_id:
        cmd.extend(['-p', str(plate_id)])
    
    # Execute slicing
    result = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=600  # 10 min timeout
    )
    
    # Check result
    if result.returncode == 0 or result.returncode == 7:
        # Success (0) or success with warnings (7)
        output_file = f'{output_base}.gcode.3mf'
        return send_file(output_file, as_attachment=True)
    else:
        return jsonify({
            'error': 'Slicing failed',
            'exit_code': result.returncode,
            'stderr': result.stderr,
            'stdout': result.stdout
        }), 500

@app.route('/health', methods=['GET'])
def health():
    """Health check endpoint."""
    result = subprocess.run([SLICE_ENGINE, '--help'], capture_output=True)
    return jsonify({'status': 'ok' if result.returncode == 0 else 'error'})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8080)
```

### REST API Example (Node.js/Express)

```javascript
const express = require('express');
const multer = require('multer');
const { execFile } = require('child_process');
const path = require('path');
const fs = require('fs');
const { v4: uuidv4 } = require('uuid');

const app = express();
const upload = multer({ dest: '/var/slice_jobs/uploads' });

const SLICE_ENGINE = '/opt/orca-slice-engine/orca-slice-engine';
const RESOURCES_DIR = '/opt/orca-slice-engine/resources';
const WORK_DIR = '/var/slice_jobs';

app.post('/slice', upload.single('model'), (req, res) => {
    const jobId = uuidv4();
    const inputFile = req.file.path;
    const outputFile = path.join(WORK_DIR, `${jobId}.gcode.3mf`);
    
    const args = [
        inputFile,
        '-o', path.join(WORK_DIR, jobId),
        '-r', RESOURCES_DIR,
        '-f', 'gcode.3mf'
    ];
    
    if (req.body.plate) {
        args.push('-p', req.body.plate);
    }
    
    execFile(SLICE_ENGINE, args, { timeout: 600000 }, (error, stdout, stderr) => {
        if (error && error.code !== 0 && error.code !== 7) {
            return res.status(500).json({
                error: 'Slicing failed',
                exitCode: error.code,
                stderr: stderr
            });
        }
        
        res.download(outputFile, 'output.gcode.3mf', (err) => {
            // Cleanup
            fs.unlinkSync(inputFile);
            fs.unlinkSync(outputFile);
        });
    });
});

app.get('/health', (req, res) => {
    execFile(SLICE_ENGINE, ['--help'], (error) => {
        res.json({ status: error ? 'error' : 'ok' });
    });
});

app.listen(8080);
```

### Job Queue Integration (Python/Celery)

```python
from celery import Celery
import subprocess
import os

app = Celery('slice_worker', broker='redis://localhost:6379/0')

SLICE_ENGINE = '/opt/orca-slice-engine/orca-slice-engine'
RESOURCES_DIR = '/opt/orca-slice-engine/resources'

@app.task(bind=True)
def slice_task(self, input_file, output_base, plate_id=None):
    """Async slicing task for Celery worker."""
    
    cmd = [
        SLICE_ENGINE,
        input_file,
        '-o', output_base,
        '-r', RESOURCES_DIR,
        '-f', 'gcode.3mf'
    ]
    
    if plate_id:
        cmd.extend(['-p', str(plate_id)])
    
    # Run with progress callback
    process = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True
    )
    
    for line in process.stdout:
        # Parse progress: "[Progress] 50% - Slicing..."
        if '[Progress]' in line:
            try:
                percent = int(line.split('[Progress]')[1].split('%')[0].strip())
                self.update_state(state='PROGRESS', meta={'percent': percent})
            except:
                pass
    
    process.wait()
    
    if process.returncode in (0, 7):
        return {
            'status': 'success',
            'output': f'{output_base}.gcode.3mf',
            'warnings': process.returncode == 7
        }
    else:
        raise Exception(f'Slicing failed with code {process.returncode}')
```

## Deployment

### Package Structure

```
orca-slice-engine-linux-x64-{version}/
├── orca-slice-engine      # Wrapper script
├── bin/
│   └── orca-slice-engine  # Main executable
├── lib/                   # Bundled libraries
└── resources/             # Printer profiles
    ├── profiles/
    ├── printers/
    └── filaments/
```

### Creating a Deployment Package

```bash
./build_slice_engine.sh -c -p -o ./dist
```

This creates:
- `orca-slice-engine-linux-x64-{version}.tar.gz`
- `orca-slice-engine-linux-x64-{version}.tar.gz.sha256`

### Installation on Target Server

```bash
# Extract
tar -xzf orca-slice-engine-linux-x64-*.tar.gz
cd orca-slice-engine-linux-x64-*

# Test
./orca-slice-engine --help
```

### Systemd Service (Optional)

```ini
# /etc/systemd/system/slice-engine.service
[Unit]
Description=OrcaSlicer Cloud Engine Worker
After=network.target

[Service]
Type=simple
User=slice-worker
WorkingDirectory=/opt/orca-slice-engine
Environment=ORCA_RESOURCES=/opt/orca-slice-engine/resources
ExecStart=/opt/orca-slice-engine/orca-slice-engine
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

### Kubernetes Deployment

```yaml
apiVersion: apps/v1
kind: Deployment
metadata:
  name: orca-slice-engine
spec:
  replicas: 3
  selector:
    matchLabels:
      app: slice-engine
  template:
    metadata:
      labels:
        app: slice-engine
    spec:
      containers:
      - name: engine
        image: orca-slice-engine:latest
        resources:
          requests:
            memory: "2Gi"
            cpu: "1"
          limits:
            memory: "4Gi"
            cpu: "2"
        volumeMounts:
        - name: resources
          mountPath: /app/resources
        - name: workdir
          mountPath: /var/slice_jobs
      volumes:
      - name: resources
        persistentVolumeClaim:
          claimName: slice-engine-resources
      - name: workdir
        emptyDir: {}
```

## Troubleshooting

### Common Issues

1. **"Resources directory not found"**
   - Ensure `resources/` directory exists alongside the executable
   - Or set `ORCA_RESOURCES` environment variable

2. **"Printer profile not found"**
   - Check that the required printer profiles exist in `resources/profiles/`
   - Verify the 3MF file contains valid printer configuration

3. **Library loading errors**
   - Run the wrapper script instead of the binary directly
   - Check `ldd bin/orca-slice-engine` for missing dependencies

### Debug Mode

Enable verbose logging for troubleshooting:
```bash
./orca-slice-engine model.3mf -v
```

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                   orca-slice-engine                      │
├─────────────────────────────────────────────────────────┤
│  main.cpp                                                │
│  ├── Argument parsing                                    │
│  ├── Resource initialization                             │
│  └── Slicing orchestration                               │
├─────────────────────────────────────────────────────────┤
│  libslic3r (shared with GUI)                             │
│  ├── Model loading (3MF, STL)                           │
│  ├── Print configuration                                 │
│  ├── Slicing algorithms                                  │
│  └── G-code generation                                   │
├─────────────────────────────────────────────────────────┤
│  Dependencies                                            │
│  ├── Boost (filesystem, log, etc.)                      │
│  ├── TBB (parallel processing)                          │
│  ├── OpenVDB (spatial data)                             │
│  └── OpenSSL (networking)                               │
└─────────────────────────────────────────────────────────┘
```

## License

See [LICENSE.txt](../LICENSE.txt) for details.
