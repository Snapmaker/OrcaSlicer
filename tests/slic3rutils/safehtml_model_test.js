// Headless check of the project page's SafeHtml (resources/web/model/model.js).
// Loads the real EscapeHtml / SafeUrlValue / SafeHtml from that file into jsdom
// (no reimplementation). Covers the gate cases: mixed srcset, picture/source,
// whitespace-scheme ('ht tps://'), and an https happy path.
//
// Run: node tests/slic3rutils/safehtml_model_test.js
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { execSync } = require('child_process');

const MODEL_JS = path.join(__dirname, '..', '..', 'resources', 'web', 'model', 'model.js');
const source = fs.readFileSync(MODEL_JS, 'utf8');

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

function loadJsdom() {
    try {
        return require('jsdom');
    } catch (e) {
        const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'safehtml-jsdom-'));
        execSync('npm install --no-save --prefix ' + tmp + ' jsdom@24', { stdio: 'inherit' });
        return require(path.join(tmp, 'node_modules', 'jsdom'));
    }
}

const extracted = extractSafeHtmlBlock(source);
if (extracted.indexOf('function SafeUrlValue') < 0)
    throw new Error('SafeUrlValue must sit between EscapeHtml and SafeHtml so this test loads the real helper');

const { JSDOM } = loadJsdom();
const dom = new JSDOM(
    '<!DOCTYPE html><html><head></head><body></body></html><script>' + extracted + '</script>',
    { runScripts: 'dangerously', url: 'file:///resources/web/model/index.html' }
);
const SafeHtml = dom.window.SafeHtml;
if (typeof SafeHtml !== 'function') throw new Error('SafeHtml did not install on the jsdom window');

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
    const out = SafeHtml('<img src="https://cdn.example.com/a.png">');
    check('https img src is kept', /src\s*=\s*["']https:\/\/cdn\.example\.com\/a\.png["']/i.test(out));
}

{
    const out = SafeHtml('<img src="https://cdn/a.png" style="background:url(http://10.0.0.1/x.png)">');
    check('style url() is dropped', !/style/i.test(out) && !/10\.0\.0\.1/.test(out));
}

{
    const out = SafeHtml('<svg><image href="http://10.0.0.1/x.png"></image></svg>');
    check('svg image href is dropped', !/<svg/i.test(out) && !/<image/i.test(out) && !/10\.0\.0\.1/.test(out));
}

{
    const http = SafeHtml('<video poster="http://10.0.0.1/p.jpg"></video>');
    const https = SafeHtml('<video poster="https://cdn/p.jpg"></video>');
    check('http poster is dropped', !/10\.0\.0\.1/.test(http) && !/poster/i.test(http));
    check('https poster is kept', /poster\s*=\s*["']https:\/\/cdn\/p\.jpg["']/i.test(https));
}

{
    const http = SafeHtml('<table background="http://10.0.0.1/b.jpg"></table>');
    const https = SafeHtml('<table background="https://cdn/b.jpg"></table>');
    check('http background is dropped', !/10\.0\.0\.1/.test(http) && !/background/i.test(http));
    check('https background is kept', /background\s*=\s*["']https:\/\/cdn\/b\.jpg["']/i.test(https));
}

{
    const out = SafeHtml('<track src="http://10.0.0.1/t.vtt">');
    check('track is removed', !/<track/i.test(out) && !/10\.0\.0\.1/.test(out));
}

{
    const out = SafeHtml('<a href="https://example.com" ping="http://10.0.0.1/p" longdesc="http://10.0.0.1/d">x</a>');
    check('ping is dropped', !/ping/i.test(out));
    check('longdesc is dropped', !/longdesc/i.test(out));
    check('https href is kept', /href\s*=\s*["']https:\/\/example\.com["']/i.test(out));
}

console.log(passed + ' passed, ' + failures + ' failed');
if (failures) process.exit(1);
