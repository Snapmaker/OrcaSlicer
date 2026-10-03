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
3. main.<hash>.js: the widgets that hard-code light colours in places the dark theme leaves
   bright (WIDGET_SITES below: the Device tab's "Camera" / "Control" title bars, its empty camera
   and control panels, the pre-print page's printer dropdown, image boxes, check circles and
   progress bar) take a colour of the app's own dark ColorScheme when the theme is dark:
   `ORIG` becomes `(Theme.of(ctx).colorScheme.brightness==dark ? scheme.<field> : ORIG)`, so
   light mode is untouched. The two empty-panel pictures get dark copies (*_dark.png, made with
   Pillow) that the dark branch shows instead.
4. Renames the patched main.<hash>.js and flutter_bootstrap.<hash>.js to new content hashes and
   updates every reference: HttpServer serves name.<hex-hash>.ext files as immutable, so an
   edited file under its old name could be served stale from a WebView cache forever.

Minified names change with every Flutter build, so every patch finds its place by a stable
anchor (a log string, an i18n key, an asset path) and captures the minified names around it.
Fails loudly (exit 1) when a statement or a reference is not where it is expected: the bundle
changed shape and the patch needs a look.

Usage: python scripts/patch_flutter_web_dark.py [bundle_dir]
       python scripts/patch_flutter_web_dark.py --check <main.js>   (dry run on a compiled file)
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


# ------------------------------------------------------------------ widget colours (step 3) ----

WIDGET_MARK = "/*edgeslicer:dark-widgets*/"
CRLF, LF = "\r\n", "\n"
ID = r'[A-Za-z_$][\w$]*'
KEYWORDS = {"if", "for", "while", "switch", "catch", "function", "return"}

# The dark-theme colours used, picked from the app's dark ColorScheme by value.
DARK_FIELDS = {
    "card": "#18181B",   # panels and cards (light: white)
    "strip": "#28282C",  # title bars, image boxes, tracks (light: #E4E4E7)
    "text": "#E4E4E7",   # body text
    "subtext": "#C1C1C1",
}

# The two empty-panel pictures that are white all over.
DARK_PICTURES = {
    "assets/images/deviceNotConnected.webp": "assets/images/deviceNotConnected_dark.png",
    "assets/images/controlDefault.png": "assets/images/controlDefault_dark.png",
}


class Names:
    """The minified names the patches need, found through stable anchors."""

    def __init__(self, js):
        self.js = js
        # A Snapmaker helper: isDark(){var s=$.ctx; if(s==null)return!1; return A.themeOf(s).scheme.brightness===B.dark}
        m = re.search(r'\n(%s)\(\)\{var s=\$\.(%s)\s*if\(s==null\)return!1\s*return A\.(%s)\(s\)\.(%s)\.(%s)===B\.(%s)\}'
                      % (ID, ID, ID, ID, ID, ID), js)
        if not m:
            fail("cannot find the isDark helper (Theme.of(ctx).colorScheme.brightness===dark)")
        self.is_dark_fn, self.global_ctx, self.theme_of, self.scheme, self.brightness, self.dark = m.groups()
        self.fields = self._dark_fields()

    def color(self, const):
        """'#RRGGBB' (alpha 1) or '#RRGGBB@a' of a B.<const>=new A.<Color>(a,r,g,b,space) constant."""
        m = re.search(r'\nB\.%s=new A\.%s\(([\d.]+),([\d.]+),([\d.]+),([\d.]+),B\.%s\)' % (re.escape(const), ID, ID), self.js)
        if not m:
            return None
        a, r, g, b = (float(x) for x in m.groups())
        hexrgb = "#%02X%02X%02X" % (round(r * 255), round(g * 255), round(b * 255))
        return hexrgb if a == 1 else "%s@%.2f" % (hexrgb, a)

    def _dark_fields(self):
        # The dark ColorScheme constant: B.x=new A.Scheme(B.<dark>, ...about 50 arguments).
        best = None
        for m in re.finditer(r'\nB\.%s=new A\.(%s)\(B\.%s,([^)]*)\)' % (ID, ID, re.escape(self.dark)), self.js):
            args = ["B." + self.dark] + m.group(2).split(",")
            if len(args) >= 40 and (best is None or len(args) > len(best[1])):
                best = (m.group(1), args)
        if not best:
            fail("cannot find the dark ColorScheme constant")
        cls, args = best
        m = re.search(r'\n%s:function %s\(([^)]*)\)\{var _=this\n(.*?)\}' % (re.escape(cls), re.escape(cls)), self.js, re.S)
        if not m:
            fail("cannot find the ColorScheme constructor " + cls)
        params = m.group(1).split(",")
        field_of = {}
        for chain in re.findall(r'((?:_\.%s=)+)(%s)\n' % (ID, ID), m.group(2) + "\n"):
            for f in re.findall(r'_\.(%s)=' % ID, chain[0]):
                field_of.setdefault(chain[1], f)
        fields = {}
        for role, want in DARK_FIELDS.items():
            for i, p in enumerate(params):
                if i < len(args) and args[i].startswith("B.") and self.color(args[i][2:]) == want and p in field_of:
                    fields[role] = field_of[p]
                    break
            else:
                fail("the dark ColorScheme has no %s colour %s" % (role, want))
        return fields

    def cond_expr(self, ctx, dark_expr, orig):
        t = "A.%s(%s).%s" % (self.theme_of, ctx, self.scheme)
        return "(%s.%s===B.%s?%s:%s)" % (t, self.brightness, self.dark, dark_expr, orig)

    def cond(self, ctx, role, orig):
        return self.cond_expr(ctx, "A.%s(%s).%s.%s" % (self.theme_of, ctx, self.scheme, self.fields[role]), orig)

    def cond_global(self, role, orig):
        """For closures without a BuildContext: the app's own global-context isDark helper."""
        t = "A.%s($.%s).%s" % (self.theme_of, self.global_ctx, self.scheme)
        return "(A.%s()?%s.%s:%s)" % (self.is_dark_fn, t, self.fields[role], orig)


