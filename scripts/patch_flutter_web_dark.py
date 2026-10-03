#!/usr/bin/env python3
"""Let the Snapmaker web app (resources/web/flutter_web) follow EdgeSlicer's dark mode.

Re-run this after every update of the Snapmaker web bundle (docs/themes.md, "Snapmaker web
pages"). It is idempotent: on an already patched bundle it only checks the result.

The bundle is Snapmaker's compiled Flutter app (the U1 Device tab, the pre-print / pre-send
pages). It ships a light and a dark theme, but its startup code calls ThemeVM.toggleTheme, which
is hard-wired to isDark=false, isSystemTheme=false, so the app is always light. This script:

1. main.<hash>.js: makes that one statement set isSystemTheme=true. The MaterialApp then uses
   ThemeMode.system, i.e. the app's own dark theme whenever "(prefers-color-scheme: dark)"
   matches, and Flutter follows changes of that query live.
2. index.html: adds a small script ahead of the app that answers that query from the page's
   dark_mode=1|0 URL parameter (GUI_App::get_international_url passes the slicer's dark mode)
   and exposes window.edgeSetDarkMode(bool), which the slicer calls on page loads and theme
   changes (WebView::ApplyFlutterTheme) to switch an open page without reloading it.
3. Renames the patched main.<hash>.js and flutter_bootstrap.<hash>.js to new content hashes and
   updates every reference: HttpServer serves name.<hex-hash>.ext files as immutable, so an
   edited file under its old name could be served stale from a WebView cache forever.

Fails loudly (exit 1) when the toggleTheme statement or a reference is not where it is expected:
the bundle changed shape and the patch needs a look.

Usage: python scripts/patch_flutter_web_dark.py [bundle_dir]
"""

import hashlib
import os
import re
import sys

MARK = "/*edgeslicer:system-theme*/"
SHIM_MARK = "edgeslicer:dark-mode"

# q.a=q.b=!1  A.Hx("[ThemeVM] toggleTheme, isDark: false, isSystemTheme: false")  if(q.a)...
# The field tested by the if() right after the log call is isSystemTheme; the other is isDark.
TOGGLE = re.compile(
    r'(?P<o>[A-Za-z_$][\w$]*)\.(?P<sys>[\w$]+)=(?P=o)\.(?P<dark>[\w$]+)=!1'
    r'(?P<tail>\s*A\.[\w$]+\("\[ThemeVM\] toggleTheme, isDark: false, isSystemTheme: false"\)\s*if\((?P=o)\.(?P=sys)\))'
)

SHIM = """<script>/* {mark}: written by scripts/patch_flutter_web_dark.py.
  The app follows "(prefers-color-scheme: dark)" (ThemeMode.system). Answer that query from the
  slicer's dark_mode=1|0 URL parameter, and let the slicer switch it live:
  window.edgeSetDarkMode(true|false). Other media queries go to the browser. */
(function () {
  var Q = '(prefers-color-scheme: dark)';
  var real = window.matchMedia ? window.matchMedia.bind(window) : null;
  var m = /[?&]dark_mode=([01])\\b/.exec(location.search + '&' + location.hash);
  var dark = m ? m[1] === '1' : !!(real && real(Q).matches);
  var listeners = [];
  var mql = {
    media: Q,
    onchange: null,
    get matches() { return dark; },
    addListener: function (f) { if (typeof f === 'function') listeners.push(f); },
    removeListener: function (f) { listeners = listeners.filter(function (g) { return g !== f; }); },
    addEventListener: function (t, f) { if (t === 'change') this.addListener(f); },
    removeEventListener: function (t, f) { if (t === 'change') this.removeListener(f); },
    dispatchEvent: function () { return true; }
  };
  window.matchMedia = function (q) {
    return String(q).replace(/\\s+/g, ' ').trim() === Q ? mql : real(q);
  };
  window.edgeSetDarkMode = function (d) {
    d = !!d;
    document.documentElement.setAttribute('data-theme', d ? 'dark' : 'light');
    if (d === dark) return;
    dark = d;
    var ev = { matches: d, media: Q };
    listeners.slice().forEach(function (f) { try { f.call(mql, ev); } catch (e) {} });
    if (typeof mql.onchange === 'function') mql.onchange(ev);
  };
})();
</script>"""


def fail(msg):
    print("patch_flutter_web_dark: " + msg, file=sys.stderr)
    sys.exit(1)


