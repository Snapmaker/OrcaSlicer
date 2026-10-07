// Gate: the slicer's Printers tab page (resources/web/orca/monitor.html + printer_cards.js).
//
// A plain Node http server (loopback, no dependencies) serves the real shipped page. Before the page
// runs, a stand-in for the slicer is injected as window.wx - the script channel PrintersPanel.cpp
// listens on - and it answers the page's messages the way PrintersPanel does (window.__printers,
// __printerGroups, __printerControl, __printerJob) with mock rows in the RemoteAccess::api_printers
// schema. A headless Edge / Chrome is driven over the DevTools protocol (Node 22's built-in
// WebSocket). No slicer, no printer, no hub, and nothing touches the desktop: the "keyboard" is
// Input.dispatchKeyEvent into the headless page. "Open Device page" is hidden for now (owner, 2026-10-07).
//
// It checks: the page loads with no uncaught exception; every row type renders (Bambu with AMS,
// chamber and fans; Snapmaker U1 with toolheads; a Moonraker host; an error; an offline one); the
// printer picture and friendly model name; the job's plate picture (fetched once over the channel);
// temperatures, fans and filament each in one outlined group, fans on one line; every filament slot
// the same size, empty or not; an error can be dismissed and comes back with a new code; the
// status sort; the state counts; the buttons follow can_pause / can_resume / can_stop (a dimmed one
// keeps its reason); Pause from the keyboard sends the right message, follows the job and keeps the
// focus across redraws; Stop asks first (Escape / Keep printing send nothing) and then sends
// confirm=1; Open Device page names the printer; groups filter; a failed read keeps the cards and
// says why; printer-supplied text is never markup; polling stops while hidden and resumes at once
// when shown; the live theme switch; text contrast >= 4.5:1 in both themes.
//
// Run: node tests/phone/printers_monitor_gate.js     (skips, exit 0, when no Chromium is found)
// WEB_DIR=<dir> points it at another copy of the pages.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const WEB = process.env.WEB_DIR || path.join(__dirname, '..', '..', 'resources', 'web', 'orca');

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

let failures = 0, passed = 0, coverHits = 0;
function check(label, cond, extra) {
    if (cond) { passed++; console.log('ok   - ' + label); }
    else { failures++; console.error('FAIL - ' + label + (extra !== undefined ? '  (' + extra + ')' : '')); }
}
const sleep = ms => new Promise(r => setTimeout(r, ms));