def method_at(js, pos):
    """(name, first parameter, start) of the method or closure the code at pos belongs to."""
    for m in reversed(list(re.finditer(r'\n(\$?[\w$]+)\((%s)(?:,%s)*\)\{' % (ID, ID), js[:pos]))):
        if m.group(1) not in KEYWORDS:
            return m.group(1), m.group(2), m.start()
    fail("no method around offset %d" % pos)


def prototype_at(js, pos):
    """(start, end) of the A.X.prototype={...} block holding pos."""
    start = js.rfind(".prototype={", 0, pos)
    end = js.find(".prototype={", pos)
    if start < 0 or end < 0:
        fail("no prototype around offset %d" % pos)
    return js.rfind("\n", 0, start), js.rfind("\n", 0, end)


def anchor(js, text):
    i = js.find(text)
    if i < 0 or js.find(text, i + 1) >= 0:
        fail("anchor %r found %d times (expected 1)" % (text, js.count(text)))
    return i


def anchor_re(js, pattern):
    hits = list(re.finditer(pattern, js))
    if len(hits) != 1:
        fail("anchor %r found %d times (expected 1)" % (pattern, len(hits)))
    return hits[0].start()


class Patcher:
    def __init__(self, js):
        self.js = js
        self.n = Names(js)
        self.edits = []  # (start, end, replacement)

    def sub(self, start, end, pattern, make, count, what):
        """Replace `count` matches of pattern inside js[start:end] with make(match)."""
        hits = list(re.finditer(pattern, self.js[start:end]))
        if len(hits) != count:
            fail("%s: pattern found %d times (expected %d)" % (what, len(hits), count))
        for m in hits:
            self.edits.append((start + m.start(), start + m.end(), make(m, start + m.start())))

    def white(self, const):
        return self.n.color(const) == "#FFFFFF"

    def result(self):
        out, last = [], 0
        for s, e, rep in sorted(self.edits):
            if s < last:
                fail("overlapping widget patches")
            out.append(self.js[last:s])
            out.append(rep)
            last = e
        out.append(self.js[last:])
        return "".join(out)


