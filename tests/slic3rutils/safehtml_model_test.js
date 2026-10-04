// Headless check of the project page's SafeHtml / EscapeHtml / EscapeClickPath
// (resources/web/model/model.js). Loads the real functions into jsdom (no reimplementation).
//
// Not wired into CMake/CI (Edge rule: no test CMake edits). Owed follow-up: hook this
// into CI. Pin: tests/slic3rutils/package.json + package-lock.json.
//
// Run: npm install --prefix tests/slic3rutils && node tests/slic3rutils/safehtml_model_test.js
// Missing node or jsdom: exit 0 with SKIP, unless CI is a truthy value (then exit 1).
// CI=0, CI=false, CI=no, CI=off, or an empty CI must not hard-fail.
'use strict';

function ciIsSet() {
    const v = process.env.CI;
    if (v == null) return false;
    const s = String(v).trim().toLowerCase();
    return s !== '' && s !== '0' && s !== 'false' && s !== 'no' && s !== 'off';
}

function skip(why) {
    if (ciIsSet()) {
        console.error('FAIL: ' + why + ' (CI is set; jsdom is required)');
        process.exit(1);
    }
    console.log('SKIP: ' + why);
    process.exit(0);
}

if (typeof process === 'undefined' || !process.versions || !process.versions.node)
    skip('node is not available');

const fs = require('fs');
const path = require('path');

const MODEL_JS = path.join(__dirname, '..', '..', 'resources', 'web', 'model', 'model.js');
const INDEX_HTML = path.join(__dirname, '..', '..', 'resources', 'web', 'model', 'index.html');
const JQUERY_JS = path.join(__dirname, '..', '..', 'resources', 'web', 'include', 'jquery-2.1.1.min.js');
const GLOBALAPI_JS = path.join(__dirname, '..', '..', 'resources', 'web', 'include', 'globalapi.js');
const JSDOM_PATH = path.join(__dirname, 'node_modules', 'jsdom');

let JSDOM;
try {
    JSDOM = require(JSDOM_PATH).JSDOM;
} catch (e) {
    skip("jsdom is not installed; run: npm install --prefix tests/slic3rutils");
}

const source = fs.readFileSync(MODEL_JS, 'utf8');
const indexHtml = fs.readFileSync(INDEX_HTML, 'utf8');
const globalapiSrc = fs.readFileSync(GLOBALAPI_JS, 'utf8');
const jquerySrc = fs.readFileSync(JQUERY_JS, 'utf8');

function extractSafeHtmlBlock(src) {
    const start = src.indexOf('function EscapeHtml');
    if (start < 0) throw new Error('EscapeHtml not found - file structure changed');
    const safeStart = src.indexOf('function SafeHtml', start);
    if (safeStart < 0) throw new Error('SafeHtml not found - file structure changed');
    let i = src.indexOf('{', safeStart);
    let depth = 0;
    let end = -1;
    for (; i < src.length; i++) {
        if (src[i] === '{') depth++;
        else if (src[i] === '}') {
            depth--;
            if (depth === 0) {
                end = i + 1;
                break;
            }
        }
    }
    if (end < 0) throw new Error('could not find end of SafeHtml()');
    return src.slice(start, end);
}

const extracted = extractSafeHtmlBlock(source);
if (extracted.indexOf('function SafeUrlValue') < 0)
    throw new Error('SafeUrlValue must sit between EscapeHtml and SafeHtml so this test loads the real helper');
if (extracted.indexOf('function EscapeClickPath') < 0)
    throw new Error('EscapeClickPath must sit between EscapeHtml and SafeHtml so this test loads the real helper');
if (extracted.indexOf('function SafeKeepUrl') < 0)
    throw new Error('SafeKeepUrl must sit between EscapeHtml and SafeHtml so this test loads the real helper');

const dom = new JSDOM(
    '<!DOCTYPE html><html><head></head><body></body></html><script>' + extracted + '</script>',
    { runScripts: 'dangerously', url: 'file:///resources/web/model/index.html' }
);
const SafeHtml = dom.window.SafeHtml;
const EscapeHtml = dom.window.EscapeHtml;
const EscapeClickPath = dom.window.EscapeClickPath;
const SafeKeepUrl = dom.window.SafeKeepUrl;
if (typeof SafeHtml !== 'function') throw new Error('SafeHtml did not install on the jsdom window');
if (typeof EscapeHtml !== 'function') throw new Error('EscapeHtml did not install on the jsdom window');
if (typeof EscapeClickPath !== 'function') throw new Error('EscapeClickPath did not install on the jsdom window');
if (typeof SafeKeepUrl !== 'function') throw new Error('SafeKeepUrl did not install on the jsdom window');

