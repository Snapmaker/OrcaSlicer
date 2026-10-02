// EdgeSlicer: dark mode for the Snapmaker web pages (resources/web/flutter_web: the U1 Device tab,
// the pre-print / pre-send page and the rest of that Flutter app).
//
// Why a filter: the Flutter app has a dark theme, but its startup code forces the light one
// (ThemeVM.toggleTheme is hard-wired to isDark=false, isSystemTheme=false), and nothing outside the
// compiled bundle can switch it back. The app renders with CanvasKit into <canvas> elements inside
// the flt-glass-pane shadow root, so there are no widgets for CSS to reach either. What is left is
// a colour filter over those canvases.
//
// The filter is an SVG colour matrix, not invert(1) hue-rotate(180deg): it flips lightness onto the
// slicer's own colours (white -> the dark window background, black -> the dark text colour, greys
// in between) and keeps each colour's hue and chroma, so blues stay blue and reds stay red. Images
// drawn into the canvas (renders, snapshots, filament colour dots) go through it as well. Platform
// views (HTML <img>/<video> that Flutter slots between its canvases) are left alone.
//
// The slicer runs this file and then window.edgeFlutterDark({bg: '#RRGGBB', fg: '#RRGGBB'}) when
// the page loads and when the theme changes (WebView::ApplyFlutterTheme);
// window.edgeFlutterDark(null) takes everything off again. Safe to run more than once.
(function () {
    'use strict';
    if (window.edgeFlutterDark)
        return;

    var STYLE = 'edge-dark-style';
    var SVG = 'edge-dark-svg';
    var FILTER = 'edge-dark-filter';
    var NS = 'http://www.w3.org/2000/svg';
    var LUMA = [0.2126, 0.7152, 0.0722];

    var state = null;
    var observer = null;

    function rgb(hex) {
        var m = /^#?([0-9a-f]{6})$/i.exec(String(hex || ''));
        if (!m)
            return null;
        var n = parseInt(m[1], 16);
        return [((n >> 16) & 255) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
    }

    // out = c + B * luma(c) + A per channel, with A = fg and B = bg - 1 - fg: white lands on bg,
    // black on fg, and R - luma, B - luma (the chroma) are unchanged before clamping.
    function matrix(bg, fg) {
        var rows = [];
        for (var i = 0; i < 3; ++i) {
            var b = bg[i] - 1 - fg[i];
            var row = [];
            for (var j = 0; j < 3; ++j)
                row.push((i === j ? 1 : 0) + b * LUMA[j]);
            row.push(0, fg[i]);
            rows.push(row.map(function (v) { return v.toFixed(4); }).join(' '));
        }
        rows.push('0 0 0 1 0');
        return rows.join(' ');
    }

    function shadow() {
        var pane = document.querySelector('flt-glass-pane');
        return pane ? pane.shadowRoot : null;
    }

    function clear(root) {
        if (!root)
            return;
        [STYLE, SVG].forEach(function (id) {
            var el = root.getElementById ? root.getElementById(id) : root.querySelector('#' + id);
            if (el)
                el.remove();
        });
    }

    function stopWaiting() {
        if (observer) {
            observer.disconnect();
            observer = null;
        }
    }

    // Returns false while Flutter has not made its view yet.
    function paint() {
        var root = shadow();
        clear(document);
        clear(root);
        if (!state) {
            stopWaiting();
            return true;
        }

        // What shows before the first frame and around the view.
        var page = document.createElement('style');
        page.id = STYLE;
        page.textContent = 'html, body { background: ' + state.bg + ' !important; }';
        (document.head || document.documentElement).appendChild(page);
        if (!root)
            return false;

        var svg = document.createElementNS(NS, 'svg');
        svg.id = SVG;
        svg.setAttribute('width', '0');
        svg.setAttribute('height', '0');
        svg.setAttribute('aria-hidden', 'true');
        svg.style.position = 'absolute';
        var filter = document.createElementNS(NS, 'filter');
        filter.id = FILTER;
        filter.setAttribute('color-interpolation-filters', 'sRGB');
        var cm = document.createElementNS(NS, 'feColorMatrix');
        cm.setAttribute('type', 'matrix');
        cm.setAttribute('values', matrix(rgb(state.bg), rgb(state.fg)));
        filter.appendChild(cm);
        svg.appendChild(filter);
        root.appendChild(svg);

        var style = document.createElement('style');
        style.id = STYLE;
        style.textContent = 'flt-canvas-container, flt-scene > canvas { filter: url(#' + FILTER + '); }';
        root.appendChild(style);
        return true;
    }

    function waitForView() {
        if (observer)
            return;
        observer = new MutationObserver(function () {
            if (shadow()) {
                stopWaiting();
                paint();
            }
        });
        observer.observe(document.documentElement, { childList: true, subtree: true });
    }

    window.edgeFlutterDark = function (cfg) {
        state = (cfg && rgb(cfg.bg) && rgb(cfg.fg)) ? { bg: cfg.bg, fg: cfg.fg } : null;
        if (!paint())
            waitForView();
    };
})();
