// Gate: camera tiles stop streaming when they are off-screen or the page is hidden, and start again
// when they are visible (resources/web/orca/stream_center.html + player.html).
//
// A mock hub (plain Node http, no dependencies) serves the real shipped page at /r/<token>/, the
// real player at /stream.html and /api/ws, and the Bambu relay's MJPEG at /r/<token>/bambu. The
// mock counts how many WebSockets and MJPEG responses are open at any moment - which is exactly
// what a hub (a go2rtc consumer, a printer camera slot) would count. A headless Edge / Chrome is
// driven over the DevTools protocol (Node 22's built-in WebSocket) at a phone-sized viewport.
// No printer, no go2rtc, no real camera.
//
// Run: node tests/phone/camera_visibility_gate.js        (skips, exit 0, when no Chromium is found)
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const crypto = require('crypto');
const { spawn } = require('child_process');

// WEB_DIR points the gate at another copy of the pages (to show it failing on the old ones).
const WEB = process.env.WEB_DIR || path.join(__dirname, '..', '..', 'resources', 'web', 'orca');
const TOKEN = 'abc123';
const BASE = '/r/' + TOKEN + '/';

function findBrowser() {
    const c = [
        process.env.CHROME_BIN,
        'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe',
        'C:/Program Files/Microsoft/Edge/Application/msedge.exe',
        'C:/Program Files/Google/Chrome/Application/chrome.exe',
        '/usr/bin/google-chrome', '/usr/bin/chromium', '/usr/bin/chromium-browser',
        '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',
    ].filter(Boolean);
    return c.find(p => fs.existsSync(p));
}