function extractFunction(src, name) {
    const start = src.indexOf('function ' + name);
    if (start < 0) throw new Error(name + ' not found - file structure changed');
    let i = src.indexOf('{', start);
    let depth = 0;
    for (; i < src.length; i++) {
        if (src[i] === '{') depth++;
        else if (src[i] === '}') {
            depth--;
            if (depth === 0) return src.slice(start, i + 1);
        }
    }
    throw new Error('could not find end of ' + name + '()');
}

function injectScript(win, text) {
    const el = win.document.createElement('script');
    el.textContent = text;
    win.document.head.appendChild(el);
}

// Load the real ShowModelInfo / ShowProfilelInfo / ConstructFileHtml (not a reimplementation)
// so removing EscapeHtml at those call sites fails this file.
function loadProjectPage() {
    const page = new JSDOM(indexHtml, {
        url: 'file:///resources/web/model/index.html',
        runScripts: 'dangerously',
        pretendToBeVisual: true
    });
    const w = page.window;
    w.SendWXDebugInfo = function () {};
    w.SendWXMessage = function () {};
    w.UpdateModelID = function () {};
    w.TranslatePage = function () {};
    w.RequestProjectInfo = function () {};
    w.Swiper = function () { return { destroy: function () {} }; };
    injectScript(w, jquerySrc);
    if (typeof w.jQuery !== 'function') throw new Error('jQuery did not install');
    w.jQuery.fn.viewer = function () { return this; };
    injectScript(w, extractFunction(globalapiSrc, 'getFileTail'));
    injectScript(w, extractFunction(globalapiSrc, 'html_decode'));
    injectScript(w, source);
    if (typeof w.ShowModelInfo !== 'function') throw new Error('ShowModelInfo did not install');
    if (typeof w.ShowProfilelInfo !== 'function') throw new Error('ShowProfilelInfo did not install');
    if (typeof w.ConstructFileHtml !== 'function') throw new Error('ConstructFileHtml did not install');
    return w;
}

function findFilesNamed(dir, name, out) {
    const entries = fs.readdirSync(dir, { withFileTypes: true });
    for (let i = 0; i < entries.length; i++) {
        const p = path.join(dir, entries[i].name);
        if (entries[i].isDirectory()) findFilesNamed(p, name, out);
        else if (entries[i].name === name) out.push(p);
    }
    return out;
}

