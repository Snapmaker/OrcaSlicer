// Gate: the Printer Selection table (resources/web/guide/24, and the wizard's page 21).
//
// A loopback-only Node http server serves the real shipped resources/ folder; a headless Edge /
// Chrome loads the page over the DevTools protocol (Node 22's built-in WebSocket). window.wx is a
// stub that records what the page sends and answers "request_userguide_profile" with mock rows the
// way WebGuideDialog.cpp does. No slicer, no printer, nothing on the desktop.
//
// It checks:
//   * the page loads with no exception; 1000 rows render as a few dozen DOM rows (virtualised),
//     images only for those rows; the last row is reachable by scrolling
//   * initial ticks follow nozzle_selected; unknown sizes / toolheads show a dash
//   * search over brand and model; per-column filters (Brand list, X range, Toolheads list,
//     Enabled yes/no, Model text) and their chips; Clear search and filters
//   * sorting: ascending, descending (unknown values last both ways), then the original order
//   * All / Clear all change only the rows shown, and say so
//   * Confirm sends save_userguide_models keyed by vendor + model with every nozzle variant, then
//     user_guide_finish; with nothing enabled it shows the notice and does not finish; Cancel
//   * a model name shared by two vendors toggles only its own row (#345)
//   * dark mode (the slicer's "dark" user agent) switches the table colours
//   * page 21 (wizard): Next saves the same way and goes to the filament page
//
// Run: node tests/guide/printer_table_gate.js       (skips, exit 0, when no Chromium is found)
// SHOTS=<dir> also writes screenshots (light, dark, filter popover, wizard page).
// MOCK_JSON=<file> uses that {"model":[...]} for the screenshots instead of the synthetic rows.
'use strict';
const fs = require('fs');
const os = require('os');
const path = require('path');
const http = require('http');
const { spawn } = require('child_process');

const RES = process.env.RES_DIR || path.join(__dirname, '..', '..', 'resources');
const SHOTS = process.env.SHOTS || '';

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

// ---- mock rows: 1000 models, some unknown values, one model name shared by two vendors ----------
function synthetic() {
    const model = [];
    const vendors = ['Snapmaker', 'BBL', 'Prusa', 'Creality', 'Voron'];
    for (let i = 0; i < 25; i++) vendors.push('Vendor' + String(i).padStart(2, '0'));
    const count = {};
    for (let n = 0; n < 998; n++) {
        const v = vendors[Math.min(vendors.length - 1, Math.floor(n / 34))];
        const k = count[v] = (count[v] === undefined ? 0 : count[v] + 1);
        const unknown = (n % 97) === 5;
        model.push({
            vendor: v, model: v + ' Model ' + k, name: (v === 'BBL' ? 'Bambu Lab ' : v + ' ') + 'P' + k,
            cover: '/web/image/printer/does_not_exist_' + n + '.png',
            nozzle_diameter: k % 3 ? '0.4' : '0.4;0.2;0.6;0.8',
            nozzle_selected: (n % 50) === 0 ? (k % 3 ? '0.4' : '0.4;0.2;0.6;0.8') : '',
            size_x: unknown ? null : 150 + (n * 7) % 260, size_y: unknown ? null : 150 + (n * 11) % 200,
            size_z: unknown ? null : 150 + (n * 13) % 250, extruders: unknown ? null : [1, 1, 1, 2, 4, 8][n % 6],
        });
    }
    // Same model name, two vendors (#345: keyed by vendor + model).
    model.push({ vendor: 'VendorA', model: 'Shared Model', name: 'Shared Model', cover: '', nozzle_diameter: '0.4',
        nozzle_selected: '', size_x: 200, size_y: 200, size_z: 200, extruders: 1 });
    model.push({ vendor: 'VendorB', model: 'Shared Model', name: 'Shared Model', cover: '', nozzle_diameter: '0.4;0.6',
        nozzle_selected: '', size_x: 300, size_y: 300, size_z: 300, extruders: 2 });
    return { model };
}