let failures = 0, passed = 0;
function check(label, cond, extra) {
    if (cond) { passed++; console.log('ok   - ' + label); }
    else { failures++; console.error('FAIL - ' + label + (extra !== undefined ? '  (' + extra + ')' : '')); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

// ---- the mock hub ------------------------------------------------------------------------------
const HOSTS = [];
for (let i = 1; i <= 6; i++) HOSTS.push({ id: 'cam' + i, alias: 'Cam ' + i, rkind: 'go2rtc', rname: 's' + i, rurl: '' });
HOSTS.push({ id: 'bbl1', alias: 'Bambu', rkind: 'p1', rname: '', rurl: '' });

const sockets = new Map();   // src -> Set of open /api/ws sockets
let mjpegOpen = 0, mjpegTotal = 0, wsTotal = 0;
const openWs = () => Array.from(sockets.values()).reduce((n, s) => n + s.size, 0);
const openSrcs = () => Array.from(sockets.entries()).filter(([, s]) => s.size).map(([k]) => k).sort();

const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const send = (type, body, code) => { res.writeHead(code || 200, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(body); };
    if (u.pathname === BASE || u.pathname === BASE + 'index.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'stream_center.html')));
    if (u.pathname === '/stream.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'player.html')));
    if (u.pathname === '/video-stream.js' || u.pathname === '/video-rtc.js')
        return send('application/javascript', fs.readFileSync(path.join(WEB, u.pathname)));
    if (u.pathname === BASE + 'state')
        return send('application/json', JSON.stringify({ hosts: HOSTS, active: HOSTS.map(h => h.id), webrtc: false, quality: [], quality_mjpeg: true, printers: [] }));
    if (u.pathname === BASE + 'bambu') {
        mjpegOpen++; mjpegTotal++;
        res.writeHead(200, { 'Content-Type': 'multipart/x-mixed-replace; boundary=frame', 'Cache-Control': 'no-store' });
        // A 1x1 GIF per part is enough for an <img> to treat it as a stream of frames.
        const gif = Buffer.from('R0lGODlhAQABAIAAAAUEBAAAACwAAAAAAQABAAACAkQBADs=', 'base64');
        const t = setInterval(() => {
            res.write('--frame\r\nContent-Type: image/gif\r\nContent-Length: ' + gif.length + '\r\n\r\n');
            res.write(gif); res.write('\r\n');
        }, 100);
        res.on('close', () => { clearInterval(t); mjpegOpen--; });
        return;
    }
    send('application/json', '{}', 404);
});
server.on('upgrade', (req, socket) => {
    const u = new URL(req.url, 'http://x');
    if (u.pathname !== '/api/ws') { socket.destroy(); return; }
    const accept = crypto.createHash('sha1').update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ' + accept + '\r\n\r\n');
    const src = u.searchParams.get('src') || '';
    if (!sockets.has(src)) sockets.set(src, new Set());
    sockets.get(src).add(socket); wsTotal++;
    // A real server answers the browser's close frame and closes the TCP connection; without that
    // the browser sits in CLOSING and the mock would count the socket as open.
    let pending = Buffer.alloc(0);
    socket.on('data', chunk => {
        pending = Buffer.concat([pending, chunk]);
        for (;;) {
            if (pending.length < 2) return;
            let len = pending[1] & 0x7f, off = 2;
            if (len === 126) { if (pending.length < 4) return; len = pending.readUInt16BE(2); off = 4; }
            else if (len === 127) { if (pending.length < 10) return; len = Number(pending.readBigUInt64BE(2)); off = 10; }
            if (pending[1] & 0x80) off += 4;
            if (pending.length < off + len) return;
            const opcode = pending[0] & 0x0f;
            pending = pending.subarray(off + len);
            if (opcode === 8) { socket.end(Buffer.from([0x88, 0x00])); return; }
        }
    });
    socket.on('error', () => {});
    socket.on('close', () => sockets.get(src).delete(socket));
});

// ---- a tiny DevTools client --------------------------------------------------------------------
class Cdp {
    constructor(url) { this.id = 0; this.cb = new Map(); this.ws = new WebSocket(url); }
    open() { return new Promise((ok, bad) => { this.ws.onopen = ok; this.ws.onerror = bad; this.ws.onmessage = m => {
        const d = JSON.parse(m.data); if (d.id && this.cb.has(d.id)) { this.cb.get(d.id)(d); this.cb.delete(d.id); } }; }); }
    send(method, params, sessionId) {
        return new Promise(ok => { const id = ++this.id; this.cb.set(id, ok); this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId })); });
    }
}

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const port = server.address().port;
    const dbgPort = 9300 + Math.floor(Math.random() * 400);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'camvis-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--autoplay-policy=no-user-gesture-required',
        '--disable-background-timer-throttling', 'about:blank'], { stdio: 'ignore' });
    try {
        let ver;
        for (let i = 0; i < 50 && !ver; i++) { try { ver = await (await fetch('http://127.0.0.1:' + dbgPort + '/json/version')).json(); } catch (e) { await sleep(200); } }
        const cdp = new Cdp(ver.webSocketDebuggerUrl); await cdp.open();
        const { result: { targetId } } = await cdp.send('Target.createTarget', { url: 'about:blank' });
        const { result: { sessionId } } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
        const S = (m, p) => cdp.send(m, p, sessionId);
        const evalJs = async expr => (await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result.result.value;
        await S('Page.enable'); await S('Runtime.enable');
        await S('Emulation.setDeviceMetricsOverride', { width: 390, height: 700, deviceScaleFactor: 1, mobile: true });

        // ---- 1. the wall -----------------------------------------------------------------------
        await S('Page.navigate', { url: 'http://127.0.0.1:' + port + BASE });
        await sleep(2500);
        const tileInfo = await evalJs(`JSON.stringify({cells: document.querySelectorAll('#grid .cell').length,
            live: document.querySelectorAll('#grid .cell iframe, #grid .cell img').length,
            vh: innerHeight, gridScroll: document.getElementById('grid').scrollHeight})`);
        const ti = JSON.parse(tileInfo);
        console.log('# wall: ' + tileInfo);
        check('the wall built all 7 tiles', ti.cells === 7, ti.cells);
        const startedWs = openWs(), startedMjpeg = mjpegOpen;
        check('only the tiles in view opened a stream (not all 7)', startedWs + startedMjpeg > 0 && startedWs + startedMjpeg < 7, startedWs + '+' + startedMjpeg);
        const initiallyOpen = openSrcs();

        // scroll to the bottom: the first tiles go out of view, the last (Bambu MJPEG) comes in
        const scrollTo = y => evalJs(`(function(){ var g = document.getElementById('grid');
            var el = g.scrollHeight > g.clientHeight ? g : document.scrollingElement; el.scrollTo(0, ${y}); return 1; })()`);
        await scrollTo(1e6);
        await sleep(600);
        check('right after scrolling, the tiles that left are still up (debounced)', initiallyOpen.every(s => openSrcs().includes(s)), openSrcs().join());
        await sleep(2600);
        const afterScroll = openSrcs();
        check('after the debounce the tiles scrolled out of view are closed', initiallyOpen.every(s => !afterScroll.includes(s)), afterScroll.join());
        check('the tiles scrolled into view are open (WebSocket or MJPEG)', openWs() + mjpegOpen > 0 && openWs() + mjpegOpen < 7, openWs() + '+' + mjpegOpen);
        check('the Bambu MJPEG relay is pulled while its tile is on screen', mjpegOpen === 1, mjpegOpen);

        // scroll back to the top: the MJPEG tile leaves (its connection closes), the first tiles return
        await scrollTo(0);
        await sleep(3000);
        check('scrolled back: the Bambu MJPEG connection is closed', mjpegOpen === 0, mjpegOpen);
        check('scrolled back: the first tiles stream again', initiallyOpen.every(s => openSrcs().includes(s)), openSrcs().join());

        // ---- 2. the page is hidden (background tab, locked screen) ------------------------------
        // Headless pages are always "visible", so the page's own signal is faked at its source:
        // document.hidden / visibilityState, then the event the browser would have fired.
        const hide = on => evalJs(`(function(on){
            Object.defineProperty(document, 'hidden', { configurable: true, get: function() { return on; } });
            Object.defineProperty(document, 'visibilityState', { configurable: true, get: function() { return on ? 'hidden' : 'visible'; } });
            document.dispatchEvent(new Event('visibilitychange')); return 1; })(${on})`);
        const before = openWs() + mjpegOpen;
        await hide(true);
        await sleep(400);
        check('hidden page: every stream is closed at once', openWs() === 0 && mjpegOpen === 0, openWs() + '+' + mjpegOpen + ' (was ' + before + ')');
        await hide(false);
        await sleep(2500);
        check('visible again: the streams come back', openWs() + mjpegOpen === before, openWs() + '+' + mjpegOpen + ' (was ' + before + ')');

        // ---- 3. the host (the app) says it is paused: window.__edgeActive(false) ----------------
        await evalJs('window.__edgeActive(false); 1');
        await sleep(400);
        check('__edgeActive(false): every stream is closed', openWs() === 0 && mjpegOpen === 0, openWs() + '+' + mjpegOpen);
        await evalJs('window.__edgeActive(true); 1');
        await sleep(2500);
        check('__edgeActive(true): the streams come back', openWs() + mjpegOpen === before, openWs() + '+' + mjpegOpen);

        // ---- 4. the player on its own (the app's printer screen frames it directly) -------------
        const p2 = (await cdp.send('Target.createTarget', { url: 'about:blank' })).result.targetId;
        const s2 = (await cdp.send('Target.attachToTarget', { targetId: p2, flatten: true })).result.sessionId;
        await cdp.send('Page.enable', {}, s2); await cdp.send('Runtime.enable', {}, s2);
        const ev2 = async expr => (await cdp.send('Runtime.evaluate', { expression: expr, returnByValue: true }, s2)).result.result.value;
        await evalJs('window.__edgeActive(false); 1'); await sleep(400); // the wall's own tiles out of the way
        await cdp.send('Page.navigate', { url: 'http://127.0.0.1:' + port + '/stream.html?src=solo&mode=mse' }, s2);
        await sleep(1500);
        const solo = () => (sockets.get('solo') || new Set()).size;
        check('the stand-alone player connects', solo() === 1, solo());
        const hide2 = on => ev2(`(function(on){
            Object.defineProperty(document, 'hidden', { configurable: true, get: function() { return on; } });
            document.dispatchEvent(new Event('visibilitychange')); return 1; })(${on})`);
        await hide2(true); await sleep(400);
        check('the stand-alone player closes its WebSocket when hidden', solo() === 0, solo());
        await hide2(false); await sleep(1500);
        check('the stand-alone player reconnects when shown, once', solo() === 1, solo());
        await hide2(true); await hide2(false); await hide2(true); await sleep(400);
        check('a flurry of hide / show leaves it closed, not leaking sockets', solo() === 0, solo());
        await hide2(false); await sleep(1500);
        check('and then exactly one socket again', solo() === 1, solo());

        console.log('# totals: ' + wsTotal + ' WebSockets, ' + mjpegTotal + ' MJPEG responses opened over the run');
    } finally {
        br.kill();
        await sleep(500);
        try { fs.rmSync(profile, { recursive: true, force: true }); } catch (e) {}
        server.close();
        if (server.closeAllConnections) server.closeAllConnections();
    }
    console.log(passed + ' passed, ' + failures + ' failed');
    return failures ? 1 : 0;
}
main().then(c => process.exit(c), e => { console.error(e); process.exit(2); });
