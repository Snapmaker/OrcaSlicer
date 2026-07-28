const { app, BrowserWindow } = require("electron");
const path = require("path");
const { spawn } = require("child_process");
const http = require("http");

const PORT = 18964;
const HOST = "127.0.0.1";

function getServerDir() {
  if (app.isPackaged) {
    return path.join(process.resourcesPath, "server");
  }
  return path.resolve(__dirname, "..");
}

function checkServerReady() {
  return new Promise((resolve) => {
    const req = http.get(`http://${HOST}:${PORT}/`, (res) => resolve(true));
    req.on("error", () => resolve(false));
    req.setTimeout(1000, () => { req.destroy(); resolve(false); });
  });
}

async function waitForServer(maxWaitMs) {
  if (!maxWaitMs) maxWaitMs = 15000;
  const start = Date.now();
  while (Date.now() - start < maxWaitMs) {
    if (await checkServerReady()) return true;
    await new Promise((r) => setTimeout(r, 300));
  }
  return false;
}

let pythonProcess = null;

async function main() {
  const serverDir = getServerDir();
  const appPy = path.join(serverDir, "app.py");

  // Kill stale server from a previous session so we start clean
  try {
    require("child_process").execSync(
      `powershell -NoProfile -Command "Get-Process python -ErrorAction SilentlyContinue | Where-Object { $_.CommandLine -like '*app.py*' } | Stop-Process -Force"`,
      { windowsHide: true, timeout: 3000 }
    );
    await new Promise((r) => setTimeout(r, 500));
  } catch (_) {}

  const pythonCmd = process.platform === "win32" ? "python" : "python3";
  pythonProcess = spawn(pythonCmd, [appPy, String(PORT)], {
    cwd: serverDir,
    stdio: ["ignore", "pipe", "pipe"],
    windowsHide: true,
  });

  pythonProcess.stdout.on("data", (d) => console.log("[server]", d.toString().trim()));
  pythonProcess.stderr.on("data", (d) => console.error("[server:err]", d.toString().trim()));
  pythonProcess.on("exit", (code) => { pythonProcess = null; });

  const ready = await waitForServer();
  if (!ready) { app.quit(); return; }

  // Reset session for a clean start
  try {
    await fetch(`http://${HOST}:${PORT}/api/reset`, { method: "POST" });
  } catch (_) {}

  const win = new BrowserWindow({
    width: 1280, height: 860,
    minWidth: 900, minHeight: 600,
    title: "Snapmaker CLI Test Tool",
    webPreferences: {
      preload: path.join(__dirname, "preload.js"),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  win.loadURL(`http://${HOST}:${PORT}/`);
  win.on("closed", () => { if (pythonProcess) { pythonProcess.kill(); pythonProcess = null; } });
}

app.whenReady().then(main);
app.on("window-all-closed", () => {
  if (pythonProcess) { pythonProcess.kill(); pythonProcess = null; }
  app.quit();
});