// First arg is a string / template / identifier (not a function expression).
const STRING_TIMER_RE = /set(?:Interval|Timeout)\s*\(\s*(?:(["'`])|(?!function\b)[A-Za-z_$][A-Za-z0-9_$]*)/;

function loadDarkModePage() {
    const page = new JSDOM(indexHtml, {
        url: 'file:///resources/web/model/index.html',
        runScripts: 'dangerously',
        pretendToBeVisual: true
    });
    const w = page.window;
    Object.defineProperty(w.navigator, 'userAgent', {
        configurable: true,
        get: function () { return 'Mozilla/5.0 Headless LightMode'; }
    });
    const intervalFns = [];
    w.setInterval = function (fn) {
        intervalFns.push(fn);
        return intervalFns.length;
    };
    injectScript(w, jquerySrc);
    if (typeof w.jQuery !== 'function') throw new Error('jQuery did not install');
    injectScript(w, extractFunction(globalapiSrc, 'RemoveCssLink'));
    injectScript(w, extractFunction(globalapiSrc, 'AddCssLink'));
    injectScript(w, extractFunction(globalapiSrc, 'CheckCssLinkExist'));
    injectScript(w, extractFunction(globalapiSrc, 'ExecuteDarkMode'));
    injectScript(w, extractFunction(globalapiSrc, 'SwitchDarkMode'));
    if (typeof w.SwitchDarkMode !== 'function') throw new Error('SwitchDarkMode did not install');
    return { w: w, intervalFns: intervalFns };
}

function descIsSanitized(box) {
    if (!box) return false;
    const html = box.innerHTML;
    const img = box.querySelector('img');
    const a = box.querySelector('a');
    if (/onerror/i.test(html)) return false;
    if (img && img.hasAttribute('onerror')) return false;
    if (img && (img.getAttribute('src') || '') === 'x') return false;
    if (a && /javascript:/i.test(a.getAttribute('href') || '')) return false;
    if (/javascript:/i.test(html)) return false;
    if (box.querySelector('script')) return false;
    return /ok/i.test(box.textContent);
}

const HOSTILE_DESC = '<img src=x onerror=alert(1)><script>alert(2)</script><a href="javascript:alert(3)">x</a><p>ok</p>';

let failures = 0;
let passed = 0;
function check(label, cond) {
    if (cond) {
        passed++;
        console.log('PASS: ' + label);
    } else {
        failures++;
        console.error('FAIL: ' + label);
    }
}

{
    const out = SafeHtml('<img src="https://cdn/a.png" srcset="https://cdn/a.png 1x, http://192.168.1.1/b.png 2x">');
    check('srcset mixed http is stripped', !/srcset/i.test(out));
    check('srcset http candidate is gone', !/192\.168\.1\.1/.test(out));
    check('https src survives srcset strip', /src\s*=\s*["']https:\/\/cdn\/a\.png["']/i.test(out));
}

{
    const out = SafeHtml('<picture><source srcset="https://a 1x, http://10.0.0.1/y 2x"><img src="https://a"></picture>');
    check('source tag is removed', !/<source/i.test(out));
    check('picture/source http candidate is gone', !/10\.0\.0\.1/.test(out));
    check('picture inner https img is kept', /src\s*=\s*["']https:\/\/a["']/i.test(out));
}

{
    const out = SafeHtml('<img src="ht tps://cdn.example.com/a.png">');
    check('spaced scheme is not treated as https', !/https:/i.test(out) && !/ht tps/i.test(out));
}

{
    const out = SafeHtml('<img src="https://cdn.example.com/a.png" alt="ok" width="10" height="10" title="t">');
    check('https img src is kept', /src\s*=\s*["']https:\/\/cdn\.example\.com\/a\.png["']/i.test(out));
    check('img alt/width/height/title are kept', /alt/i.test(out) && /width/i.test(out) && /height/i.test(out) && /title/i.test(out));
}

{
    const out = SafeHtml('<img src="http://cdn.example.com/a.png">');
    check('plain http img src is dropped', !/src/i.test(out) && !/http:\/\/cdn\.example\.com/i.test(out));
}

{
    const out = SafeHtml('<img src="https://user:pw@cdn.example.com/a.png">');
    check('https img src with userinfo is dropped', !/src/i.test(out) && !/user:pw/i.test(out));
}

{
    const out = SafeHtml('<img src="https://cdn/a.png" style="background:url(http://10.0.0.1/x.png)">');
    check('style url() is dropped', !/style/i.test(out) && !/10\.0\.0\.1/.test(out));
}

{
    const out = SafeHtml('<div style="background:url(http://10.0.0.1/x.png)">x</div>');
    check('style on div is dropped', !/style/i.test(out) && !/10\.0\.0\.1/.test(out) && /<div/i.test(out));
}

{
    const out = SafeHtml('<svg><image href="http://10.0.0.1/x.png"></image></svg>');
    check('svg image href is dropped', !/<svg/i.test(out) && !/<image/i.test(out) && !/10\.0\.0\.1/.test(out));
}

{
    const out = SafeHtml('<video poster="https://cdn/p.jpg"></video><track src="http://10.0.0.1/t.vtt">');
    check('video and track are removed', !/<video/i.test(out) && !/<track/i.test(out) && !/10\.0\.0\.1/.test(out));
}

{
    const out = SafeHtml('<table background="http://10.0.0.1/b.jpg"></table>');
    check('table background is dropped', !/background/i.test(out) && !/10\.0\.0\.1/.test(out));
}

{
    const out = SafeHtml('<a href="https://example.com" ping="http://10.0.0.1/p" longdesc="http://10.0.0.1/d">x</a>');
    check('ping is dropped', !/ping/i.test(out));
    check('longdesc is dropped', !/longdesc/i.test(out));
    check('https href is kept', /href\s*=\s*["']https:\/\/example\.com["']/i.test(out));
}

{
    const js = SafeHtml('<a href="javascript:alert(1)">x</a>');
    const tab = SafeHtml('<a href="java&#x09;script:alert(1)">x</a>');
    const data = SafeHtml('<a href="data:text/html,hi">x</a>');
    const vbs = SafeHtml('<a href="vbscript:msgbox(1)">x</a>');
    check('javascript: href is dropped', !/href/i.test(js) || !/javascript:/i.test(js));
    check('javascript: payload is gone', !/alert/i.test(js));
    check('obfuscated java&#x09;script: href is dropped', !/href/i.test(tab) || !/javascript:/i.test(tab));
    check('obfuscated javascript payload is gone', !/alert/i.test(tab));
    check('data: href is dropped', !/href/i.test(data) || !/data:/i.test(data));
    check('vbscript: href is dropped', !/href/i.test(vbs) || !/vbscript:/i.test(vbs));
}

{
    const out = SafeHtml('<a href="https://user:pw@example.com/">x</a>');
    check('https href with userinfo is dropped', !/href/i.test(out) || !/user:pw/i.test(out));
}

{
    const out = SafeHtml('<a href="https://example.com">x</a>');
    check('a target=_blank gets rel=noopener', /rel\s*=\s*["'][^"']*noopener/i.test(out));
}

{
    const img = SafeHtml('<img src="https://cdn/a.png" onerror="alert(1)">');
    const a = SafeHtml('<a href="https://example.com" onclick="alert(1)">x</a>');
    const div = SafeHtml('<div onclick="alert(1)">x</div>');
    check('img onerror is dropped', !/onerror/i.test(img) && !/alert/i.test(img));
    check('a onclick is dropped', !/onclick/i.test(a) && !/alert/i.test(a));
    check('div onclick is dropped', !/onclick/i.test(div) && !/alert/i.test(div));
}

{
    const out = SafeHtml('<img src="https://cdn/a.png" dynsrc="http://10.0.0.1/x" lowsrc="http://10.0.0.1/y" imagesrcset="http://10.0.0.1/z 2x">');
    check('dynsrc is dropped', !/dynsrc/i.test(out) && !/10\.0\.0\.1/.test(out));
    check('lowsrc is dropped', !/lowsrc/i.test(out));
    check('imagesrcset is dropped', !/imagesrcset/i.test(out));
}

{
    const out = SafeHtml('<noscript><img src="http://10.0.0.1/x"></noscript><template><img src="http://10.0.0.1/y"></template><p>ok</p>');
    check('noscript is removed', !/<noscript/i.test(out));
    check('template is removed', !/<template/i.test(out));
    check('noscript/template http is gone', !/10\.0\.0\.1/.test(out));
}

{
    const src = 'data:image/png;base64,xx" onclick=alert(1) x="';
    const name = 'Bob\'s <file>&x.pdf';
    check('EscapeHtml preview src encodes quotes and <',
        EscapeHtml(src).indexOf('"') < 0 && EscapeHtml(src).indexOf('<') < 0 && /&quot;/.test(EscapeHtml(src)));
    check('EscapeHtml file name encodes quotes, < and &',
        EscapeHtml(name) === 'Bob&#39;s &lt;file&gt;&amp;x.pdf');
}

{
    const raw = 'C:\\dir\\a&b"c\'d<e';
    const out = EscapeClickPath(raw);
    check('EscapeClickPath encodes & first', out.indexOf('&amp;') >= 0 && !/&b/.test(out.replace(/&amp;/g, '')));
    check('EscapeClickPath encodes quote, apostrophe and <',
        /&quot;/.test(out) && /\\'/.test(out) && /&lt;/.test(out) && out.indexOf('<') < 0 && out.indexOf('"') < 0);
}

{
    check('CSP has default-src none', /default-src\s+'none'/.test(indexHtml));
    check('CSP has base-uri none', /base-uri\s+'none'/.test(indexHtml));
    check('CSP has form-action none', /form-action\s+'none'/.test(indexHtml));
    check('CSP has frame-src none', /frame-src\s+'none'/.test(indexHtml));
    check('CSP script-src allows self', /script-src[^;]*'self'/.test(indexHtml));
    check('CSP style-src allows self', /style-src[^;]*'self'/.test(indexHtml));
    check('CSP has font-src data:', /font-src[^;]*data:/.test(indexHtml));
    check('CSP has connect-src', /connect-src/.test(indexHtml));
    check('CSP has object-src', /object-src/.test(indexHtml));
    check('CSP has no unsafe-eval', !/unsafe-eval/.test(indexHtml));
    const imgSrc = ((indexHtml.match(/img-src\s+([^;]+)/) || [])[1] || '').trim();
    const imgTokens = imgSrc.split(/\s+/).filter(Boolean);
    const imgAllowed = ['https:', "'self'", 'data:'];
    check('CSP img-src is https: self data: only',
        imgTokens.length === 3
        && imgAllowed.every(function (t) { return imgTokens.indexOf(t) >= 0; })
        && imgTokens.every(function (t) { return imgAllowed.indexOf(t) >= 0; }));
}

{
    check('globalapi.js has no string-form setInterval', !/setInterval\s*\(\s*["']/.test(globalapiSrc));
    check('globalapi.js has no string-form setTimeout', !/setTimeout\s*\(\s*["']/.test(globalapiSrc));
    check('globalapi.js has no eval(', !/\beval\s*\(/.test(globalapiSrc));
    check('globalapi.js has no new Function', !/new\s+Function\s*\(/.test(globalapiSrc));
    const webRoot = path.join(__dirname, '..', '..', 'resources', 'web');
    const copies = findFilesNamed(webRoot, 'globalapi.js', []);
    check('found every globalapi.js copy', copies.length >= 4);
    for (let i = 0; i < copies.length; i++) {
        const src = fs.readFileSync(copies[i], 'utf8');
        const rel = path.relative(webRoot, copies[i]);
        check(rel + ' has no string/template/variable timer', !STRING_TIMER_RE.test(src));
        check(rel + ' has no eval(', !/\beval\s*\(/.test(src));
        check(rel + ' has no new Function', !/new\s+Function\s*\(/.test(src));
    }
}

{
    const page = new JSDOM(indexHtml, { url: 'file:///resources/web/model/index.html' });
    const scripts = page.window.document.querySelectorAll('script[src]');
    const links = page.window.document.querySelectorAll('link[rel="stylesheet"]');
    check('page chrome scripts are relative (self)',
        scripts.length > 0 && Array.prototype.every.call(scripts, function (s) { return !/^[a-z]+:/i.test(s.getAttribute('src') || ''); }));
    check('page chrome stylesheets are relative (self)',
        links.length > 0 && Array.prototype.every.call(links, function (s) { return !/^[a-z]+:/i.test(s.getAttribute('href') || ''); }));
}

{
    const forms = [
        ['https:///u:p@h', 'https:///u:p@h'],
        ['https:\\\\u:p@h', 'https:\\u:p@h'],
        ['https:/u:p@h', 'https:/u:p@h'],
        ['https:u:p@h', 'https:u:p@h']
    ];
    for (let i = 0; i < forms.length; i++) {
        const label = forms[i][0];
        const raw = forms[i][1];
        check('SafeKeepUrl rejects ' + label, SafeKeepUrl(raw, true) === false);
        const out = SafeHtml('<img src="' + raw + '/x.png">');
        check('SafeHtml drops img src ' + label, !/src/i.test(out) && !/u:p/i.test(out));
    }
    check('SafeKeepUrl rejects https://:pw@host', SafeKeepUrl('https://:pw@host', true) === false);
    const pwOnly = SafeHtml('<img src="https://:pw@host/x.png">');
    check('SafeHtml drops img src https://:pw@host', !/src/i.test(pwOnly) && !/:pw@/i.test(pwOnly));
}

{
    const hostile = 'data:image/png;base64,xx" onclick=alert(1)><img src=x onerror=alert(2) x="';
    const w = loadProjectPage();
    w.ShowModelInfo({
        name: 'n',
        author: 'a',
        upload_type: 'origin',
        license: 'CC0',
        description: 'd',
        preview_img: [{ filepath: hostile }]
    });
    const imgs = w.document.querySelectorAll('#ModelPreviewList img');
    const src = imgs[0] ? imgs[0].getAttribute('src') || '' : '';
    check('ShowModelInfo preview src escapes quotes and <',
        imgs.length === 1 && !imgs[0].hasAttribute('onclick') && !imgs[0].hasAttribute('onerror')
        && src.indexOf('"') >= 0 && src.indexOf('<') >= 0);
}

{
    const hostile = 'data:image/png;base64,yy" onclick=alert(1)><img src=x onerror=alert(2) y="';
    const w = loadProjectPage();
    w.ShowProfilelInfo({
        name: 'n',
        author: 'a',
        description: 'd',
        preview_img: [{ filepath: hostile }]
    });
    const imgs = w.document.querySelectorAll('#ProfilePreviewList img');
    const src = imgs[0] ? imgs[0].getAttribute('src') || '' : '';
    check('ShowProfilelInfo preview src escapes quotes and <',
        imgs.length === 1 && !imgs[0].hasAttribute('onclick') && !imgs[0].hasAttribute('onerror')
        && src.indexOf('"') >= 0 && src.indexOf('<') >= 0);
}

{
    const w = loadProjectPage();
    w.ConstructFileHtml('FILE_OTHER_List', [
        { filepath: 'C:\\dir\\a&b"c\'d<e.pdf', filename: "Bob's <file>&x.pdf" }
    ]);
    const nameEl = w.document.querySelector('#FILE_OTHER_List .FileName');
    const menu = w.document.querySelector('#FILE_OTHER_List .FileMenu');
    const oc = menu ? menu.getAttribute('onclick') || '' : '';
    check('ConstructFileHtml file name escapes quotes, < and &',
        !!nameEl && nameEl.children.length === 0
        && /&lt;file&gt;/.test(nameEl.innerHTML) && /&amp;x/.test(nameEl.innerHTML)
        && nameEl.textContent.indexOf("Bob's") >= 0);
    check('ConstructFileHtml onClick path uses EscapeClickPath',
        /^OnClickOpenFile\('/.test(oc) && oc.indexOf('&') >= 0 && oc.indexOf('"') >= 0
        && oc.indexOf('<') >= 0 && /\\'/.test(oc));
}

{
    const w = loadProjectPage();
    w.ConstructFileHtml('FILE_OTHER_List', [
        { filepath: 'data:image/png;base64,xx" onclick=alert(1)><img src=x onerror=alert(2) x="', filename: 'evil.png' }
    ]);
    const imgs = w.document.querySelectorAll('#FILE_OTHER_List .ImageIcon img');
    const src = imgs[0] ? imgs[0].getAttribute('src') || '' : '';
    check('ConstructFileHtml image path escapes quotes and <',
        imgs.length === 1 && !imgs[0].hasAttribute('onclick') && !imgs[0].hasAttribute('onerror')
        && src.indexOf('"') >= 0 && src.indexOf('<') >= 0);
}

{
    const w = loadProjectPage();
    w.ShowModelInfo({
        name: 'n',
        author: 'a',
        upload_type: 'origin',
        license: 'CC0',
        description: HOSTILE_DESC,
        preview_img: []
    });
    check('ShowModelInfo description is sanitized', descIsSanitized(w.document.getElementById('Model_Desc')));
}

{
    const w = loadProjectPage();
    w.ShowProfilelInfo({
        name: 'n',
        author: 'a',
        description: HOSTILE_DESC,
        preview_img: []
    });
    check('ShowProfilelInfo description is sanitized', descIsSanitized(w.document.getElementById('Profile_Desc')));
}

{
    const w = loadProjectPage();
    w.ShowModelInfo({
        name: '<img src=x onerror=alert(1)>Evil',
        author: 'a',
        upload_type: 'origin',
        license: 'CC0',
        description: 'd',
        preview_img: []
    });
    const el = w.document.getElementById('ModelName');
    check('ShowModelInfo ModelName renders as text',
        !!el && el.children.length === 0 && !el.querySelector('img')
        && el.textContent.indexOf('<img') >= 0 && el.textContent.indexOf('Evil') >= 0);
}

{
    const dark = loadDarkModePage();
    const href = './css/dark.css';
    check('light mode chrome starts with dark.css', dark.w.CheckCssLinkExist(href) > 0);
    dark.w.SwitchDarkMode(href);
    check('SwitchDarkMode registers a dark-mode interval', dark.intervalFns.length >= 1);
    const putBack = dark.w.document.createElement('link');
    putBack.setAttribute('href', href);
    putBack.rel = 'stylesheet';
    dark.w.document.head.appendChild(putBack);
    check('dark.css can be put back after the first pass', dark.w.CheckCssLinkExist(href) > 0);
    dark.intervalFns[dark.intervalFns.length - 1]();
    check('dark-mode interval removes dark.css in light mode', dark.w.CheckCssLinkExist(href) === 0);
}

console.log(passed + ' passed, ' + failures + ' failed');
if (failures) process.exit(1);