// ---- mock rows, in the api_printers schema (RemoteAccess.cpp, RemoteControl::describe_bambu,
// SnapmakerLan::list_printers, RemoteControl::describe_hosts) ----------------------------------------
const ROWS = [
    { id: '01P00A111111111', kind: 'bambu', name: 'X1C Garage', model: 'O1C2', model_name: 'Bambu Lab H2C', model_short: 'H2C',
      picture: '/profiles/BBL/Bambu%20Lab%20H2C_cover.png', thumb_id: '20261007-101500-benchy', online: true, connected: true,
      status: 'RUNNING', print_status: 'RUNNING', printing: true, percent: 42, left_time_s: 3780, layer: 12, total_layers: 200,
      task: 'Benchy_plate_1', stage: 'Printing', ip: '192.168.1.30', bed_temp: 55, bed_target: 55,
      nozzles: [{ temp: 219.6, target: 220 }], can_pause: true, can_resume: false, can_stop: true, print_error: null,
      hms: { count: 0 },
      controls: { heaters: [{ id: 'bed', label: 'Bed', temp: 55, target: 55, settable: true }, { id: 'nozzle0', label: 'Nozzle', temp: 219.6, target: 220, settable: true },
                            { id: 'chamber', label: 'Chamber', temp: 31, target: 0, settable: false }],
                  fans: [{ id: 'part', label: 'Part cooling fan', percent: 60 }, { id: 'aux', label: 'Aux fan', percent: 0 }, { id: 'chamber', label: 'Chamber fan', percent: 30 }] },
      ams: [{ id: '0', side: '', humidity_pct: 22, trays: [
          { id: '0', exists: true, type: 'PLA', color: '#FF0000', remain: 80 }, { id: '1', exists: true, type: 'PETG', color: '#00AE42', remain: 5 },
          { id: '2', exists: false, type: '', color: '', remain: -1 }, { id: '3', exists: true, type: 'PLA Silk', color: '#F4EE2A', remain: 40 }] }],
      ext_spools: [{ ams_id: '254', side: '', exists: true, type: 'TPU', color: '#FFFFFF' }] },
    { id: '01P00A222222222', kind: 'bambu', name: 'P1S <img src=x onerror="window.__xss=1">', model: 'C12', online: true, connected: true,
      status: 'FAILED', print_status: 'FAILED', printing: false, percent: 10, task: 'Bracket', can_pause: false, can_resume: false, can_stop: true,
      print_error: { code: '0300400C', message: 'The build plate is not placed.', actions: [] }, hms: { count: 1, message: 'Plate missing' },
      controls: { heaters: [{ id: 'bed', label: 'Bed', temp: 24, target: 0 }], fans: [] }, ams: [], ext_spools: [] },
    { id: 'sm:U1-0042', kind: 'snapmaker', name: 'U1 Lab', model: 'Snapmaker U1', url: 'http://192.168.1.20', ip: '192.168.1.20', via: 'lan',
      online: true, printing: false, status: 'paused', print_status: 'paused', percent: 77, left_time_s: 600, layer: 80, total_layers: 100,
      task: 'gear.gcode', bed_temp: 60, bed_target: 60, nozzles: [{ temp: 200, target: 200 }, { temp: 25, target: 0 }, { temp: 25, target: 0 }, { temp: 25, target: 0 }],
      toolheads: [{ index: 0, type: 'PLA', color: '#3366FF', loaded: true }, { index: 1, type: 'PLA', color: '#FFFFFF', loaded: true },
                  { index: 2, type: '', color: '', loaded: false }, { index: 3, type: 'TPU', sub_type: 'NONE', color: '#000000', loaded: true }],
      can_pause: false, can_resume: true, can_stop: true, print_error: null },
    { id: 'ph:voron', kind: 'printhost', name: 'Voron 2.4', model: 'Voron', url: 'http://voron.local', ip: 'voron.local', online: true,
      status: 'standby', print_status: 'standby', printing: false, bed_temp: 22, bed_target: 0,
      nozzles: [{ temp: 23, target: 0 }], can_pause: false, can_resume: false, can_stop: false, print_error: null },
    { id: '01P00A333333333', kind: 'bambu', name: 'A1 Mini', model: 'N1', online: false, connected: false, status: '', printing: false,
      can_pause: false, can_resume: false, can_stop: false, print_error: null },
];
const GROUPS = [{ id: 'all', name: 'All printers', all: true, printers: [] },
                { id: 'multi-device', name: 'Multi-device', all: false, printers: ['01P00A111111111', 'sm:U1-0042'] }];

// The stand-in slicer, injected before the page's own scripts run.
// A 1x1 PNG, for the plate picture and the printer picture.
const PNG1 = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==';
const SHIM = `(function(){ var PNG1 = '${PNG1}';
    window.__sent = []; window.__rows = ${JSON.stringify(ROWS)}; window.__groups = ${JSON.stringify(GROUPS)};
    window.__fail = null; var nextJob = 100;
    function later(f) { setTimeout(f, 30); }
    window.wx = { postMessage: function(m) {
        window.__sent.push(m);
        if (m === 'printers_get') later(function() {
            if (window.__fail) window.__printers({ ok: false, error: window.__fail, printers: [], at: Date.now() });
            else window.__printers({ ok: true, printers: JSON.parse(JSON.stringify(window.__rows)), at: Date.now() });
        });
        else if (m === 'printers_groups_get') later(function() { window.__printerGroups(window.__groups); });
        else if (m.indexOf('printers_thumb:') === 0) {
            window.__thumbAsks = (window.__thumbAsks || 0) + 1;
            later(function() { window.__printerThumb({ id: m.slice(15), data: 'data:image/png;base64,' + PNG1 }); });
        }
        else if (m.indexOf('printers_control:') === 0) {
            var parts = m.split(':'), id = parts.slice(3).join(':'), job = nextJob++;
            later(function() { window.__printerControl({ id: id, action: parts[1], ok: true, job: job, dry_run: false }); });
        } else if (m.indexOf('printers_job:') === 0) {
            var n = +m.slice(13);
            window.__jobPolls = (window.__jobPolls || 0) + 1;
            later(function() { window.__printerJob({ id: n, state: window.__jobPolls < 2 ? 'running' : 'done', text: 'sending', error: '' }); });
        }
    } };
})();`;