def patch_widgets(js):
    p = Patcher(js)
    n = p.n

    def colour_is(want):
        def check(m):
            return n.color(m.group("c")[2:]) == want
        return check

    def sub_if(start, end, pattern, check, make, count, what):
        hits = [m for m in re.finditer(pattern, js[start:end]) if check(m)]
        if len(hits) != count:
            fail("%s: pattern found %d times (expected %d)" % (what, len(hits), count))
        for m in hits:
            p.edits.append((start + m.start(), start + m.end(), make(m, start + m.start())))

    # -- The Device tab's title bars ("Camera | [live] [video]", "Control  (refresh)  Print
    #    Preferences"): one widget, the one that writes the "|" between title and tabs.
    bar = anchor_re(js, r'A\.%s\("\|",' % ID)
    name, ctx, mstart = method_at(js, bar)
    mend = js.find("\n$S:", bar) if name.startswith("$") else js.find("}}\n", bar)
    # bar background (the ColorScheme colour its callers pass, light grey in both themes)
    p.sub(mstart, mend, r'new A\.(%s)\((%s\.%s),(%s),\3,new A\.' % (ID, ID, ID, ID),
          lambda m, _: "new A.%s(%s,%s,%s,new A." % (m.group(1), n.cond(ctx, "strip", m.group(2)), m.group(3), m.group(3)),
          1, "title bar background")
    # title text (#333333) and the "|" (#666666)
    sub_if(mstart, mend, r'A\.(?P<f>%s)\((?P<r>%s),(?P=r),(?P<c>B\.%s),' % (ID, ID, ID), colour_is("#333333"),
           lambda m, _: "A.%s(%s,%s,%s," % (m.group("f"), m.group("r"), m.group("r"), n.cond(ctx, "text", m.group("c"))),
           1, "title bar text")
    sub_if(mstart, mend, r'A\.(?P<f>%s)\((?P<r>%s),(?P=r),(?P<c>B\.%s),' % (ID, ID, ID), colour_is("#666666"),
           lambda m, _: "A.%s(%s,%s,%s," % (m.group("f"), m.group("r"), m.group("r"), n.cond(ctx, "subtext", m.group("c"))),
           1, "title bar separator")
    # the selected tab's white chip, in the tab closure right after the widget (no BuildContext there)
    sub_if(mend, mend + 2000, r'(?P<v>%s)=(?P=v)\?(?P<c>B\.%s):(?P<o>B\.%s)\n' % (ID, ID, ID), colour_is("#FFFFFF"),
           lambda m, _: "%s=%s?%s:%s\n" % (m.group("v"), m.group("v"), n.cond_global("card", m.group("c")), m.group("o")),
           1, "selected tab chip")

    # -- The Camera bar's two tab icons (SVG, #242424 baked in): tint them in the dark theme.
    #    The same for every bar's tab icons (the "Printing Task" bar too): all are new TabItem(svg(...)).
    m = re.search(r'new A\.(%s)\(A\.(%s)\("assets/svgs/device/liveCamera\.svg",' % (ID, ID), js)
    if not m:
        fail("Camera bar tab icon not found")
    item, svg_fn = m.groups()
    icons = 0
    for m in re.finditer(r'new A\.%s\(A\.%s\("(assets/svgs/[^"]+)",(%s),' % (re.escape(item), re.escape(svg_fn), ID), js):
        mname, c, _ = method_at(js, m.start())
        if mname not in ("G", "$3"):
            fail("tab icon %s outside a build method (%s)" % (m.group(1), mname))
        p.edits.append((m.start(), m.end(), 'new A.%s(A.%s("%s",%s,' % (item, svg_fn, m.group(1), n.cond(c, "text", m.group(2)))))
        icons += 1
    if icons < 2:
        fail("tab icons: found %d (expected the Camera bar's two at least)" % icons)

    # -- The sidebar's "Unconnected device" chevron (black54).
    at = anchor_re(js, r'A\.%s\("Unconnected device"' % ID)
    _, c, ms = method_at(js, at)
    sub_if(at, js.find("\n$S:", at), r'(?P<pre>A\.%s\(%s\.%s\?B\.%s:B\.%s,)(?P<c>B\.%s),' % (ID, ID, ID, ID, ID, ID), colour_is("#000000@0.54"),
           lambda m, _: "%s%s," % (m.group("pre"), n.cond(c, "subtext", m.group("c"))),
           1, "sidebar chevron")

    # -- The empty camera and control panels: pictures with a white background baked in. Every
    #    use of the picture (or of a constant wrapping it) gets the dark copy in the dark theme.
    for light, dark in DARK_PICTURES.items():
        m = re.search(r'\nB\.(%s)=new A\.(%s)\("%s",([^\n]*)\)\n' % (ID, ID, re.escape(light)), js)
        if not m:
            fail("no constant for " + light)
        pic, pic_cls, pic_args = m.groups()
        dark_pic = 'new A.%s("%s",%s)' % (pic_cls, dark, pic_args)
        exprs = {pic: dark_pic}
        for w in re.finditer(r'\nB\.(%s)=new A\.(%s)\(([^\n]*?)\bB\.%s\b([^\n]*)\)\n' % (ID, ID, re.escape(pic)), js):
            exprs[w.group(1)] = "new A.%s(%s%s%s)" % (w.group(2), w.group(3), dark_pic, w.group(4))
        uses = 0
        for const, expr in exprs.items():
            for u in re.finditer(r'\bB\.%s\b(?!=new)' % re.escape(const), js):
                line = js.rfind("\n", 0, u.start()) + 1
                if re.match(r'B\.%s=new ' % ID, js[line:line + 80]):
                    continue  # a constant definition (its own or a wrapper's), not a use
                mname, uctx, _ = method_at(js, u.start())
                if mname not in ("G", "$3"):
                    fail("%s used outside a build method (%s)" % (light, mname))
                p.edits.append((u.start(), u.end(), n.cond_expr(uctx, expr, "B." + const)))
                uses += 1
        if uses == 0:
            fail("%s is never used" % light)

    # -- Pre-print page, "Select Printer": the dropdown button and its menu are white, the arrow
    #    black, the printer picture box light grey.
    sel = anchor_re(js, r'A\.%s\("Click to select printer"' % ID)
    _, ctx, mstart = method_at(js, sel)
    mend = js.find("\n$S:", sel)
    sub_if(mstart, mend, r'new A\.(?P<k>%s)\((?P<c>B\.%s),(?P<r>%s),' % (ID, ID, ID), colour_is("#FFFFFF"),
           lambda m, _: "new A.%s(%s,%s," % (m.group("k"), n.cond(ctx, "card", m.group("c")), m.group("r")),
           2, "printer dropdown background")
    sub_if(mstart, mend, r'new A\.(?P<k>%s)\((?P<c>B\.%s),(?P<r>%s),(?P=r),' % (ID, ID, ID), colour_is("#EEEEEE"),
           lambda m, _: "new A.%s(%s,%s,%s," % (m.group("k"), n.cond(ctx, "strip", m.group("c")), m.group("r"), m.group("r")),
           1, "printer picture box")
    sub_if(mstart, mend, r'A\.(?P<f>%s)\((?P<c>B\.%s),-1,1\)' % (ID, ID), colour_is("#EEEEEE"),
           lambda m, _: "A.%s(%s,-1,1)" % (m.group("f"), n.cond(ctx, "strip", m.group("c"))),
           1, "printer dropdown border")
    sub_if(mstart, mend, r'(?P<pre>A\.%s\(%s\.%s\?B\.%s:B\.%s,)(?P<c>B\.%s),' % (ID, ID, ID, ID, ID, ID), colour_is("#000000"),
           lambda m, _: "%s%s," % (m.group("pre"), n.cond(ctx, "text", m.group("c"))),
           1, "printer dropdown arrow")

    # -- Pre-print page, "Model Information": the thumbnail boxes (light grey).
    s, e = prototype_at(js, anchor_re(js, r'A\.%s\("Model Information"' % ID))
    for m in [m for m in re.finditer(r'new A\.(?P<k>%s)\((?P<c>B\.%s),(?P<r>%s),(?P=r),' % (ID, ID, ID), js[s:e])
              if n.color(m.group("c")[2:]) == "#EEEEEE"]:
        _, c, _ = method_at(js, s + m.start())
        p.edits.append((s + m.start(), s + m.end(),
                        "new A.%s(%s,%s,%s," % (m.group("k"), n.cond(c, "strip", m.group("c")), m.group("r"), m.group("r"))))
    if not any(s <= ed[0] < e for ed in p.edits):
        fail("model thumbnail boxes: nothing found")

    # -- Pre-print page, "Print Preferences": the unchecked circles are white.
    s, e = prototype_at(js, anchor_re(js, r'A\.%s\("Time-lapse Camera"' % ID))
    hits = [m for m in re.finditer(r'(?P<v>%s)=c\?(?P<on>B\.%s):(?P<c>B\.%s)\n' % (ID, ID, ID), js[s:e]) if p.white(m.group("c")[2:])]
    if len(hits) != 1:
        fail("preference circles: pattern found %d times (expected 1)" % len(hits))
    m = hits[0]
    _, c, _ = method_at(js, s + m.start())
    p.edits.append((s + m.start(), s + m.end(), "%s=c?%s:%s\n" % (m.group("v"), m.group("on"), n.cond(c, "card", m.group("c")))))

    # -- Pre-print page: the upload progress bar's track (light grey), next to the Send button.
    send = anchor_re(js, r'A\.%s\(A\.%s\("Send",' % (ID, ID))
    s = js.find(".prototype={", send)
    e = js.find(".prototype={", s + 1)
    hits = [m for m in re.finditer(r'A\.(?P<f>%s)\((?P<c>B\.%s),(?P<r>%s),(?P=r),8,' % (ID, ID, ID), js[s:e]) if n.color(m.group("c")[2:]) == "#EEEEEE"]
    if len(hits) != 1:
        fail("progress bar track: pattern found %d times (expected 1)" % len(hits))
    m = hits[0]
    _, c, _ = method_at(js, s + m.start())
    p.edits.append((s + m.start(), s + m.end(), "A.%s(%s,%s,%s,8," % (m.group("f"), n.cond(c, "strip", m.group("c")), m.group("r"), m.group("r"))))

    # The marker rides on the first edit; it only has to survive.
    p.edits.sort()
    s0, e0, r0 = p.edits[0]
    p.edits[0] = (s0, e0, WIDGET_MARK + r0)
    print("widgets: %d colour patches (dark scheme fields %s)" % (len(p.edits), n.fields))
    return p.result()


