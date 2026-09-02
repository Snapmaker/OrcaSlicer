(()=>{var _={blink:!0,gecko:!1,webkit:!1,unknown:!1},K=()=>navigator.vendor==="Google Inc."||navigator.userAgent.includes("Edg/")?"blink":navigator.vendor==="Apple Computer, Inc."?"webkit":navigator.vendor===""&&navigator.userAgent.includes("Firefox")?"gecko":"unknown",C=K(),R=()=>typeof ImageDecoder>"u"?!1:C==="blink",B=()=>typeof Intl.v8BreakIterator<"u"&&typeof Intl.Segmenter<"u",z=()=>{let i=[0,97,115,109,1,0,0,0,1,5,1,95,1,120,0];return WebAssembly.validate(new Uint8Array(i))},M=()=>{let i=document.createElement("canvas");return i.width=1,i.height=1,i.getContext("webgl2")!=null?2:i.getContext("webgl")!=null?1:-1},D=()=>window.chrome&&chrome.runtime&&chrome.runtime.id,w={browserEngine:C,hasImageCodecs:R(),hasChromiumBreakIterators:B(),supportsWasmGC:z(),crossOriginIsolated:window.crossOriginIsolated,webGLVersion:M(),isChromeExtension:D()};function c(...i){return new URL(I(...i),document.baseURI).toString()}function I(...i){return i.filter(e=>!!e).map((e,n)=>n===0?S(e):F(S(e))).filter(e=>e.length).join("/")}function F(i){let e=0;for(;e<i.length&&i.charAt(e)==="/";)e++;return i.substring(e)}function S(i){let e=i.length;for(;e>0&&i.charAt(e-1)==="/";)e--;return i.substring(0,e)}function E(i,e){return i.canvasKitBaseUrl?i.canvasKitBaseUrl:e.engineRevision&&!e.useLocalCanvasKit?I("https://www.gstatic.com/flutter-canvaskit",e.engineRevision):"canvaskit"}var v=class{constructor(){this._scriptLoaded=!1}setTrustedTypesPolicy(e){this._ttPolicy=e}async loadEntrypoint(e){let{entrypointUrl:n=c("main.dart.js"),onEntrypointLoaded:t,nonce:r}=e||{};return this._loadJSEntrypoint(n,t,r)}async load(e,n,t,r,a){a??=l=>{l.initializeEngine(t).then(u=>u.runApp())};let{entrypointBaseUrl:s}=t,{entryPointBaseUrl:o}=t;if(!s&&o&&(console.warn("[deprecated] `entryPointBaseUrl` is deprecated and will be removed in a future release. Use `entrypointBaseUrl` instead."),s=o),e.compileTarget==="dart2wasm")return this._loadWasmEntrypoint(e,n,s,a);{let l=e.mainJsPath??"main.dart.js",u=c(s,l);return this._loadJSEntrypoint(u,a,r)}}didCreateEngineInitializer(e){typeof this._didCreateEngineInitializerResolve=="function"&&(this._didCreateEngineInitializerResolve(e),this._didCreateEngineInitializerResolve=null,delete _flutter.loader.didCreateEngineInitializer),typeof this._onEntrypointLoaded=="function"&&this._onEntrypointLoaded(e)}_loadJSEntrypoint(e,n,t){let r=typeof n=="function";if(!this._scriptLoaded){this._scriptLoaded=!0;let a=this._createScriptTag(e,t);if(r)console.debug("Injecting <script> tag. Using callback."),this._onEntrypointLoaded=n,document.head.append(a);else return new Promise((s,o)=>{console.debug("Injecting <script> tag. Using Promises. Use the callback approach instead!"),this._didCreateEngineInitializerResolve=s,a.addEventListener("error",o),document.head.append(a)})}}async _loadWasmEntrypoint(e,n,t,r){if(!this._scriptLoaded){this._scriptLoaded=!0,this._onEntrypointLoaded=r;let{mainWasmPath:a,jsSupportRuntimePath:s}=e,o=c(t,a),l=c(t,s);this._ttPolicy!=null&&(l=this._ttPolicy.createScriptURL(l));let d=(await import(l)).compileStreaming(fetch(o)),p;e.renderer==="skwasm"?p=(async()=>{let h=await n.skwasm;return window._flutter_skwasmInstance=h,{skwasm:h.wasmExports,skwasmWrapper:h,ffi:{memory:h.wasmMemory}}})():p=Promise.resolve({}),await(await(await d).instantiate(await p,{loadDynamicModule:async(h,j)=>{let A=fetch(c(t,h)),L=c(t,j);this._ttPolicy!=null&&(L=this._ttPolicy.createScriptURL(L));let x=import(L);return[await A,await x]}})).invokeMain()}}_createScriptTag(e,n){let t=document.createElement("script");t.type="application/javascript",n&&(t.nonce=n);let r=e;return this._ttPolicy!=null&&(r=this._ttPolicy.createScriptURL(e)),t.src=r,t}};async function T(i,e,n){if(e<0)return i;let t,r=new Promise((a,s)=>{t=setTimeout(()=>{s(new Error(`${n} took more than ${e}ms to resolve. Moving on.`,{cause:T}))},e)});return Promise.race([i,r]).finally(()=>{clearTimeout(t)})}var g=class{setTrustedTypesPolicy(e){this._ttPolicy=e}loadServiceWorker(e){if(!e||!("serviceWorker"in navigator))return Promise.resolve();let n=()=>{console.warn(`Loading the service worker using Flutter bootstrap is deprecated and will stop working in a future release.
For more details, see: https://github.com/flutter/flutter/issues/156910`)},t=()=>{let{serviceWorkerVersion:r,serviceWorkerUrl:a=c(`flutter_service_worker.js?v=${r}`),timeoutMillis:s=4e3}=e,o=a;this._ttPolicy!=null&&(o=this._ttPolicy.createScriptURL(o));let l=navigator.serviceWorker.register(o).then(u=>this._getNewServiceWorker(u,r)).then(this._waitForServiceWorkerActivation);return T(l,s,"prepareServiceWorker")};return e.serviceWorkerUrl!=null?(n(),t()):navigator.serviceWorker.getRegistration().then(r=>r?t():Promise.resolve())}async _getNewServiceWorker(e,n){if(!e.active&&(e.installing||e.waiting))return console.debug("Installing/Activating first service worker."),e.installing||e.waiting;if(e.active.scriptURL.endsWith(n))return console.debug("Loading from existing service worker."),e.active;{let t=await e.update();return console.debug("Updating service worker."),t.installing||t.waiting||t.active}}async _waitForServiceWorkerActivation(e){if(!e||e.state==="activated")if(e){console.debug("Service worker already active.");return}else throw new Error("Cannot activate a null service worker!");return new Promise((n,t)=>{e.addEventListener("statechange",()=>{e.state==="activated"&&(console.debug("Activated new service worker."),n())})})}};var y=class{constructor(e,n="flutter-js"){let t=e||[/\.js$/,/\.mjs$/];window.trustedTypes&&(this.policy=trustedTypes.createPolicy(n,{createScriptURL:function(r){if(r.startsWith("blob:"))return r;let a=new URL(r,window.location),s=a.pathname.split("/").pop();if(t.some(l=>l.test(s)))return a.toString();console.error("URL rejected by TrustedTypes policy",n,":",r,"(download prevented)")}}))}};var k=i=>{let e=WebAssembly.compileStreaming(fetch(i));return(n,t)=>((async()=>{let r=await e,a=await WebAssembly.instantiate(r,n);t(a,r)})(),{})};var U=(i,e,n,t)=>(window.flutterCanvasKitLoaded=(async()=>{if(window.flutterCanvasKit)return window.flutterCanvasKit;let r=n.hasChromiumBreakIterators&&n.hasImageCodecs;if(!r&&e.canvasKitVariant=="chromium")throw"Chromium CanvasKit variant specifically requested, but unsupported in this browser";let a=r&&e.canvasKitVariant!=="full",s=t;e.canvasKitVariant=="experimentalWebParagraph"?s=c(s,"experimental_webparagraph"):a&&(s=c(s,"chromium"));let o=c(s,"canvaskit.js");i.flutterTT.policy&&(o=i.flutterTT.policy.createScriptURL(o));let l=k(c(s,"canvaskit.wasm")),u=await import(o);return window.flutterCanvasKit=await u.default({instantiateWasm:l}),window.flutterCanvasKit})(),window.flutterCanvasKitLoaded);var W=async(i,e,n,t)=>{let a=!n.hasImageCodecs||!n.hasChromiumBreakIterators?"skwasm_heavy":e.enableWimp?"wimp":"skwasm",s=c(t,`${a}.js`),o=s;i.flutterTT.policy&&(o=i.flutterTT.policy.createScriptURL(o));let l=k(c(t,`${a}.wasm`));return await(await import(o)).default({skwasmSingleThreaded:e.enableWimp||!n.crossOriginIsolated||n.isChromeExtension||e.forceSingleThreadedSkwasm,instantiateWasm:l,locateFile:(d,p)=>d.endsWith(".ww.js")?URL.createObjectURL(new Blob([`
"use strict";

let eventListener;
eventListener = (message) => {
    const pendingMessages = [];
    const data = message.data;
    data["instantiateWasm"] = (info,receiveInstance) => {
        const instance = new WebAssembly.Instance(data["wasm"], info);
        return receiveInstance(instance, data["wasm"])
    };
    import(data.js).then(async (skwasm) => {
        await skwasm.default(data);

        removeEventListener("message", eventListener);
        for (const message of pendingMessages) {
            dispatchEvent(message);
        }
    });
    removeEventListener("message", eventListener);
    eventListener = (message) => {

        pendingMessages.push(message);
    };

    addEventListener("message", eventListener);
};
addEventListener("message", eventListener);
`],{type:"application/javascript"})):c(t,d),mainScriptUrlOrBlob:s})};var P=w.supportsWasmGC,G=P&&w.webGLVersion>0,b=class{async loadEntrypoint(e){let{serviceWorker:n,...t}=e||{},r=new y,a=new g;a.setTrustedTypesPolicy(r.policy),await a.loadServiceWorker(n).catch(o=>{console.warn("Exception while loading service worker:",o)});let s=new v;return s.setTrustedTypesPolicy(r.policy),this.didCreateEngineInitializer=s.didCreateEngineInitializer.bind(s),s.loadEntrypoint(t)}async load({serviceWorkerSettings:e,onEntrypointLoaded:n,nonce:t,config:r}={}){r??={};let a=_flutter.buildConfig;if(!a)throw"FlutterLoader.load requires _flutter.buildConfig to be set";let s=r.wasmAllowList?.[w.browserEngine]??_[w.browserEngine],o=m=>{switch(m){case"skwasm":return G&&s;default:return!0}},l=m=>m.compileTarget==="dart2wasm"&&!P||r.renderer&&r.renderer!=m.renderer?!1:o(m.renderer),u=a.builds.find(l);if(!u)throw"FlutterLoader could not find a build compatible with configuration and environment.";let d={};d.flutterTT=new y,e&&(d.serviceWorkerLoader=new g,d.serviceWorkerLoader.setTrustedTypesPolicy(d.flutterTT.policy),await d.serviceWorkerLoader.loadServiceWorker(e).catch(m=>{console.warn("Exception while loading service worker:",m)}));let p=E(r,a);u.renderer==="canvaskit"?d.canvasKit=U(d,r,w,p):u.renderer==="skwasm"&&(d.skwasm=W(d,r,w,p));let f=new v;return f.setTrustedTypesPolicy(d.flutterTT.policy),this.didCreateEngineInitializer=f.didCreateEngineInitializer.bind(f),f.load(u,d,r,t,n)}};window._flutter||(window._flutter={});window._flutter.loader||(window._flutter.loader=new b);})();
//# sourceMappingURL=flutter.js.map

