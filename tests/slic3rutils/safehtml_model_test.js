// Headless check of the project page's SafeHtml (resources/web/model/model.js).
// Loads the real EscapeHtml / SafeUrlValue / SafeHtml from that file into jsdom
// (no reimplementation).
//
// Not wired into CMake/CI (Edge rule: no test CMake edits). Owed follow-up: hook this
// into CI. Pin: tests/slic3rutils/package.json + package-lock.json.
//
// Run: npm install --prefix tests/slic3rutils && node tests/slic3rutils/safehtml_model_test.js
// Missing node or jsdom: exit 0 with SKIP (so an unwired CI job does not go red).
'use strict';

function skip(why) {
    console.log('SKIP: ' + why);
    process.exit(0);
}

if (typeof process === 'undefined' || !process.versions || !process.versions.node)
    skip('node is not available');

const fs = require('fs');
const path = require('path');

const MODEL_JS = path.join(__dirname, '..', '..', 'resources', 'web', 'model', 'model.js');
const JSDOM_PATH = path.join(__dirname, 'node_modules', 'jsdom');

let JSDOM;
try {
    JSDOM = require(JSDOM_PATH).JSDOM;
} catch (e) {
    skip("jsdom is not installed; run: npm install --prefix tests/slic3rutils");
}

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

const extracted = extractSafeHtmlBlock(source);
if (extracted.indexOf('function SafeUrlValue') < 0)
    throw new Error('SafeUrlValue must sit between EscapeHtml and SafeHtml so this test loads the real helper');

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
    const out = SafeHtml('<img src="https://cdn.example.com/a.png" alt="ok" width="10" height="10" title="t">');
    check('https img src is kept', /src\s*=\s*["']https:\/\/cdn\.example\.com\/a\.png["']/i.test(out));
    check('img alt/width/height/title are kept', /alt/i.test(out) && /width/i.test(out) && /height/i.test(out) && /title/i.test(out));
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

console.log(passed + ' passed, ' + failures + ' failed');
if (failures) process.exit(1);