def dark_picture(src, dst, names_bg="#18181B", names_fg="#FCFCFC"):
    """A dark copy of a light placeholder picture: white -> the dark card colour, black -> the dark
    text colour, hue and chroma kept (the luma flip the slicer once used as a whole-page filter)."""
    try:
        from PIL import Image
    except ImportError:
        fail("Pillow is needed to make %s (pip install pillow)" % os.path.basename(dst))
    bg = [int(names_bg[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    fg = [int(names_fg[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    im = Image.open(src).convert("RGBA")
    px = im.load()
    for y in range(im.size[1]):
        for x in range(im.size[0]):
            r, g, b, a = px[x, y]
            c = (r / 255, g / 255, b / 255)
            luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]
            out = [min(1, max(0, c[i] + (bg[i] - 1 - fg[i]) * luma + fg[i])) for i in range(3)]
            px[x, y] = (round(out[0] * 255), round(out[1] * 255), round(out[2] * 255), a)
    im.save(dst, optimize=True)


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


def check(path):
    """Dry run of the main.js patches on a compiled file (e.g. a newer Snapmaker bundle)."""
    js = read(path)
    if MARK not in js and len(list(TOGGLE.finditer(js))) != 1:
        fail("toggleTheme statement not found in " + path)
    if WIDGET_MARK not in js:
        patch_widgets(js.replace(CRLF, LF))
    print("ok: every patch finds its place in " + path)


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--check":
        check(sys.argv[2])
        return
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

    # 3. Widget colours, and the dark copies of the empty-panel pictures they show.
    if WIDGET_MARK in main_text:
        print("%s: widget colours already patched" % main_js)
    else:
        # The patterns are written for LF; git may have checked the bundle out with CRLF.
        crlf = CRLF in main_text
        main_text = patch_widgets(main_text.replace(CRLF, LF))
        if crlf:
            main_text = main_text.replace(LF, CRLF)
    for light, dark in DARK_PICTURES.items():
        src = os.path.join(bundle, "assets", *light.split("/"))
        dst = os.path.join(bundle, "assets", *dark.split("/"))
        if not os.path.isfile(dst):
            if not os.path.isfile(src):
                fail("missing picture " + src)
            dark_picture(src, dst)
            print("made " + dark)

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