if (!window._flutter) {
  window._flutter = {};
}
_flutter.buildConfig = {"engineRevision":"42d3d75a56efe1a2e9902f52dc8006099c45d937","builds":[{"compileTarget":"dart2js","renderer":"canvaskit","mainJsPath":"main.d60eb566ddd3eda0.js"},{}]};


// =============================================================================
// Orca web bootstrap.
//
// This file is the single source of truth for the Flutter web loading flow.
// `flutter build web` copies it to build/web/flutter_bootstrap.js, substituting
// the two build tokens at the very top of this file (flutter_js and
// flutter_build_config): the first makes `_flutter.loader` available, the second
// sets `_flutter.buildConfig` (which FlutterLoader.load requires — that is why
// .load() must live here, not in index.html with a bare flutter.js).
//
// IMPORTANT: never type the literal double-brace tokens inside a comment. The
// build substitutes them EVERYWHERE in the file, including comments — doing so
// injects the loader code mid-comment, and since the loader contains a
// multi-line template literal, the comment's `//` only covers the first line,
// turning the rest into live JS and breaking parsing.
//
// Edit THIS file, not the built output.
//
// index.html only need:
//   <script src="flutter_overlay.js"></script>        (pre-Flutter loading/error overlay)
//   <script src="flutter_bootstrap.js" async></script>
// (tool/hash_web_assets.mjs rewrites those src to content-hashed
// flutter_overlay.<hash>.js / flutter_bootstrap.<hash>.js for immutable caching.)
// =============================================================================
(function () {
  var FLUTTER_LOAD_TIMEOUT_MS = 120000; // 120s — entrypoint download phase
  var FLUTTER_FIRST_FRAME_TIMEOUT_MS = 120000; // 120s — first-frame watchdog after entrypoint loaded
  var entrypointReady = false;
  var loadingEl = document.getElementById('flutter-loading');
  var errorEl = document.getElementById('flutter-error');
  var detailEl = document.getElementById('flutter-error__detail');
  var timeoutId = null;
  var startTime = performance.now();

  function logProgress(step) {
    console.log('[Orca Web] +' + Math.round(performance.now() - startTime) + 'ms ' + step);
  }

  // Forward a log line to the native container's file log via the WCP postMessage
  // bridge (window.sendMessage -> window.wx.postMessage). The native side handles
  // the sw_FileLog cmd by appending {level, content} to its log file. Best-effort:
  // if the bridge is not yet available we only emit to the console. This lets us
  // persist resource-load / engine failures even when the Flutter UI never comes up.
  var _wcpLogSeq = 0;
  function writeWcpLog(level, message) {
    var content = '[' + new Date().toISOString() + '][' + level + '][OrcaWeb][' + message + ']';
    var packet = {
      header: { seqid: 'weblog-' + (++_wcpLogSeq) },
      payload: {
        cmd: 'sw_FileLog',
        event_id: null,
        params: { level: level, content: content },
        metadata: null
      }
    };
    try {
      var json = JSON.stringify(packet);
      if (typeof window.sendMessage === 'function') {
        window.sendMessage(json);
      } else if (window.wx && typeof window.wx.postMessage === 'function') {
        window.wx.postMessage(json);
      } else {
        console.error('[Orca Web] WCP log bridge unavailable; not sent: ' + message);
      }
    } catch (e) {
      console.error('[Orca Web] writeWcpLog failed: ' + e);
    }
  }

  logProgress('Start loading');

  // One-time migration: the previous package registered a service worker
  // (orca_service_worker.js) that is NOT shipped by this build, but a SW that was
  // registered earlier persists in the browser/WebView until it is explicitly
  // unregistered — and it keeps intercepting requests and serving its stale
  // cache. Unregister every SW and wipe every Cache Storage bucket so this build
  // is served fresh. Best-effort; can be removed once all clients have migrated.
  (function cleanupLegacyServiceWorker() {
    try {
      if ('serviceWorker' in navigator) {
        navigator.serviceWorker.getRegistrations().then(function (regs) {
          regs.forEach(function (reg) { reg.unregister(); });
        }).catch(function () {});
      }
      if (window.caches && typeof caches.keys === 'function') {
        caches.keys().then(function (keys) {
          keys.forEach(function (k) { caches.delete(k); });
        }).catch(function () {});
      }
    } catch (e) { /* ignore */ }
  })();

  // WASM MIME guard: WebAssembly.compileStreaming rejects responses whose
  // Content-Type is not application/wasm ("Unexpected response MIME type").
  // The embedded local HTTP server serves canvaskit.wasm as octet-stream, so
  // the engine's streaming compile fails at startup. Wrap compileStreaming to
  // detect a wrong/missing MIME, fall back to buffered compile (which skips the
  // MIME check), and log once for diagnosis. Must be installed before
  // _flutter.loader.load() runs.
  (function patchWasmStreaming() {
    var orig = WebAssembly.compileStreaming;
    if (typeof orig !== 'function') { return; }
    var warned = false;
    WebAssembly.compileStreaming = function (source) {
      return Promise.resolve(source).then(function (resp) {
        var ct = '';
        try { ct = (resp && resp.headers && resp.headers.get('Content-Type')) || ''; } catch (e) {}
        if (ct.indexOf('application/wasm') !== -1) {
          return orig(resp);
        }
        if (!warned) {
          warned = true;
          console.warn('[Orca Web] wasm served with MIME "' + ct + '" (expected application/wasm); using buffered compile fallback');
          writeWcpLog('warn', 'wasm MIME "' + ct + '" != application/wasm; buffered compile fallback used');
        }
        return resp.arrayBuffer().then(function (buf) {
          return WebAssembly.compile(buf);
        });
      });
    };
  })();

  // User tapped the error page's Reload button: log it to the native file log
  // (best-effort) before window.location.reload() navigates away.
  window.__orcaLogReload = function () {
    console.log('[Orca Web] Reload button clicked, reloading');
    writeWcpLog('info', 'Reload button clicked, reloading');
  };

  function showFatalError(reason) {
    if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
    if (loadingEl) loadingEl.remove();
    if (errorEl) errorEl.style.display = 'flex';
    // Error detail is intentionally NOT shown in the UI (product decision); it is
    // reported to the console and the native file log via the WCP postMessage bridge.
    // if (detailEl) detailEl.textContent = reason;
    console.error('[Orca Web] Load failed: ' + reason);
    writeWcpLog('error', 'Load failed: ' + reason);
  }

  // Capture internal errors thrown by main.js (uncaught exceptions during
  // Dart static init / main(), or unhandled promise rejections inside the
  // engine) and surface them in the error overlay instead of letting the user
  // stare at a spinner until the first-frame timeout fires. Without this, the
  // real stack lives only in the console and the overlay shows a generic
  // "first frame did not render" message.
  // Gated on loadingEl: once the first frame has rendered and the overlay is
  // removed, runtime errors are left to Flutter's own handler — we never hijack
  // a live app.
  function describeError(kind, ev) {
    var msg = '', stack = '';
    if (kind === 'error') {
      msg = (ev && ev.message) || (ev && ev.error && ev.error.message) || String(ev);
      stack = (ev && ev.error && ev.error.stack) || '';
    } else { // unhandledrejection
      var reason = ev && ev.reason;
      if (reason && reason instanceof Error) {
        msg = reason.message || String(reason);
        stack = reason.stack || '';
      } else {
        msg = (reason !== undefined && reason !== null) ? String(reason) : '<no reason>';
      }
    }
    var detail = msg || '<no message>';
    if (stack) { detail += '\n' + stack; }
    if (detail.length > 2000) { detail = detail.slice(0, 2000) + '\n…(truncated)'; }
    return detail;
  }

  function captureFatal(kind, ev) {
    if (!loadingEl) { return; } // app already up — leave runtime errors to Flutter
    var prefix = 'main.js runtime error';
    if (kind === 'error' && ev && ev.filename) {
      prefix += ' (' + ev.filename + (ev.lineno ? (':' + ev.lineno) : '') + ')';
    }
    showFatalError(prefix + ':\n' + describeError(kind, ev));
  }

  // Diagnostic snapshot for timeout reports: which stage stalled. Helps tell apart
  // "main.js never downloaded" from "downloaded but engine never started" —
  // on Windows WebView the 7MB canvaskit.wasm compile is a common stall point.
  function _loadDiag() {
    var mainJs = null;
    for (var k in _resUrls) {
      if (_isMainJs(k)) { mainJs = _resUrls[k]; break; }
    }
    var ck = _resUrls['canvaskit.wasm'];
    return 'Diag{resources=' + _resLoaded +
           ', main.js=' + (mainJs ? 'loaded(' + mainJs + 'ms)' : 'NOT loaded') +
           ', canvaskit.wasm=' + (ck ? 'loaded(' + ck + 'ms)' : 'not loaded') + '}';
  }

  // Resource loading progress: log every fetched resource (script / font / asset /
  // manifest / icon) to the console as it completes, so the sequence and timing of
  // each load is visible during diagnosis. Covers main.js, AssetManifest,
  // canvaskit.wasm, fonts, icons, etc. — including resources the engine fetches
  // internally.
  var _resLoaded = 0;
  var _resUrls = {};
  function _shortUrl(url) {
    try {
      var name = url.split('?')[0].split('#')[0].split('/').pop();
      return name ? decodeURIComponent(name) : url;
    } catch (e) { return url; }
  }
  // The entrypoint is content-hashed at build time (main.<hash>.js), so match by
  // shape instead of the literal "main.js".
  function _isMainJs(url) {
    return /^main\..+\.js$/.test(_shortUrl(url));
  }
  if (window.PerformanceObserver) {
    try {
      var resObserver = new PerformanceObserver(function (list) {
        list.getEntries().forEach(function (entry) {
          _resLoaded++;
          _resUrls[_shortUrl(entry.name)] = Math.round(entry.duration);
          logProgress('Resource [' + _resLoaded + '] ' + _shortUrl(entry.name) +
                      ' (' + Math.round(entry.duration) + 'ms, ' +
                      Math.round(entry.transferSize || 0) + 'B)');
        });
      });
      // Prefer the modern (type + buffered) form; fall back to the legacy entryTypes
      // form for older WebViews. Both are best-effort — wrapped in try/catch.
      try {
        resObserver.observe({ type: 'resource', buffered: true });
      } catch (e1) {
        resObserver.observe({ entryTypes: ['resource'] });
      }
    } catch (e) {
      logProgress('PerformanceObserver(resource) unavailable: ' + e);
    }
  }

  // Resource-load failures (img / script / link) reach the window 'error' listener
  // in the CAPTURE phase with ev.target = the element and no ev.message / ev.error.
  // JS runtime errors reach it with ev.message / ev.error set. Route them apart:
  // resource failures are logged and forwarded to the WCP file log, but do not by
  // themselves trigger the fatal page — the timeout / first-frame guards below still
  // cover the critical "main.js never loaded" case. Gated on loadingEl so a
  // running app's lazy-load failures are left to Flutter, matching captureFatal.
  function onResourceFail(target) {
    if (!loadingEl) { return; }
    var url = '';
    if (target) {
      url = target.src || target.href ||
            (target.getAttribute && target.getAttribute('src')) || '';
    }
    var msg = 'Resource failed to load: ' + (url ? _shortUrl(url) : '<unknown>');
    logProgress(msg);
    console.error('[Orca Web] ' + msg);
    writeWcpLog('error', msg);
    // The entrypoint has no cache fallback: if it fails (404 / MIME / throttled),
    // the app cannot boot. Surface the error now instead of spinning until the
    // 30s watchdog — on a throttled loopback nothing more is coming.
    if (url && _isMainJs(url) && !entrypointReady) {
      showFatalError('Entrypoint failed to load (' + _shortUrl(url) +
                     '); no cache fallback. ' + _loadDiag());
    }
  }

  window.addEventListener('error', function (ev) {
    var target = ev && ev.target;
    var isResource = target && target !== window &&
      (target.tagName === 'IMG' || target.tagName === 'SCRIPT' || target.tagName === 'LINK') &&
      !ev.message && !ev.error;
    if (isResource) {
      onResourceFail(target);
    } else {
      captureFatal('error', ev);
    }
  }, true);
  window.addEventListener('unhandledrejection', function (ev) { captureFatal('rejection', ev); });

  // 120s timeout guard: if onEntrypointLoaded has not fired, treat loading as failed and show the static error page
  timeoutId = setTimeout(function () {
    if (!entrypointReady) {
      showFatalError('Resource load timed out (' + (FLUTTER_LOAD_TIMEOUT_MS / 1000) +
                     's); onEntrypointLoaded not fired. ' + _loadDiag());
    }
  }, FLUTTER_LOAD_TIMEOUT_MS);

  // Flutter rendered its first frame (the loading overlay is closed here; this also logs and acts as a fallback)
  window.addEventListener('flutter-first-frame', function () {
    logProgress('First frame rendered (flutter-first-frame)');
    if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
    if (loadingEl) { logProgress('Closing loading overlay'); loadingEl.remove(); loadingEl = null; }
  });

  // Guard: if the FlutterLoader failed to load or was blocked (404 / MIME / CSP), report it instead of throwing a ReferenceError
  if (!window._flutter || !_flutter.loader) {
    showFatalError('FlutterLoader not loaded or blocked (check 404 / MIME type / CSP).');
    return;
  }

  // No service worker is used. The entrypoint is content-hashed at build time
  // (main.<hash>.js) and served with Cache-Control: immutable, so unchanged
  // builds are served from the browser's HTTP cache across boots without any SW.
  // canvaskit / assets / fonts are handled by the local server's cache headers.

  // Use FlutterLoader.load (the modern, non-deprecated API). _flutter.buildConfig is set by the
  // flutter_build_config token above (do NOT write it here with double braces — the build substitutes
  // tokens inside comments too), so .load() works (calling it from a bare flutter.js throws
  // "FlutterLoader.load requires _flutter.buildConfig to be set").
  // serviceWorkerSettings is intentionally omitted (deprecated in the loader):
  // caching is handled by content-hash + server cache headers, not a SW.
  logProgress('FlutterLoader ready, calling load to fetch main.js');
  try {
    _flutter.loader.load({
      // Use the canvaskit files shipped next to this app (canvaskit/ dir) instead
      // of the gstatic CDN. The CDN default is wrong for the embedded WebView:
      // the local HTTP server already serves canvaskit.wasm (7MB) from disk, and
      // offline / weak-network installs have no access to gstatic at all.
      config: {
        canvasKitBaseUrl: 'canvaskit/'
      },
      onEntrypointLoaded: function (engineInitializer) {
        entrypointReady = true;
        logProgress('Entrypoint loaded (onEntrypointLoaded), initializing engine');
        if (timeoutId) { clearTimeout(timeoutId); timeoutId = null; }
        // Do not close the loading overlay here: onEntrypointLoaded only means the script has been downloaded,
        // the UI has not rendered yet. The overlay is closed on flutter-first-frame (first rendered frame).
        // Second-stage watchdog: if the first frame hasn't rendered within FLUTTER_FIRST_FRAME_TIMEOUT_MS,
        // surface an error instead of spinning forever. (Cleared by the flutter-first-frame listener and showFatalError.)
        timeoutId = setTimeout(function () {
          if (loadingEl) {
            showFatalError('First frame did not render within ' +
                           (FLUTTER_FIRST_FRAME_TIMEOUT_MS / 1000) +
                           's after the entrypoint loaded.');
          }
        }, FLUTTER_FIRST_FRAME_TIMEOUT_MS);
        engineInitializer.initializeEngine().then(function (appRunner) {
          logProgress('initializeEngine done, calling runApp');
          return appRunner.runApp();
        }).catch(function (err) {
          showFatalError('Engine init failed: ' + (err && err.message ? err.message : err));
        });
      }
    });
  } catch (err) {
    showFatalError('Loader error: ' + (err && err.message ? err.message : err));
  }
})();