def read(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def content_hash(text):
    # Line endings normalised: git checks the bundle out with CRLF on Windows and LF elsewhere.
    return hashlib.sha256(text.replace("\r\n", "\n").encode("utf-8")).hexdigest()[:16]


def one_name(pattern, text, what):
    names = sorted(set(re.findall(pattern, text)))
    if len(names) != 1:
        fail("expected one %s reference, found %s" % (what, names or "none"))
    return names[0]


def rename(bundle, old, new, text):
    """Write text as `new` and drop `old` (or rewrite `old` in place when the name stays)."""
    if old == new:
        if read(os.path.join(bundle, old)) != text:
            write(os.path.join(bundle, old), text)
        return
    write(os.path.join(bundle, new), text)
    os.remove(os.path.join(bundle, old))
    print("renamed %s -> %s" % (old, new))


def replace_refs(bundle, old, new):
    """Replace `old` with `new` in every text file at the top of the bundle."""
    for name in sorted(os.listdir(bundle)):
        path = os.path.join(bundle, name)
        if not os.path.isfile(path) or not name.endswith((".js", ".html", ".json")):
            continue
        text = read(path)
        if old in text:
            write(path, text.replace(old, new))
            print("  %s: now names %s" % (name, new))


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bundle = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "resources", "web", "flutter_web")
    index_path = os.path.join(bundle, "index.html")
    if not os.path.isfile(index_path):
        fail("no index.html in " + bundle)

    index = read(index_path)
    boot = one_name(r'flutter_bootstrap\.[0-9a-f]{16,}\.js', index, "flutter_bootstrap.<hash>.js in index.html")
    main_js = one_name(r'main\.[0-9a-f]{16,}\.js', read(os.path.join(bundle, boot)), "main.<hash>.js in " + boot)
    if main_js not in index:
        fail("index.html does not preload " + main_js)

    # 1. The toggleTheme statement.
    main_text = read(os.path.join(bundle, main_js))
    if MARK in main_text:
        print("%s: already patched" % main_js)
    else:
        hits = list(TOGGLE.finditer(main_text))
        if len(hits) != 1:
            fail("toggleTheme statement found %d times in %s (expected 1); the bundle changed, see "
                 "the TOGGLE pattern in this script" % (len(hits), main_js))
        main_text = TOGGLE.sub(lambda m: "%s.%s=!0,%s.%s=!1%s%s" % (
            m["o"], m["sys"], m["o"], m["dark"], MARK, m["tail"]), main_text)
        print("%s: toggleTheme now keeps isSystemTheme=true" % main_js)

    # 3. New content hashes, main first (the bootstrap names it, so its hash follows).
    new_main = "main.%s.js" % content_hash(main_text)
    rename(bundle, main_js, new_main, main_text)
    if new_main != main_js:
        replace_refs(bundle, main_js, new_main)
    boot_text = read(os.path.join(bundle, boot))
    new_boot = "flutter_bootstrap.%s.js" % content_hash(boot_text)
    rename(bundle, boot, new_boot, boot_text)
    if new_boot != boot:
        replace_refs(bundle, boot, new_boot)

    # 2. The media query script, ahead of every other script.
    index = read(index_path)
    if SHIM_MARK in index:
        print("index.html: dark mode script already there")
    else:
        if index.count("</head>") != 1:
            fail("index.html: expected one </head>")
        if -1 < index.find("<script") < index.find("</head>"):
            fail("index.html: a script runs in <head>; put the dark mode script ahead of it by hand")
        nl = "\r\n" if "\r\n" in index else "\n"
        shim = SHIM.replace("{mark}", SHIM_MARK).replace("\n", nl)
        write(index_path, index.replace("</head>", shim + "</head>"))
        print("index.html: dark mode script added")

    # Check the result.
    index = read(index_path)
    boot = one_name(r'flutter_bootstrap\.[0-9a-f]{16,}\.js', index, "flutter_bootstrap.<hash>.js in index.html")
    boot_text = read(os.path.join(bundle, boot))
    main_js = one_name(r'main\.[0-9a-f]{16,}\.js', boot_text, "main.<hash>.js in " + boot)
    main_text = read(os.path.join(bundle, main_js))
    if MARK not in main_text or main_js not in index or SHIM_MARK not in index:
        fail("the patched bundle does not check out")
    if main_js != "main.%s.js" % content_hash(main_text) or boot != "flutter_bootstrap.%s.js" % content_hash(boot_text):
        fail("a patched file's name does not match its content hash")
    print("ok: %s, %s" % (boot, main_js))


if __name__ == "__main__":
    main()