// ---- static server for resources/ -----------------------------------------------------------------
const TYPES = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.png': 'image/png', '.svg': 'image/svg+xml', '.jpg': 'image/jpeg' };
const server = http.createServer((req, res) => {
    const u = decodeURIComponent(req.url.split('?')[0]);
    const f = path.join(RES, u);
    if (!f.startsWith(path.resolve(RES)) || !fs.existsSync(f) || fs.statSync(f).isDirectory()) { res.writeHead(404); res.end(); return; }
    res.writeHead(200, { 'Content-Type': TYPES[path.extname(f).toLowerCase()] || 'application/octet-stream' });
    fs.createReadStream(f).pipe(res);
});

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

function stub(mock) {
    return `window.__sent = []; window.__mock = ${JSON.stringify(mock)};
        window.wx = { postMessage: function (s) { var m = JSON.parse(s); window.__sent.push(m);
            if (m.command === 'request_userguide_profile') setTimeout(function () {
                HandleStudio({ command: 'response_userguide_profile', sequence_id: '10001', response: window.__mock }); }, 20); } };`;
}

async function main() {
    const exe = findBrowser();
    if (!exe) { console.log('SKIP: no Edge / Chrome found (set CHROME_BIN)'); return 0; }
    await new Promise(ok => server.listen(0, '127.0.0.1', ok));
    const origin = 'http://127.0.0.1:' + server.address().port;
    const dbgPort = 9300 + Math.floor(Math.random() * 400);
    const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'pttable-'));
    const br = spawn(exe, ['--headless=new', '--remote-debugging-port=' + dbgPort, '--user-data-dir=' + profile,
        '--no-first-run', '--no-default-browser-check', '--disable-gpu', '--hide-scrollbars=false', 'about:blank'], { stdio: 'ignore' });
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
        });
        await S('Page.enable'); await S('Runtime.enable');
        let stubId = null;
        const load = async (page, { dark = false, mock = synthetic(), w = 1000, h = 720 } = {}) => {
            await S('Emulation.setDeviceMetricsOverride', { width: w, height: h, deviceScaleFactor: 1, mobile: false });
            await S('Emulation.setUserAgentOverride', { userAgent: 'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120 Safari/537.36 SM-Slicer/2.4' + (dark ? ' dark' : '') });
            if (stubId) await S('Page.removeScriptToEvaluateOnNewDocument', { identifier: stubId });
            stubId = (await S('Page.addScriptToEvaluateOnNewDocument', { source: stub(mock) })).result.identifier;
            errors.length = 0;
            await S('Page.navigate', { url: origin + '/web/guide/' + page + '/index.html?lang=en' });
            for (let i = 0; i < 50; i++) { await sleep(100); try { if (await evalJs('!!(window.PrinterTable && PrinterTable._state().rows.length)')) break; } catch (e) { } }
            await sleep(300);
        };
        const shot = async name => {
            if (!SHOTS) return;
            fs.mkdirSync(SHOTS, { recursive: true });
            const r = await S('Page.captureScreenshot', { format: 'png' });
            fs.writeFileSync(path.join(SHOTS, name), Buffer.from(r.result.data, 'base64'));
            console.log('     shot ' + path.join(SHOTS, name));
        };
        const st = expr => evalJs(`(function(){ var s = PrinterTable._state(); return (${expr}); })()`);
        const click = sel => evalJs(`(function(){ var e = document.querySelector(${JSON.stringify(sel)}); if (!e) return false; e.click(); return true; })()`);
        const head = key => `.pt-th[data-key="${key}"]`;

        // ============================================================ 1. load, virtualisation
        await load('24');
        check('page 24 loads with no uncaught exception', errors.length === 0, errors.join(' | '));
        check('it asked the slicer for the profile', await evalJs(`window.__sent.some(function(m){ return m.command === 'request_userguide_profile'; })`));
        check('1000 rows are in the table', await st('s.rows.length === 1000 && s.view.length === 1000'), await st('s.rows.length'));
        const domRows = await evalJs(`document.querySelectorAll('.pt-body .pt-row').length`);
        check('only the rows in view (plus a margin) exist in the DOM', domRows > 10 && domRows < 80, domRows);
        check('cover images only for those rows', await evalJs(`document.querySelectorAll('.pt-body img, .pt-body .noimg').length`) <= domRows);
        check('one header row with eight columns, in order',
            await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-head .pt-th'), function(t){ return t.dataset.key; }).join(',')`) === 'checked,icon,brand,model,x,y,z,th'.replace('model', 'name'));
        check('initial ticks follow nozzle_selected (20 enabled)', await st('s.rows.filter(function(r){ return r.checked; }).length === 20'));
        check('the first rendered row\'s checkbox shows its state',
            await evalJs(`(function(){ var r = PrinterTable._state().view[0]; var n = document.querySelector('.pt-body .pt-row[data-id="' + r.id + '"] input'); return n && n.checked === r.checked; })()`));
        check('count line says how many printers and how many are enabled', /1000 printers .* 20 enabled/.test(await evalJs(`document.querySelector('.pt-count').textContent`)),
            await evalJs(`document.querySelector('.pt-count').textContent`));
        const t0 = Date.now();
        await evalJs(`(function(){ var b = document.querySelector('.pt-body'); b.scrollTop = b.scrollHeight; b.dispatchEvent(new Event('scroll')); })()`);
        await sleep(200);
        check('scrolling to the end renders the last row', await evalJs(`(function(){ var s = PrinterTable._state(); var last = s.view[s.view.length - 1]; return !!document.querySelector('.pt-body .pt-row[data-id="' + last.id + '"]'); })()`));
        check('and the DOM stays small after the scroll', await evalJs(`document.querySelectorAll('.pt-body .pt-row').length`) < 80);
        check('scroll + render under 1 s', Date.now() - t0 < 1000, Date.now() - t0);
        check('an unknown size shows a dash', await evalJs(`(function(){ var s = PrinterTable._state(); var r = s.rows.filter(function(r){ return r.x === null; })[0];
            document.querySelector('.pt-body').scrollTop = s.view.indexOf(r) * 36; document.querySelector('.pt-body').dispatchEvent(new Event('scroll')); return r.id; })()`).then(async id => {
            await sleep(150);
            return evalJs(`(function(){ var n = document.querySelector('.pt-body .pt-row[data-id="${id}"]'); return n && n.children[4].textContent === '\u2014' && n.children[7].textContent === '\u2014'; })()`);
        }));

        // ============================================================ 2. search
        await evalJs(`(function(){ var i = document.querySelector('.pt-toolbar .searchTerm'); i.value = 'bambu p1'; i.dispatchEvent(new Event('input')); })()`);
        check('search matches brand and model words (Bambu Lab P1, P10..P19)', await st('s.view.length === 11 && s.view.every(function(r){ return r.vendor === "BBL"; })'), await st('s.view.length'));
        check('the count line says how many are shown', /11 of 1000 shown/.test(await evalJs(`document.querySelector('.pt-count').textContent`)));
        check('the All / Clear all scope says "the 11 shown"', /11 shown/.test(await evalJs(`document.querySelector('.pt-scope').textContent`)));
        // ============================================================ 3. All / Clear all act on the shown rows
        const outside = () => st('s.rows.filter(function(r){ return s.view.indexOf(r) < 0; }).map(function(r){ return r.checked ? 1 : 0; }).join("")');
        const before = await outside();
        await evalJs(`document.querySelector('.pt-toolbar .SmallBtn_Green').click()`);
        check('All ticks the 11 shown rows', await st('s.view.every(function(r){ return r.checked; })'));
        check('and leaves the 989 others as they were', (await outside()) === before);
        await evalJs(`document.querySelector('.pt-toolbar .SmallBtn').click()`);
        check('Clear all unticks the 11 shown rows', await st('s.view.every(function(r){ return !r.checked; })'));
        check('and leaves the others as they were', (await outside()) === before);
        await click('.pt-chips .pt-link');
        check('Clear search and filters brings back all 1000', await st('s.view.length === 1000 && s.state.search === ""') && await evalJs(`document.querySelector('.pt-toolbar .searchTerm').value === ''`));

        // ============================================================ 4. sorting
        await click(head('x') + ' .pt-sort');
        check('X ascending: sorted, unknown last', await st(`(function(){ var v = s.view.map(function(r){ return r.x; }); var k = v.filter(function(x){ return x !== null; });
            for (var i = 1; i < k.length; i++) if (k[i] < k[i-1]) return false; return v.slice(k.length).every(function(x){ return x === null; }) && v.indexOf(null) === k.length; })()`));
        check('the header says ascending', await evalJs(`document.querySelector('${head('x')}').getAttribute('aria-sort') === 'ascending'`));
        await click(head('x') + ' .pt-sort');
        check('X descending: sorted, unknown still last', await st(`(function(){ var v = s.view.map(function(r){ return r.x; }); var k = v.filter(function(x){ return x !== null; });
            for (var i = 1; i < k.length; i++) if (k[i] > k[i-1]) return false; return v.indexOf(null) === k.length; })()`));
        await click(head('x') + ' .pt-sort');
        check('a third click returns to the original order (Snapmaker first)', await st('s.view.every(function(r, i){ return r.order === i; }) && s.view[0].vendor === "Snapmaker"') &&
            await evalJs(`document.querySelector('${head('x')}').getAttribute('aria-sort') === 'none'`));
        await click(head('brand') + ' .pt-sort');
        check('Brand ascending sorts by brand name', await st(`(function(){ var c = new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' });
            for (var i = 1; i < s.view.length; i++) if (c.compare(s.view[i-1].brand, s.view[i].brand) > 0) return false; return true; })()`));
        await click(head('checked') + ' .pt-sort');
        check('Enabled ascending puts the enabled printers first', await st('(function(n){ return n > 0 && s.view.slice(0, n).every(function(r){ return r.checked; }) && !s.view[n].checked; })(s.rows.filter(function(r){ return r.checked; }).length)'));
        await click(head('checked') + ' .pt-sort'); await click(head('checked') + ' .pt-sort');

        // ============================================================ 5. column filters
        await click(head('brand') + ' .pt-filter-btn');
        check('the Brand filter opens a list with a find box', await evalJs(`!!document.querySelector('.pt-pop input[type=text]') && document.querySelectorAll('.pt-pop .pt-opt').length === 32`),
            await evalJs(`document.querySelectorAll('.pt-pop .pt-opt').length`));
        await shot('table_filter_brand.png');
        await click('.pt-pop .pt-pop-btns .pt-link:last-child');   // Select none
        await evalJs(`(function(){ var o = Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /Prusa|Bambu Lab/.test(l.textContent); });
            o.forEach(function(l){ l.querySelector('input').click(); }); return o.length; })()`);
        check('Brand: Prusa + Bambu Lab only', await st('s.view.length === 68 && s.view.every(function(r){ return r.vendor === "Prusa" || r.vendor === "BBL"; })'), await st('s.view.length'));
        check('the funnel is highlighted and a chip names the brands', await evalJs(`document.querySelector('${head('brand')} .pt-filter-btn').classList.contains('on')`) &&
            /Brand: Bambu Lab, Prusa/.test(await evalJs(`document.querySelector('.pt-chips').textContent`)), await evalJs(`document.querySelector('.pt-chips').textContent`));
        await evalJs(`document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await evalJs(`document.querySelector('.pt-pop') && document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        check('Esc closes the filter, not the dialog', !(await evalJs(`!!document.querySelector('.pt-pop')`)) &&
            !(await evalJs(`window.__sent.some(function(m){ return m.command === 'close_page' || m.command === 'user_guide_cancel'; })`)));
        await click(head('x') + ' .pt-filter-btn');
        await evalJs(`(function(){ var i = document.querySelector('.pt-pop input[type=number]'); i.value = '300'; i.dispatchEvent(new Event('input')); })()`);
        check('X at least 300 combines with the brand filter', await st('s.view.length > 0 && s.view.every(function(r){ return r.x !== null && r.x >= 300 && (r.vendor === "Prusa" || r.vendor === "BBL"); })'));
        check('chip "X mm ≥ 300"', /X mm ≥ 300/.test(await evalJs(`document.querySelector('.pt-chips').textContent`)));
        await click('.pt-chips .pt-link');
        await click(head('th') + ' .pt-filter-btn');
        check('the Toolheads list holds 1, 2, 4, 8 and a dash', await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-pop .pt-opt span:not(.n)'), function(s){ return s.textContent; }).join(',')`) === '1,2,4,8,\u2014',
            await evalJs(`Array.prototype.map.call(document.querySelectorAll('.pt-pop .pt-opt span:not(.n)'), function(s){ return s.textContent; }).join(',')`));
        await click('.pt-pop .pt-pop-btns .pt-link:last-child');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return l.querySelector('span').textContent === '4'; })[0].querySelector('input').click()`);
        check('Toolheads = 4 only', await st('s.view.length > 0 && s.view.every(function(r){ return r.th === 4; })'));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        await click('.pt-chips .pt-link');
        await click(head('checked') + ' .pt-filter-btn');
        await evalJs(`Array.prototype.filter.call(document.querySelectorAll('.pt-pop .pt-opt'), function(l){ return /Not enabled/.test(l.textContent); })[0].querySelector('input').click()`);
        check('Enabled filter: only the enabled ones', await st('s.view.length > 0 && s.view.length === s.rows.filter(function(r){ return r.checked; }).length && s.view.every(function(r){ return r.checked; })'), await st('s.view.length'));
        await click('.pt-chips .pt-link');
        await click(head('name') + ' .pt-filter-btn');
        await evalJs(`(function(){ var i = document.querySelector('.pt-pop input[type=text]'); i.value = 'shared'; i.dispatchEvent(new Event('input')); })()`);
        check('Model text filter', await st('s.view.length === 2'));
        await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);

        // ============================================================ 6. same model name, two vendors
        await evalJs(`(function(){ var s = PrinterTable._state(); var a = s.view.filter(function(r){ return r.vendor === 'VendorA'; })[0];
            document.querySelector('.pt-body .pt-row[data-id="' + a.id + '"]').querySelectorAll('.pt-td')[2].click(); })()`);
        check('clicking a row ticks it; the other vendor\'s same-named model stays unticked',
            await st('s.rows.filter(function(r){ return r.model === "Shared Model"; }).map(function(r){ return r.vendor + "=" + r.checked; }).join(",")') === 'VendorA=true,VendorB=false');
        await click('.pt-chips .pt-link');

        // ============================================================ 7. Confirm / Cancel
        await evalJs(`window.__sent = []`);
        await click('#AcceptBtn');
        const sent = await evalJs(`window.__sent`);
        const save = sent.find(m => m.command === 'save_userguide_models');
        check('Confirm sends save_userguide_models, then user_guide_finish', !!save && sent.map(m => m.command).join(',') === 'save_userguide_models,user_guide_finish', sent.map(m => m.command).join(','));
        const keys = save ? Object.keys(save.data) : [];
        const nOn = await st('s.rows.filter(function(r){ return r.checked; }).length');
        check('one entry per enabled model, keyed vendor + "\\n" + model', keys.length === nOn && keys.includes('VendorA\nShared Model') && !keys.includes('VendorB\nShared Model'), keys.length);
        const mockNoz = await evalJs(`(function(){ var o = {}; window.__mock.model.forEach(function(m){ o[m.vendor + String.fromCharCode(10) + m.model] = m.nozzle_diameter; }); return o; })()`);
        check('each entry carries every nozzle variant of its model (as sent by the slicer)', !!save && keys.every(k => save.data[k].nozzle_diameter === mockNoz[k] &&
            save.data[k].vendor + String.fromCharCode(10) + save.data[k].model === k) && keys.some(k => save.data[k].nozzle_diameter === '0.4;0.2;0.6;0.8'));
        await evalJs(`window.__sent = []`);
        await click('.pt-toolbar .SmallBtn');   // Clear all (nothing filtered: every printer)
        await click('#AcceptBtn');
        check('with nothing enabled, Confirm shows the notice and does not finish',
            (await evalJs(`window.__sent.map(function(m){ return m.command; }).join(',')`)) === 'save_userguide_models' && await evalJs(`$('#NoticeBody').is(':visible')`));
        await evalJs(`document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        check('Esc closes the notice first', !(await evalJs(`$('#NoticeBody').is(':visible')`)) && !(await evalJs(`window.__sent.some(function(m){ return m.command === 'close_page'; })`)));
        await evalJs(`window.__sent = []`);
        await click('#PreBtn');
        check('Cancel sends user_guide_cancel only', (await evalJs(`window.__sent.map(function(m){ return m.command; }).join(',')`)) === 'user_guide_cancel');
        await evalJs(`document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
        check('Esc with nothing open closes the dialog (close_page)', await evalJs(`window.__sent.some(function(m){ return m.command === 'close_page'; })`));
        check('no uncaught exception during all that', errors.length === 0, errors.join(' | '));

        // ============================================================ 8. themes
        const bgLight = await evalJs(`getComputedStyle(document.querySelector('.pt-grid')).backgroundColor`);
        await load('24', { dark: true });
        const bgDark = await evalJs(`getComputedStyle(document.querySelector('.pt-grid')).backgroundColor`);
        check('dark user agent: dark.css is on and the table turns dark', bgLight === 'rgb(255, 255, 255)' && bgDark === 'rgb(45, 45, 49)', bgLight + ' / ' + bgDark);
        check('dark: body text is light', await evalJs(`getComputedStyle(document.querySelector('.pt-body .pt-td:nth-child(4)')).color`) === 'rgb(239, 239, 240)');

        // ============================================================ 9. wizard page 21
        await load('21', { w: 820, h: 660 });
        check('page 21 loads the same table', errors.length === 0 && await st('s.rows.length === 1000'), errors.join(' | '));
        await evalJs(`window.__sent = []`);
        await click('#AcceptBtn');
        await sleep(400);
        const s21 = await evalJs(`(window.__sent || []).length`).catch(() => -1);
        check('Next saves and moves to the filament page', /\/guide\/22\/index\.html/.test(await evalJs(`location.pathname`)), await evalJs(`location.pathname`) + ' sent=' + s21);

        // ============================================================ screenshots with real rows
        if (SHOTS) {
            const real = process.env.MOCK_JSON ? JSON.parse(fs.readFileSync(process.env.MOCK_JSON, 'utf8')) : synthetic();
            await load('24', { mock: real });
            await sleep(800);
            await shot('table_light.png');
            await click(head('brand') + ' .pt-filter-btn');
            await sleep(300);
            await shot('table_filter_popover.png');
            await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
            await click(head('th') + ' .pt-sort'); await click(head('th') + ' .pt-sort');
            await click(head('x') + ' .pt-filter-btn');
            await evalJs(`(function(){ var i = document.querySelector('.pt-pop input[type=number]'); i.value = '300'; i.dispatchEvent(new Event('input')); })()`);
            await evalJs(`document.querySelector('.pt-pop').dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true }))`);
            await sleep(600);
            await shot('table_sorted_filtered.png');
            await load('24', { mock: real, dark: true });
            await sleep(800);
            await shot('table_dark.png');
            await load('21', { mock: real, w: 820, h: 660 });
            await sleep(800);
            await shot('wizard_page21.png');
        }
    } finally {
        try { br.kill(); } catch (e) { }
        server.close();
    }
    console.log(`\n${passed} passed, ${failures} failed`);
    return failures ? 1 : 0;
}

main().then(c => process.exit(c), e => { console.error(e); process.exit(2); });