const server = http.createServer((req, res) => {
    const u = new URL(req.url, 'http://x');
    const send = (type, body, code) => { res.writeHead(code || 200, { 'Content-Type': type, 'Cache-Control': 'no-store' }); res.end(body); };
    if (u.pathname === '/web/orca/monitor.html') return send('text/html; charset=utf-8', fs.readFileSync(path.join(WEB, 'monitor.html')));
    if (u.pathname === '/web/orca/printer_cards.js') return send('application/javascript', fs.readFileSync(path.join(WEB, 'printer_cards.js')));
    if (decodeURIComponent(u.pathname) === '/profiles/BBL/Bambu Lab H2C_cover.png') { coverHits++; return send('image/png', Buffer.from(PNG1, 'base64')); }
    send('text/plain', 'not found', 404);
});

class Cdp {
    constructor(url) { this.id = 0; this.cb = new Map(); this.events = []; this.ws = new WebSocket(url); }
    open() { return new Promise((ok, bad) => { this.ws.onopen = ok; this.ws.onerror = bad; this.ws.onmessage = m => {
        const d = JSON.parse(m.data);
        if (d.id && this.cb.has(d.id)) { this.cb.get(d.id)(d); this.cb.delete(d.id); }
        else if (d.method) this.events.push(d); }; }); }
    send(method, params, sessionId) {
        return new Promise(ok => { const id = ++this.id; this.cb.set(id, ok); this.ws.send(JSON.stringify({ id, method, params: params || {}, sessionId })); });
    }
}

