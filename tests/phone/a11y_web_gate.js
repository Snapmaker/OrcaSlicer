// Gate: accessibility basics of the hub web pages (resources/web/orca/stream_center.html and hub.html).
//
// A mock hub (plain Node http, no dependencies, loopback only) serves the real shipped pages and the
// handful of JSON endpoints the Streams and Devices views read. A headless Edge / Chrome is driven over
// the DevTools protocol (Node 22's built-in WebSocket). No printer, no real hub, no camera, and nothing
// touches the desktop: the "keyboard" is Input.dispatchKeyEvent into the headless page.
//
// It checks, on the real pages:
//   * the pages load with no uncaught exception
//   * camera chips: role=button, aria-pressed, a tick when selected, Enter / Space toggle, focus kept
//   * page tabs: keyboard reachable, aria-current follows the view
//   * the expand control is named "Expand camera" and works from the keyboard; tile frames have a title
//   * Devices: Pause / Resume / Stop name their printer, a dimmed one is aria-disabled, Enter presses it,
//     focus survives the 5 s redraw, the result is announced; event severity is a word and a mark
//   * #note is a polite live region
//   * hub.html: QR canvases have a text alternative, "Copy link" is a real <button>, span.btn are keyboard buttons
//   * contrast: every text/background pair that the pages use is >= 4.5:1 in dark, light, and both with
//     prefers-contrast: more (computed from the page's own resolved colours; the table is printed)
//   * prefers-reduced-motion: the infinite flash, transitions and smooth scroll are off
//
// Run: node tests/phone/a11y_web_gate.js        (skips, exit 0, when no Chromium is found)
// WEB_DIR=<dir> points it at another copy of the pages (to show it failing on the old ones).
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

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