// ---- colour maths ------------------------------------------------------------------------------
function parseColor(c) {
    const m = /^rgba?\(([^)]+)\)/.exec(String(c).trim());
    if (!m) return null;
    const p = m[1].split(',').map(s => parseFloat(s));
    return p.length >= 3 ? p.slice(0, 3) : null;
}
function lum(rgb) {
    const f = v => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * f(rgb[0]) + 0.7152 * f(rgb[1]) + 0.0722 * f(rgb[2]);
}
function contrast(a, b) { const x = lum(a), y = lum(b); return (Math.max(x, y) + 0.05) / (Math.min(x, y) + 0.05); }

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const port = server.address().port;
    const dbgPort = 9700 + Math.floor(Math.random() * 200);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'printersmon-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--disable-background-timer-throttling', 'about:blank'], { stdio: 'ignore' });
    try {
        let ver;
        for (let i = 0; i < 50 && !ver; i++) { try { ver = await (await fetch('http://127.0.0.1:' + dbgPort + '/json/version')).json(); } catch (e) { await sleep(200); } }
        const cdp = new Cdp(ver.webSocketDebuggerUrl); await cdp.open();
        const { result: { targetId } } = await cdp.send('Target.createTarget', { url: 'about:blank' });
        const { result: { sessionId } } = await cdp.send('Target.attachToTarget', { targetId, flatten: true });
        const S = (m, p) => cdp.send(m, p, sessionId);
        const evalJs = async expr => {
            const r = await S('Runtime.evaluate', { expression: expr, returnByValue: true, awaitPromise: true });
            if (r.result && r.result.exceptionDetails) throw new Error('eval failed: ' + JSON.stringify(r.result.exceptionDetails).slice(0, 300));
            return r.result.result.value;
        };
        const key = async k => {
            const code = { Enter: 13, Escape: 27, ' ': 32, Tab: 9 }[k];
            const text = k === 'Enter' ? '\r' : k === ' ' ? ' ' : undefined;
            await S('Input.dispatchKeyEvent', { type: 'keyDown', key: k, code: k === ' ' ? 'Space' : k, windowsVirtualKeyCode: code, text });
            await S('Input.dispatchKeyEvent', { type: 'keyUp', key: k, code: k === ' ' ? 'Space' : k, windowsVirtualKeyCode: code });
        };
        await S('Page.enable'); await S('Runtime.enable');
        await S('Emulation.setDeviceMetricsOverride', { width: 1280, height: 900, deviceScaleFactor: 1, mobile: false });
        await S('Page.addScriptToEvaluateOnNewDocument', { source: SHIM });

        // ---- 1. load --------------------------------------------------------------------------
        await S('Page.navigate', { url: 'http://127.0.0.1:' + port + '/web/orca/monitor.html?embed=1&theme=dark&tasks=1' });
        await sleep(1200);
        const exceptions = () => cdp.events.filter(e => e.method === 'Runtime.exceptionThrown');
        check('the page loads with no uncaught exception', exceptions().length === 0, JSON.stringify(exceptions().map(e => e.params.exceptionDetails.text)));
        check('it asked the slicer for the groups and the printers over the wx channel',
              (await evalJs('JSON.stringify(window.__sent)')).includes('printers_groups_get') && (await evalJs('window.__sent.indexOf("printers_get") >= 0')));

        // ---- 2. cards -------------------------------------------------------------------------
        const order = JSON.parse(await evalJs(`JSON.stringify([].map.call(document.querySelectorAll('#grid .pcard'), c => c.getAttribute('data-printer-id')))`));
        check('one card per row (5)', order.length === 5, order.length);
        check('status sort: error, paused, printing, idle, offline', JSON.stringify(order) === JSON.stringify(['01P00A222222222', 'sm:U1-0042', '01P00A111111111', 'ph:voron', '01P00A333333333']), order.join());
        const counts = await evalJs(`document.getElementById('counts').textContent`);
        check('state counts in the header', /1 printing/.test(counts) && /1 paused/.test(counts) && /1 with an error/.test(counts) && /1 idle/.test(counts) && /1 offline/.test(counts), counts);
        const card = id => `document.querySelector('.pcard[data-printer-id="${id}"]')`;
        const x1 = JSON.parse(await evalJs(`(function(c){ return JSON.stringify({
            pill: c.querySelector('.ppill').textContent, task: c.querySelector('.ptask').textContent,
            bar: c.querySelector('[role=progressbar]').getAttribute('aria-valuenow'), prog: c.querySelector('.pprog').textContent,
            temps: [].map.call(c.querySelectorAll('.ptemps .pitem'), x => x.textContent), fans: [].map.call(c.querySelectorAll('.pfans .pitem'), x => x.textContent),
            legends: [].map.call(c.querySelectorAll('fieldset.pgroup > legend'), x => x.textContent),
            fanH: c.querySelector('.pfans .pitems').getBoundingClientRect().height,
            sizes: [].map.call(c.querySelectorAll('.psw'), x => { const r = x.getBoundingClientRect(); return Math.round(r.width) + 'x' + Math.round(r.height); }),
            model: c.querySelector('.pmodel').textContent, icon: (c.querySelector('img.picon') || {}).naturalWidth,
            thumb: (c.querySelector('img.pthumb') || {}).src || '', open: !!c.querySelector('[data-fk^="open:"]'), sw: c.querySelectorAll('.psw').length,
            empty: c.querySelectorAll('.psw.vacant').length, low: c.querySelectorAll('.psw.low').length,
            swLabel: c.querySelector('.psw').getAttribute('aria-label'), bg: getComputedStyle(c.querySelector('.psw')).backgroundColor }); })(${card('01P00A111111111')})`));
        check('Bambu: state, job, progress bar 42', x1.pill === 'Printing' && x1.task === 'Benchy_plate_1' && x1.bar === '42', JSON.stringify(x1));
        check('Bambu: time left and layer', x1.prog === '42% \u00b7 1h 3m left \u00b7 layer 12/200', x1.prog);
        check('Bambu: temperatures in one group: bed, nozzle, chamber (sensor, no target)', JSON.stringify(x1.temps) === JSON.stringify(['Bed55\u00b0 / 55\u00b0', 'Nozzle220\u00b0 / 220\u00b0', 'Chamber31\u00b0']), x1.temps.join('|'));
        check('Bambu: fans in one group, short names Part / Aux / Chamber', JSON.stringify(x1.fans) === JSON.stringify(['Part60%', 'Aux0%', 'Chamber30%']), x1.fans.join('|'));
        check('Bambu: the fans fit one line', x1.fanH > 0 && x1.fanH < 24, x1.fanH);
        check('Bambu: Temperatures, Fans and Filament are outlined groups', JSON.stringify(x1.legends) === JSON.stringify(['Temperatures', 'Fans', 'Filament']), x1.legends.join('|'));
        check('Bambu: every filament slot is the same size, empty or not', x1.sizes.length === 5 && x1.sizes.every(z => z === x1.sizes[0]), x1.sizes.join(' '));
        check('Bambu: the model line says H2C (not O1C2) with the address', x1.model === 'H2C \u00b7 192.168.1.30', x1.model);
        check('Bambu: the printer picture from the vendor profile is shown', x1.icon === 1 && coverHits >= 1, x1.icon + ' / ' + coverHits);
        check('Bambu: the job\'s plate picture, asked for once over the channel', x1.thumb.indexOf('data:image/png;base64,') === 0 && (await evalJs('window.__thumbAsks')) === 1, x1.thumb.slice(0, 40));
        check('"Open Device page" is not shown', !x1.open);
        check('Bambu: 4 AMS slots + 1 external spool, the empty one dashed, the 5% one marked low', x1.sw === 5 && x1.empty === 1 && x1.low === 1, JSON.stringify(x1));
        check('Bambu: a swatch names slot, material, colour and what is left', x1.swLabel === 'AMS A slot 1: PLA #FF0000, 80% left' && x1.bg === 'rgb(255, 0, 0)', x1.swLabel + ' ' + x1.bg);
        const u1 = JSON.parse(await evalJs(`(function(c){ return JSON.stringify({ pill: c.querySelector('.ppill').textContent, sw: c.querySelectorAll('.psw').length,
            empty: c.querySelectorAll('.psw.vacant').length, prog: c.querySelector('.pprog').textContent, chips: c.querySelectorAll('.ptemps .pitem').length,
            model: c.querySelector('.pmodel').textContent }); })(${card('sm:U1-0042')})`));
        check('Snapmaker U1: paused, 4 toolheads (one empty), bed + 4 nozzles', u1.pill === 'Paused' && u1.sw === 4 && u1.empty === 1 && u1.chips === 5, JSON.stringify(u1));
        check('Snapmaker U1: model line', u1.model === 'Snapmaker U1 \u00b7 192.168.1.20', u1.model);
        check('Snapmaker U1: progress while paused', u1.prog === '77% \u00b7 10m left \u00b7 layer 80/100', u1.prog);
        const p1s = JSON.parse(await evalJs(`(function(c){ return JSON.stringify({ pill: c.querySelector('.ppill').textContent, err: (c.querySelector('.perrt') || {}).textContent,
            name: c.querySelector('.pname').textContent, imgs: c.querySelectorAll('img').length, xss: !!window.__xss }); })(${card('01P00A222222222')})`));
        check('error card: state and the printer\'s own message', p1s.pill === 'Error' && p1s.err === 'The build plate is not placed.', JSON.stringify(p1s));
        check('printer-supplied text is text, never markup', p1s.imgs === 0 && !p1s.xss && p1s.name.indexOf('<img src=x') > 0, JSON.stringify(p1s));
        const off = JSON.parse(await evalJs(`(function(c){ return JSON.stringify({ pill: c.querySelector('.ppill').textContent, chips: c.querySelectorAll('.pitem').length,
            dis: [].map.call(c.querySelectorAll('.pacts button[aria-disabled=true]'), b => b.title) }); })(${card('01P00A333333333')})`));
        check('offline card: no live values, buttons dimmed with the reason', off.pill === 'Offline' && off.chips === 0 && off.dis.length === 3 && off.dis[0] === 'This printer is offline.', JSON.stringify(off));
        const host = JSON.parse(await evalJs(`(function(c){ return JSON.stringify({ pill: c.querySelector('.ppill').textContent,
            pause: c.querySelector('[data-fk="act:ph:voron:pause"]').title }); })(${card('ph:voron')})`));
        check('Moonraker host: idle, Pause dimmed with the printer\'s state as the reason', host.pill === 'Idle' && /standby/.test(host.pause), JSON.stringify(host));

        // ---- 3. Pause from the keyboard ---------------------------------------------------------
        await evalJs(`window.__sent = []; document.querySelector('[data-fk="act:01P00A111111111:pause"]').focus(); 1`);
        await key('Enter');
        await sleep(150);
        check('Enter on Pause sends printers_control:pause:0:<id>', (await evalJs('JSON.stringify(window.__sent)')).includes('printers_control:pause:0:01P00A111111111'), await evalJs('JSON.stringify(window.__sent)'));
        await sleep(3000);
        const pz = JSON.parse(await evalJs(`JSON.stringify({ sent: window.__sent, line: (${card('01P00A111111111')}.querySelector('.pjobline') || {}).textContent,
            ann: document.getElementById('announce').textContent, fk: document.activeElement && document.activeElement.getAttribute('data-fk') })`));
        check('the job is followed until done and announced', pz.sent.some(m => m.indexOf('printers_job:') === 0) && pz.line === 'Paused.' && pz.ann === 'Paused.', JSON.stringify(pz));
        check('after the job the list is read again', pz.sent.filter(m => m === 'printers_get').length >= 1, pz.sent.join());
        check('the keyboard focus survives the redraws', pz.fk === 'act:01P00A111111111:pause', pz.fk);
        // A dimmed button does nothing.
        await evalJs(`window.__sent = []; document.querySelector('[data-fk="act:ph:voron:pause"]').click(); 1`);
        await sleep(100);
        check('a dimmed button sends nothing', (await evalJs('window.__sent.length')) === 0);

        // ---- 4. Stop asks first ----------------------------------------------------------------
        await evalJs(`window.__sent = []; document.querySelector('[data-fk="act:sm:U1-0042:stop"]').focus(); 1`);
        await key('Enter');
        await sleep(100);
        let dlg = JSON.parse(await evalJs(`JSON.stringify({ open: document.getElementById('scrim').classList.contains('open'), focus: document.activeElement.id,
            text: document.getElementById('dlgtext').textContent, modal: document.getElementById('dialog').getAttribute('aria-modal') })`));
        check('Stop opens a confirmation with the safe answer focused', dlg.open && dlg.focus === 'dlgkeep' && /U1 Lab/.test(dlg.text) && dlg.modal === 'true', JSON.stringify(dlg));
        await key('Escape');
        await sleep(100);
        dlg = JSON.parse(await evalJs(`JSON.stringify({ open: document.getElementById('scrim').classList.contains('open'), sent: window.__sent, fk: document.activeElement.getAttribute('data-fk') })`));
        check('Escape closes it, sends nothing and returns the focus to Stop', !dlg.open && dlg.sent.length === 0 && dlg.fk === 'act:sm:U1-0042:stop', JSON.stringify(dlg));
        await evalJs(`document.querySelector('[data-fk="act:sm:U1-0042:stop"]').click(); document.getElementById('dlgkeep').click(); 1`);
        await sleep(100);
        check('Keep printing sends nothing', (await evalJs('window.__sent.length')) === 0);
        await evalJs(`window.__jobPolls = 0; document.querySelector('[data-fk="act:sm:U1-0042:stop"]').click(); document.getElementById('dlgstop').click(); 1`);
        await sleep(150);
        check('Stop print sends printers_control:stop:1:<id> (confirmed)', (await evalJs('JSON.stringify(window.__sent)')).includes('printers_control:stop:1:sm:U1-0042'), await evalJs('JSON.stringify(window.__sent)'));

        // ---- 5. dismissing an error ------------------------------------------------------------
        const errShown = () => evalJs(`!!${card('01P00A222222222')}.querySelector('.perr')`);
        await evalJs(`document.querySelector('[data-fk="dismiss:01P00A222222222"]').focus(); 1`);
        const xName = await evalJs(`document.activeElement.getAttribute('aria-label')`);
        check('the error has a named dismiss button', /^Dismiss this error on P1S/.test(xName || ''), xName);
        await key('Enter');
        await sleep(100);
        check('dismissed: the banner is gone and that is announced', !(await errShown()) && /Error dismissed/.test(await evalJs(`document.getElementById('announce').textContent`)));
        check('the focus moved to a control of the same card', /^act:01P00A222222222:/.test(await evalJs(`document.activeElement.getAttribute('data-fk') || ''`)));
        await evalJs(`document.getElementById('refresh').click(); 1`);
        await sleep(200);
        check('it stays dismissed across reads while the code is the same', !(await errShown()));
        check('the dismissal is kept for the session', /0300400C/.test(await evalJs(`sessionStorage.getItem('edgeslicer_printers_dismissed') || ''`)));
        await evalJs(`window.__rows[1].print_error = { code: '0300400D', message: 'Another error.', actions: [] }; document.getElementById('refresh').click(); 1`);
        await sleep(200);
        check('a different error code shows again', await errShown());
        await evalJs(`window.__rows[1].print_error = { code: '0300400C', message: 'The build plate is not placed.', actions: [] }; 1`);

        // ---- Open Device page: hidden, but the page still shows a refusal -------------------------
        await evalJs(`window.__printerOpen({ id: '01P00A111111111', ok: false, error: 'Select a Bambu Lab printer preset' }); 1`);
        check('a refused open is shown', /Bambu Lab printer preset/.test(await evalJs(`document.getElementById('banner').textContent`)));
        await evalJs(`window.__sent = []; document.getElementById('tasks').click(); 1`);
        check('?tasks=1 offers the old Multi-device tasks window', (await evalJs('JSON.stringify(window.__sent)')) === JSON.stringify(['printers_tasks']));

        // ---- 6. groups -------------------------------------------------------------------------
        const gopts = await evalJs(`JSON.stringify([].map.call(document.querySelectorAll('#group option'), o => o.textContent))`);
        check('the group selector lists All printers and the seeded group', gopts === JSON.stringify(['All printers', 'Multi-device']), gopts);
        await evalJs(`(function(){ var s = document.getElementById('group'); s.value = 'multi-device'; s.dispatchEvent(new Event('change')); return 1; })()`);
        const gcount = await evalJs(`document.querySelectorAll('#grid .pcard').length`);
        check('a group shows only its printers', gcount === 2, gcount);
        await evalJs(`(function(){ var s = document.getElementById('group'); s.value = 'all'; s.dispatchEvent(new Event('change')); return 1; })()`);

        // ---- 7. a failed read keeps the cards --------------------------------------------------
        await evalJs(`window.__fail = 'the slicer is busy'; document.getElementById('refresh').click(); 1`);
        await sleep(200);
        const fail = JSON.parse(await evalJs(`JSON.stringify({ cards: document.querySelectorAll('#grid .pcard').length, banner: document.getElementById('banner').textContent })`));
        check('a failed read keeps the last cards and says why', fail.cards === 5 && /slicer is busy/.test(fail.banner) && /last known/.test(fail.banner), JSON.stringify(fail));
        await evalJs(`window.__fail = null; document.getElementById('refresh').click(); 1`);
        await sleep(200);
        check('the next good read clears the message', (await evalJs(`document.getElementById('banner').style.display`)) === 'none');

        // ---- 8. polling follows the tab -------------------------------------------------------
        await sleep(3000); // the Stop job above has finished and been read back
        await evalJs(`window.__printersActive(false); window.__sent = []; 1`);
        await sleep(11000);
        check('hidden: no reads for longer than the 10 s cadence', (await evalJs('window.__sent.filter(m => m === "printers_get").length')) === 0, await evalJs('JSON.stringify(window.__sent)'));
        await evalJs(`window.__printersActive(true); 1`);
        await sleep(100);
        check('shown again: read at once', (await evalJs('window.__sent.filter(m => m === "printers_get").length')) === 1, await evalJs('JSON.stringify(window.__sent)'));
        await sleep(10500);
        check('and then every 10 s', (await evalJs('window.__sent.filter(m => m === "printers_get").length')) === 2, await evalJs('JSON.stringify(window.__sent)'));

        // ---- 9. themes and contrast -----------------------------------------------------------
        const pairs = `(function(){
            function bgOf(n) { while (n) { var b = getComputedStyle(n).backgroundColor; if (b && b !== 'rgba(0, 0, 0, 0)' && b !== 'transparent') return b; n = n.parentElement; } return getComputedStyle(document.body).backgroundColor; }
            var out = [];
            ['.ppill', '.pname', '.pmodel', '.ptask', '.pprog', '.pitem .k', '.pitem .v', '.pgroup legend', '.pfname', '.perrt', '.perrx', '.pbtn', '.pbtn.stop', '#counts li', '#updated', '.pjobline', '#banner', '.pstage']
              .forEach(function(sel) { document.querySelectorAll(sel).forEach(function(n) {
                  if (!n.offsetParent && sel !== '#banner') return;
                  out.push({ sel: sel, fg: getComputedStyle(n).color, bg: bgOf(n), off: n.classList.contains('off') }); }); });
            return JSON.stringify(out); })()`;
        for (const theme of ['dark', 'light']) {
            await evalJs(`window.__edgeTheme('${theme}'); document.getElementById('banner').style.display = 'block'; document.getElementById('banner').textContent = 'x'; 1`);
            await sleep(100);
            check(theme + ': __edgeTheme switches in place', (await evalJs('document.documentElement.getAttribute("data-theme")')) === theme);
            const list = JSON.parse(await evalJs(pairs));
            const worst = {};
            list.forEach(p => {
                if (p.off) return; // a dimmed control is exempt (WCAG 1.4.3 inactive components)
                const f = parseColor(p.fg), b = parseColor(p.bg);
                if (!f || !b) return;
                const r = contrast(f, b);
                if (!(p.sel in worst) || r < worst[p.sel].r) worst[p.sel] = { r, fg: p.fg, bg: p.bg };
            });
            const low = Object.keys(worst).filter(k => worst[k].r < 4.5);
            console.log('# ' + theme + ' contrast: ' + Object.keys(worst).map(k => k + ' ' + worst[k].r.toFixed(2)).join(', '));
            check(theme + ': every text / background pair is >= 4.5:1', low.length === 0, low.map(k => k + ' ' + worst[k].r.toFixed(2) + ' ' + worst[k].fg + ' on ' + worst[k].bg).join('; '));
        }
        check('no uncaught exception over the run', exceptions().length === 0, JSON.stringify(exceptions().map(e => e.params.exceptionDetails.text)));
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