// ---- colour maths -------------------------------------------------------------------------------
function parseColor(c) {
    c = String(c).trim();
    let m = /^#([0-9a-f]{3})$/i.exec(c);
    if (m) return m[1].split('').map(x => parseInt(x + x, 16));
    m = /^#([0-9a-f]{6})$/i.exec(c);
    if (m) return [0, 2, 4].map(i => parseInt(m[1].substr(i, 2), 16));
    m = /^rgba?\(([^)]+)\)/.exec(c);
    if (m) return m[1].split(/[ ,\/]+/).filter(Boolean).slice(0, 3).map(Number);
    throw new Error('cannot parse colour ' + c);
}
function lum(rgb) {
    const [r, g, b] = rgb.map(v => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); });
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}
function ratio(a, b) {
    const la = lum(parseColor(a)), lb = lum(parseColor(b));
    return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

// ---- the mock hub -------------------------------------------------------------------------------
const HOSTS = [1, 2, 3].map(i => ({ id: 'cam' + i, alias: 'Cam ' + i, rkind: 'go2rtc', rname: 's' + i, rurl: '' }));
const PRINTERS = [
    { id: 'sm:U1-0042', kind: 'snapmaker', name: 'Mock U1', model: 'U1', online: true, ip: '10.0.0.50', print_status: 'printing', printing: true,
      percent: 42, left_time_s: 3600, can_pause: true, can_resume: false, can_stop: true, bed_temp: 60, bed_target: 60, nozzles: [{ temp: 210, target: 210 }], added_by: 'manual' },
    { id: 'bbl:X1C', kind: 'bambu', name: 'Mock Bambu', model: 'X1C', online: true, ip: '10.0.0.51', print_status: 'paused', printing: false,
      can_pause: false, can_resume: true, can_stop: true,
      print_error: { code: '0300_4000', message: 'Filament ran out', actions: [{ verb: 'resume_error', label: 'Resume', remote_safe: true }, { verb: 'stop_error', label: 'Stop print', remote_safe: false }] } },
];
const EVENTS = [
    { id: 1, severity: 'info', title: 'Print started', text: 'Mock U1', time: Date.now() - 600000 },
    { id: 2, severity: 'warning', title: 'Filament low', text: 'Mock Bambu', time: Date.now() - 300000 },
    { id: 3, severity: 'error', title: 'Print failed', text: 'Mock U1', time: Date.now() - 60000 },
];
const controlPosts = [];
const hubPosts = [];
const HUB_INFO = { phone: true, url: 'http://192.168.1.5:13640/r/' + TOKEN + '/', video: null, remote: {
    on: true, access: { state: 'serving', message: '' }, url: 'https://pc.example.ts.net/r/' + TOKEN + '/', allowed_logins: ['a@example.com'], funnel_command: '' } };

const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const send = (type, body, code) => { res.writeHead(code || 200, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(body); };
    const json = (o, code) => send('application/json', JSON.stringify(o), code);
    let body = '';
    req.on('data', d => body += d);
    req.on('end', () => {
        // --- the Stream / Devices page, as a phone gets it
        if (u.pathname === BASE || u.pathname === BASE + 'index.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'stream_center.html')));
        if (u.pathname === '/stream.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'player.html')));
        if (u.pathname === '/video-stream.js' || u.pathname === '/video-rtc.js') return send('application/javascript', fs.readFileSync(path.join(WEB, u.pathname.slice(1))));
        if (u.pathname === '/pc.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'stream_center.html')));
        if (u.pathname === BASE + 'qrcode.js') return send('application/javascript', fs.readFileSync(path.join(WEB, 'qrcode.js')));
        if (u.pathname === BASE + 'state') return json({ hosts: HOSTS, active: ['cam1', 'cam2'], webrtc: false, quality: [], quality_mjpeg: true, printers: [] });
        if (u.pathname === BASE + 'api/instances') return json({ instances: [{ id: 1, index: 1, title: 'Mock slicer', hidden: false }] });
        if (u.pathname === BASE + 'events') return json({ last_id: 3, events: EVENTS });
        if (u.pathname === BASE + 'i/1/api/printers') return json({ printers: PRINTERS });
        if (u.pathname === BASE + 'i/1/api/snapmaker/devices') return json({ connect: { devices: [] } });
        const ctl = /^\/r\/abc123\/i\/1\/api\/printers\/(.+)\/control$/.exec(u.pathname);
        if (ctl && req.method === 'POST') { controlPosts.push({ id: decodeURIComponent(ctl[1]), body }); return json({ job: 7 }); }
        if (u.pathname === BASE + 'i/1/api/jobs/7') return json({ state: 'done', text: 'Paused.' });
        // --- the hub's own page, as the PC's browser gets it
        if (u.pathname === '/' || u.pathname === '/hub.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'hub.html'), 'utf8').replace('__HUB_SECRET__', 'test-secret'));
        if (u.pathname === '/hub/qrcode.js') return send('application/javascript', fs.readFileSync(path.join(WEB, 'qrcode.js')));
        if (u.pathname === '/hub/info') return json(HUB_INFO);
        if (u.pathname === '/hub/instances') return json({ instances: [] });
        if (u.pathname.indexOf('/hub/') === 0 && req.method === 'POST') { hubPosts.push(u.pathname + u.search); return json(HUB_INFO); }
        if (u.pathname.indexOf('/hub/') === 0) return json({});
        send('application/json', '{}', 404);
    });
});

// ---- a tiny DevTools client ---------------------------------------------------------------------
class Cdp {
    constructor(url) { this.id = 0; this.cb = new Map(); this.listeners = []; this.ws = new WebSocket(url); }
    open() { return new Promise((ok, bad) => { this.ws.onopen = ok; this.ws.onerror = bad; this.ws.onmessage = m => {
        const d = JSON.parse(m.data);
        if (d.id && this.cb.has(d.id)) { this.cb.get(d.id)(d); this.cb.delete(d.id); }
        else if (d.method) this.listeners.forEach(l => l(d)); }; }); }
    send(method, params, sessionId) {
        return new Promise(ok => { const id = ++this.id; this.cb.set(id, ok); this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId })); });
    }
}

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const port = server.address().port;
    const origin = 'http://127.0.0.1:' + port;
    const dbgPort = 9300 + Math.floor(Math.random() * 400);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'a11yweb-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', 'about:blank'], { stdio: 'ignore' });
    try {
        let ver;
        for (let i = 0; i < 50 && !ver; i++) { try { ver = await (await fetch('http://127.0.0.1:' + dbgPort + '/json/version')).json(); } catch (e) { await sleep(200); } }
        const cdp = new Cdp(ver.webSocketDebuggerUrl); await cdp.open();
        const { result: { targetId } } = await cdp.send('Target.createTarget', { url: 'about:blank' });
        const { result: { sessionId } } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
        const S = (m, p) => cdp.send(m, p, sessionId);
        const evalJs = async expr => {
            const r = (await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true })).result;
            if (r.exceptionDetails) throw new Error('page eval failed: ' + (r.exceptionDetails.exception && r.exceptionDetails.exception.description || r.exceptionDetails.text));
            return r.result.value;
        };
        const errors = [];
        cdp.listeners.push(d => {
            if (d.sessionId !== sessionId) return;
            if (d.method === 'Runtime.exceptionThrown') errors.push((d.params.exceptionDetails.exception && d.params.exceptionDetails.exception.description) || d.params.exceptionDetails.text);
            if (d.method === 'Runtime.consoleAPICalled' && d.params.type === 'error') errors.push('console.error: ' + d.params.args.map(a => a.value || a.description).join(' '));
            if (d.method === 'Page.javascriptDialogOpening') S('Page.handleJavaScriptDialog', { accept: false });
        });
        await S('Page.enable'); await S('Runtime.enable');
        await S('Emulation.setDeviceMetricsOverride', { width: 390, height: 844, deviceScaleFactor: 1, mobile: true });
        const media = features => S('Emulation.setEmulatedMedia', { features });
        const key = async (k, code, vk, text) => {
            await S('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code, windowsVirtualKeyCode: vk, text });
            await S('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code, windowsVirtualKeyCode: vk });
        };
        const ENTER = () => key('Enter', 'Enter', 13, '\r');
        const SPACE = () => key(' ', 'Space', 32, ' ');
        const ESC = () => key('Escape', 'Escape', 27);
        const focus = sel => evalJs(`(function(){ var e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.focus(); return document.activeElement === e; })()`);
        const load = async url => { await S('Page.navigate', { url }); await sleep(2200); };

        // ============================================================ 1. the Streams view
        await load(origin + BASE + '?theme=dark');
        check('Streams page: no uncaught exception on load', errors.length === 0, errors.join(' | '));
        check('the helper is attached: every chip name is a keyboard button',
            await evalJs(`(function(){ var n = document.querySelectorAll('#chips .chip > [data-fk$=":name"]');
                return n.length === 3 && Array.prototype.every.call(n, function(e){ return e.getAttribute('role') === 'button' && e.tabIndex === 0; }); })()`));
        check('chips: aria-pressed follows the selection (2 of 3 pressed)',
            await evalJs(`JSON.stringify(Array.prototype.map.call(document.querySelectorAll('#chips .chip > [data-fk$=":name"]'), function(e){ return e.getAttribute('aria-pressed'); }))`) === '["true","true","false"]');
        check('a selected chip shows a tick, an unselected one does not (state is not colour alone)',
            await evalJs(`(function(){ var c = document.querySelectorAll('#chips .chip'); return !!c[0].querySelector('.tick') && !c[2].querySelector('.tick'); })()`));
        check('the tick is hidden from assistive tech (the pressed state says it)', await evalJs(`document.querySelector('#chips .tick').getAttribute('aria-hidden') === 'true'`));
        check('the Add and Phone chips and the four page tabs are keyboard buttons',
            await evalJs(`['addchip','phonechip'].every(function(i){ var e = document.getElementById(i); return e.getAttribute('role') === 'button' && e.tabIndex === 0; })
                && document.querySelectorAll('#tabs .tab[role=button][tabindex="0"]').length === 4`));
        check('#note is a polite live region', await evalJs(`(function(n){ return n.getAttribute('aria-live') === 'polite' && n.getAttribute('role') === 'status'; })(document.getElementById('note'))`));
        check('the grid size selects have names', await evalJs(`document.getElementById('cols').getAttribute('aria-label') === 'Grid columns' && document.getElementById('rows').getAttribute('aria-label') === 'Grid rows'`));

        check('Tab order: the third chip can take the focus', await focus('#chips [data-fk="chip:cam3:name"]'));
        await ENTER(); await sleep(300);
        check('Enter on a chip selects it (aria-pressed true, tick appears)',
            await evalJs(`(function(e){ return e.getAttribute('aria-pressed') === 'true' && !!e.querySelector('.tick'); })(document.querySelector('#chips [data-fk="chip:cam3:name"]'))`));
        check('the keyboard stays on that chip after the redraw',
            await evalJs(`document.activeElement && document.activeElement.getAttribute('data-fk') === 'chip:cam3:name'`));
        await SPACE(); await sleep(300);
        check('Space on a chip clears it again', await evalJs(`document.querySelector('#chips [data-fk="chip:cam3:name"]').getAttribute('aria-pressed') === 'false'`));

        // tiles: names, frame titles, expand
        check('every camera tile frame has a title', await evalJs(`(function(){ var f = document.querySelectorAll('#grid .cell iframe'); return f.length >= 1 && Array.prototype.every.call(f, function(x){ return /^Live camera: Cam \\d$/.test(x.title); }); })()`),
            await evalJs(`JSON.stringify(Array.prototype.map.call(document.querySelectorAll('#grid .cell iframe'), function(x){ return x.title; }))`));
        check('each tile is a labelled group', await evalJs(`(function(){ var c = document.querySelectorAll('#grid .cell[data-id]'); return c.length >= 1 && Array.prototype.every.call(c, function(x){ return x.getAttribute('role') === 'group' && /^Cam \\d$/.test(x.getAttribute('aria-label')); }); })()`));
        check('the expand control is named "Expand camera" (not the glyph)', await evalJs(`(function(e){ return e.getAttribute('role') === 'button' && e.getAttribute('aria-label') === 'Expand camera' && e.getAttribute('aria-expanded') === 'false'; })(document.querySelector('#grid .cell .expand'))`));
        check('Expand can take the focus', await focus('#grid .cell .expand'));
        await ENTER(); await sleep(300);
        check('Enter on Expand fills the screen and renames the control',
            await evalJs(`(function(c){ var e = c && c.querySelector('.expand'); return !!c && e.getAttribute('aria-label') === 'Close expanded camera' && e.getAttribute('aria-expanded') === 'true'; })(document.querySelector('#grid .cell.expanded'))`));
        await ESC(); await sleep(300);
        check('Escape closes it and the name goes back', await evalJs(`!document.querySelector('#grid .cell.expanded') && document.querySelector('#grid .cell .expand').getAttribute('aria-label') === 'Expand camera'`));

        // ============================================================ 2. the Devices view
        check('Devices tab can take the focus', await focus('#tabs .tab[data-view="devices"]'));
        await ENTER(); await sleep(1800);
        check('Enter on the Devices tab opens it and aria-current follows',
            await evalJs(`document.getElementById('devices').style.display === 'block' && document.querySelector('#tabs .tab[data-view="devices"]').getAttribute('aria-current') === 'page' && !document.querySelector('#tabs .tab[data-view="streams"]').hasAttribute('aria-current')`));
        const names = JSON.parse(await evalJs(`JSON.stringify(Array.prototype.map.call(document.querySelectorAll('#devices .printer .acts [role=button]'), function(e){ return e.getAttribute('aria-label') + '|' + (e.getAttribute('aria-disabled') || ''); }))`));
        console.log('# card buttons: ' + names.join(', '));
        check('Pause / Resume / Stop name their printer (three cards, three different Stops)',
            names.indexOf('Pause Mock U1|') >= 0 && names.indexOf('Resume Mock U1|true') >= 0 && names.indexOf('Stop Mock U1|') >= 0 && names.indexOf('Stop Mock Bambu|') >= 0);
        check('a button the printer says no to is aria-disabled, not silently dead', names.indexOf('Pause Mock Bambu|true') >= 0 && names.indexOf('Resume Mock Bambu|') >= 0);
        check('the error action buttons are named and the unsafe one is aria-disabled',
            await evalJs(`(function(){ var r = document.querySelector('[data-fk="err:bbl:X1C:resume_error"]'), s = document.querySelector('[data-fk="err:bbl:X1C:stop_error"]'); return !!r && !r.hasAttribute('aria-disabled') && !!s && s.getAttribute('aria-disabled') === 'true' && /Mock Bambu/.test(r.getAttribute('aria-label')); })()`));
        check('event severity is a word for assistive tech and a mark for the eye',
            await evalJs(`(function(){ var rows = document.querySelectorAll('.evbox .ev'); var txt = Array.prototype.map.call(rows, function(r){ return r.textContent; }).join('|');
                var err = document.querySelector('.evbox .ev.err'), warn = document.querySelector('.evbox .ev.warn');
                return /Error: Print failed/.test(txt) && /Warning: Filament low/.test(txt) && err.querySelector('.dot').textContent === '\\u2715' && warn.querySelector('.dot').textContent === '\\u25B2' && err.querySelector('.dot').getAttribute('aria-hidden') === 'true'; })()`));

        check('Pause can take the focus', await focus('[data-fk="ctl:sm:U1-0042:pause"]'));
        await ENTER(); await sleep(2800);
        check('Enter on Pause sends the command to the printer', controlPosts.length === 1 && controlPosts[0].id === 'sm:U1-0042' && /action=pause/.test(controlPosts[0].body), JSON.stringify(controlPosts));
        check('the result is announced on the live region', await evalJs(`document.getElementById('announce').textContent === 'Paused.'`), await evalJs(`document.getElementById('announce').textContent`));
        check('the status line under the card is a live region', await evalJs(`!!document.querySelector('#devices .printer .ctlline[aria-live="polite"]')`));
        // the list redraws every ~5 s: put the focus on Stop, wait for a redraw, and it must still be there
        await focus('[data-fk="ctl:sm:U1-0042:stop"]');
        await evalJs(`window.__marker = document.querySelector('#devices .printer'); 1`);
        await sleep(6000);
        check('the Devices list was redrawn meanwhile', await evalJs(`window.__marker !== document.querySelector('#devices .printer')`));
        check('a redraw keeps the keyboard on the same button', await evalJs(`document.activeElement && document.activeElement.getAttribute('data-fk') === 'ctl:sm:U1-0042:stop'`),
            await evalJs(`document.activeElement && (document.activeElement.getAttribute('data-fk') || document.activeElement.tagName)`));
        await ENTER(); await sleep(400);
        check('Stop asks first: a labelled dialog opens with the focus on the safe answer',
            await evalJs(`(function(s){ return s.style.display === 'flex' && s.getAttribute('role') === 'dialog' && s.getAttribute('aria-label') === 'Stop the print' && document.activeElement.textContent === 'Keep printing'; })(document.getElementById('ctlsheet'))`),
            await evalJs(`document.activeElement.textContent`));
        await ENTER(); await sleep(300);
        check('Enter on "Keep printing" closes it without stopping', await evalJs(`document.getElementById('ctlsheet').style.display === 'none'`) && controlPosts.length === 1, controlPosts.length);
        check('the notifications control, when offered, is a keyboard button', await evalJs(`(function(b){ return !b || (b.getAttribute('role') === 'button' && b.tabIndex === 0); })(document.querySelector('.pushbox [data-fk="push"]'))`));

        // ============================================================ 2b. the PC's own Streams page (dialogs)
        // The slicer's Stream tab (not the phone): Add and Phone chips are there, and open dialogs.
        errors.length = 0;
        await load(origin + '/pc.html?theme=dark');
        check('PC Streams page: no uncaught exception on load', errors.length === 0, errors.join(' | '));
        check('Add chip opens the dialog from the keyboard', await focus('#addchip') && (await ENTER(), await sleep(300), await evalJs(`document.getElementById('modal-bg').style.display === 'flex' && document.getElementById('modal').getAttribute('aria-label') === 'Add printer or camera'`)));
        check('dialog fields are tied to their labels', await evalJs(`(function(){ var l = document.querySelectorAll('#modal label[for]'); return l.length >= 1 && Array.prototype.every.call(l, function(x){ return !!document.getElementById(x.htmlFor); }); })()`));
        check('dialog buttons are keyboard buttons', await evalJs(`document.querySelectorAll('#modal .buttons [role=button][tabindex="0"]').length === 2`));
        const chipsBefore = await evalJs(`document.querySelectorAll('#chips .chip').length`);
        await focus('#modal .buttons .cancel'); await ENTER(); await sleep(300);
        check('Enter on Cancel cancels (it does not also press OK)', await evalJs(`document.getElementById('modal-bg').style.display === 'none'`) && await evalJs(`document.querySelectorAll('#chips .chip').length`) === chipsBefore);
        await focus('#phonechip'); await ENTER(); await sleep(400);
        check('Phone chip opens its dialog from the keyboard, focus lands inside it, QR (if any) has a text alternative',
            await evalJs(`document.getElementById('modal-bg').style.display === 'flex' && document.getElementById('modal').contains(document.activeElement) && Array.prototype.every.call(document.querySelectorAll('#modal canvas'), function(c){ return c.getAttribute('role') === 'img' && /written out as text/.test(c.getAttribute('aria-label')); })`));
        await ESC(); await sleep(200);
        check('Escape closes the Phone dialog', await evalJs(`document.getElementById('modal-bg').style.display === 'none'`));

        // ============================================================ 3. contrast, four ways
        const SCEN = [['dark', 'dark', 'no-preference'], ['dark + Increase Contrast', 'dark', 'more'], ['light', 'light', 'no-preference'], ['light + Increase Contrast', 'light', 'more']];
        const TEXT = ['--fg3', '--fg4', '--fg5', '--mute', '--mute2', '--mute3', '--mute4', '--mute5', '--mute6', '--faint', '--faint2', '--err-fg', '--err2-fg', '--warn-fg', '--warn2-fg', '--ok-fg', '--note-fg', '--link-fg'];
        const SURF = ['--bg', '--panel', '--panel2', '--card', '--sunk', '--field'];
        console.log('# contrast (WCAG ratio; every pair must be >= 4.5)');
        const table = {};
        for (const [name, theme, pc] of SCEN) {
            await media([{ name: 'prefers-contrast', value: pc }]);
            await load(origin + BASE + '?theme=' + theme);
            const out = JSON.parse(await evalJs(`(function(){
                var cs = getComputedStyle(document.documentElement), v = function(n){ return cs.getPropertyValue(n).trim(); };
                var vars = {}; ${JSON.stringify(TEXT.concat(SURF))}.forEach(function(n){ vars[n] = v(n); });
                return JSON.stringify({ vars: vars });
            })()`));
            // elements whose selectors need the real ancestors: build them inside the page
            const el2 = JSON.parse(await evalJs(`(function(){
                var mk = function(html){ var d = document.createElement('div'); d.innerHTML = html; document.body.appendChild(d); var e = d.querySelector('[data-probe]'), s = getComputedStyle(e); var r = { fg: s.color, bg: s.backgroundColor }; d.remove(); return r; };
                return JSON.stringify({
                    tab: mk('<div id="tabs" style="display:block"><span class="tab active" data-probe>x</span></div>'),
                    btn: mk('<div class="view"><span class="btn" data-probe>x</span></div>'),
                    send: mk('<div id="send"><span class="btn" data-probe>x</span></div>'),
                    sendWarn: mk('<div id="send"><span class="btn warn" data-probe>x</span></div>'),
                    mod: mk('<div class="view"><div class="setup"><div class="procrow"><span class="btn mod" data-probe>x</span></div></div></div>'),
                    badge: mk('<div id="tabs"><span class="tab"><span class="badge" data-probe>3</span></span></div>'),
                    chip: mk('<span class="chip active" data-probe>x</span>'),
                    ok: mk('<div id="modal"><span class="ok" data-probe>x</span></div>')
                });
            })()`));
            const rows = [];
            for (const t of TEXT) for (const sf of SURF) {
                const r = ratio(out.vars[t], out.vars[sf]);
                rows.push([t + ' on ' + sf, r]);
            }
            const worst = rows.slice().sort((a, b) => a[1] - b[1]);
            table[name] = { worst: worst[0], elements: {} };
            const bad = rows.filter(r => r[1] < 4.5);
            check('[' + name + '] all ' + rows.length + ' text/surface pairs >= 4.5:1 (lowest ' + worst[0][0] + ' = ' + worst[0][1].toFixed(2) + ')', bad.length === 0, bad.map(b => b[0] + '=' + b[1].toFixed(2)).join('; '));
            for (const [k, label] of [['tab', 'active page tab'], ['btn', '.view .btn'], ['send', '#send .btn'], ['sendWarn', '#send .btn.warn'], ['mod', '.btn.mod'], ['badge', 'tab badge'], ['chip', 'selected chip'], ['ok', 'dialog OK']]) {
                const r = ratio(el2[k].fg, el2[k].bg);
                table[name].elements[label] = r;
                check('[' + name + '] ' + label + ' text on its own background = ' + r.toFixed(2) + ':1', r >= 4.5);
            }
        }

        // ============================================================ 4. reduced motion
        await media([{ name: 'prefers-contrast', value: 'no-preference' }, { name: 'prefers-reduced-motion', value: 'no-preference' }]);
        await load(origin + BASE + '?theme=dark');
        const motion = () => evalJs(`(function(){
            var p = document.getElementById('plate'), b = document.createElement('span'); b.className = 'btn small flash'; p.appendChild(b);
            var a = getComputedStyle(b).animationName; b.remove();
            var bar = document.createElement('div'); bar.className = 'card'; bar.innerHTML = '<div class="bar"><div></div></div>'; document.body.appendChild(bar);
            var t = getComputedStyle(bar.querySelector('.bar div')).transitionDuration; bar.remove();
            return JSON.stringify({ anim: a, trans: t });
        })()`);
        const m0 = JSON.parse(await motion());
        check('baseline (motion allowed): the Preview button flashes', m0.anim === 'pvflash', m0.anim);
        await media([{ name: 'prefers-reduced-motion', value: 'reduce' }]);
        const m1 = JSON.parse(await motion());
        check('Reduce Motion: the infinite flash is off', m1.anim === 'none', m1.anim);
        check('Reduce Motion: transitions are off', parseFloat(m1.trans) === 0, m1.trans);

        // the smooth scroll that a notification tap-through does
        const scrollBehaviour = async calm => {
            await media([{ name: 'prefers-reduced-motion', value: calm ? 'reduce' : 'no-preference' }]);
            await S('Page.navigate', { url: origin + BASE + '?theme=dark' }); await sleep(1500);
            await evalJs(`localStorage.setItem('edgeslicer_pending_printer', 'sm:U1-0042'); window.__sc = []; 1`);
            await S('Page.addScriptToEvaluateOnNewDocument', { source: `window.__sc = []; var o = Element.prototype.scrollIntoView; Element.prototype.scrollIntoView = function(a) { window.__sc.push(a && a.behavior || 'default'); return o.apply(this, arguments); };` });
            await S('Page.navigate', { url: origin + BASE + '?theme=dark' }); await sleep(1800);
            await evalJs(`document.querySelector('#tabs .tab[data-view="devices"]').click(); 1`); await sleep(2500);
            return evalJs(`JSON.stringify(window.__sc)`);
        };
        const sbNormal = await scrollBehaviour(false);
        const sbCalm = await scrollBehaviour(true);
        check('tap-through scroll: smooth when motion is allowed', /smooth/.test(sbNormal), sbNormal);
        check('tap-through scroll: instant under Reduce Motion', /auto/.test(sbCalm) && !/smooth/.test(sbCalm), sbCalm);
        await media([{ name: 'prefers-reduced-motion', value: 'no-preference' }]);

        // ============================================================ 5. the hub's own page
        errors.length = 0;
        await load(origin + '/');
        await sleep(1500);
        check('hub.html: no uncaught exception on load', errors.length === 0, errors.join(' | '));
        const qr = JSON.parse(await evalJs(`JSON.stringify(Array.prototype.map.call(document.querySelectorAll('canvas.qr'), function(c){ return [c.getAttribute('role'), c.getAttribute('aria-label')]; }))`));
        check('hub.html: both QR codes (Home, Away) have a text alternative', qr.length === 2 && qr.every(q => q[0] === 'img' && /^QR code for .+written out as text/.test(q[1])), JSON.stringify(qr));
        check('hub.html: "Copy link" is a real <button>', await evalJs(`(function(b){ return !!b && b.tagName === 'BUTTON' && b.type === 'button' && b.textContent === 'Copy link'; })(Array.prototype.filter.call(document.querySelectorAll('.btn'), function(e){ return e.textContent === 'Copy link'; })[0])`));
        check('hub.html: every span.btn is a keyboard button',
            await evalJs(`(function(){ var s = document.querySelectorAll('span.btn'); return s.length >= 5 && Array.prototype.every.call(s, function(e){ return e.getAttribute('role') === 'button' && e.tabIndex === 0; }); })()`),
            await evalJs(`Array.prototype.filter.call(document.querySelectorAll('span.btn'), function(e){ return e.getAttribute('role') !== 'button'; }).map(function(e){ return e.textContent; }).join(',')`));
        check('hub.html: the notification tabs are keyboard buttons with a pressed state', await evalJs(`(function(t){ return t.length === 4 && Array.prototype.every.call(t, function(e){ return e.getAttribute('role') === 'button' && e.hasAttribute('aria-pressed'); }); })(document.querySelectorAll('#notifytabs .tab'))`));
        check('hub.html: the remove-x has a name', await evalJs(`Array.prototype.some.call(document.querySelectorAll('.login .btn'), function(e){ return /^Remove a@example.com$/.test(e.getAttribute('aria-label')); })`));
        check('hub.html: Enter on a button presses it (Disable -> POST /hub/phone?on=0)', await focus('#phonebody .btn.warn') && (await ENTER(), await sleep(500), hubPosts.indexOf('/hub/phone?on=0') >= 0), hubPosts.join(','));
        const hubRatios = JSON.parse(await evalJs(`(function(){
            var mk = function(html){ var d = document.createElement('div'); d.innerHTML = html; document.querySelector('.card').appendChild(d); var e = d.firstChild, s = getComputedStyle(e); var r = { fg: s.color, bg: s.backgroundColor }; d.remove(); return r; };
            var card = getComputedStyle(document.querySelector('.card')).backgroundColor;
            return JSON.stringify({ btn: mk('<span class="btn">x</span>'), note: [getComputedStyle(document.querySelector('.note')).color, card], h2: [getComputedStyle(document.querySelector('.card h2')).color, card],
                src: [getComputedStyle(document.querySelector('.srcoffer')).color, getComputedStyle(document.body).backgroundColor] });
        })()`));
        check('hub.html: .btn white on green = ' + ratio(hubRatios.btn.fg, hubRatios.btn.bg).toFixed(2) + ':1', ratio(hubRatios.btn.fg, hubRatios.btn.bg) >= 4.5);
        check('hub.html: note text on the card = ' + ratio(hubRatios.note[0], hubRatios.note[1]).toFixed(2) + ':1', ratio(hubRatios.note[0], hubRatios.note[1]) >= 4.5);
        check('hub.html: card headings on the card = ' + ratio(hubRatios.h2[0], hubRatios.h2[1]).toFixed(2) + ':1', ratio(hubRatios.h2[0], hubRatios.h2[1]) >= 4.5);
        check('hub.html: source offer on the page = ' + ratio(hubRatios.src[0], hubRatios.src[1]).toFixed(2) + ':1', ratio(hubRatios.src[0], hubRatios.src[1]) >= 4.5);
        console.log('# table: ' + JSON.stringify(table));
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
