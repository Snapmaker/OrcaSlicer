// EdgeSlicer: FlashForge Device tab camera player. See index.html for the message contract.
(function () {
  'use strict';

  var img = document.getElementById('mjpeg');
  var video = document.getElementById('video');
  var overlay = document.getElementById('overlay');
  var playBtn = document.getElementById('play');
  var spinner = document.getElementById('spinner');
  var message = document.getElementById('message');
  var fsBtn = document.getElementById('fullscreen');

  // A 1x1 transparent GIF: assigning it is what reliably drops an MJPEG connection; removing the
  // src attribute alone can leave the multipart request running.
  var BLANK = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7';

  var address = '';
  var playing = false;
  var generation = 0;     // bumps on every start/stop so late events from an old stream are ignored
  var flvPlayer = null;
  var hlsPlayer = null;
  var startTimer = null;
  var stallTimer = null;

  function send(command) {
    var text = JSON.stringify({ sequence_id: String(Date.now()), command: command });
    try {
      if (window.wx && window.wx.postMessage) {
        window.wx.postMessage(text);
      } else if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.wx) {
        window.webkit.messageHandlers.wx.postMessage(text);
      }
    } catch (e) { /* not hosted by the slicer (opened in a browser for testing) */ }
  }

  function kindOf(url) {
    if (/\.m3u8(\?|$|\/|#)/i.test(url)) return 'hls';
    if (/\.flv(\?|$|\/|#)/i.test(url)) return 'flv';
    return 'mjpeg'; // LAN printers: http://<ip>:8080/?action=stream
  }

  function showOverlay(text, opts) {
    opts = opts || {};
    overlay.style.display = 'flex';
    overlay.style.background = opts.dim ? 'rgba(0,0,0,0.55)' : 'transparent';
    spinner.style.display = opts.busy ? 'block' : 'none';
    playBtn.style.display = opts.play ? 'block' : 'none';
    message.textContent = text || '';
    message.style.display = text ? 'block' : 'none';
  }

  function hideOverlay() {
    overlay.style.display = 'none';
  }

  function clearTimers() {
    if (startTimer) { clearTimeout(startTimer); startTimer = null; }
    if (stallTimer) { clearTimeout(stallTimer); stallTimer = null; }
  }

  function stop() {
    generation++;
    clearTimers();
    playing = false;
    if (flvPlayer) {
      try { flvPlayer.pause(); } catch (e) {}
      try { flvPlayer.unload(); } catch (e) {}
      try { flvPlayer.detachMediaElement(); } catch (e) {}
      try { flvPlayer.destroy(); } catch (e) {}
      flvPlayer = null;
    }
    if (hlsPlayer) {
      try { hlsPlayer.destroy(); } catch (e) {}
      hlsPlayer = null;
    }
    try { video.pause(); } catch (e) {}
    video.removeAttribute('src');
    try { video.load(); } catch (e) {}
    video.style.display = 'none';
    img.onload = null;
    img.onerror = null;
    img.src = BLANK;
    img.style.display = 'none';
    fsBtn.style.display = 'none';
  }

  function showIdle() {
    if (address) {
      showOverlay('Click to start the camera', { play: true });
    } else {
      showOverlay('Camera offline');
    }
  }

  function failed(gen, text) {
    if (gen !== generation) return;
    stop();
    showOverlay(text + ' Click to try again.', { play: true });
  }

  function started(gen) {
    if (gen !== generation) return;
    if (stallTimer) { clearTimeout(stallTimer); stallTimer = null; }
    hideOverlay();
    fsBtn.style.display = 'block';
  }

  function startMjpeg(gen, url) {
    img.onload = function () { started(gen); };
    img.onerror = function () {
      failed(gen, 'The camera stream at ' + url + ' could not be opened.');
    };
    img.style.display = 'block';
    img.src = url;
  }

  function startVideo(gen, url, kind) {
    video.style.display = 'block';
    video.onplaying = function () { started(gen); };
    video.onerror = function () { failed(gen, 'The camera stream could not be played.'); };
    if (kind === 'flv') {
      if (!(window.flvjs && window.flvjs.isSupported())) {
        failed(gen, 'This viewer cannot play FLV streams.');
        return;
      }
      flvPlayer = window.flvjs.createPlayer({ type: 'flv', url: url, isLive: true },
                                            { enableStashBuffer: false });
      flvPlayer.on(window.flvjs.Events.ERROR, function () {
        failed(gen, 'The camera stream could not be played.');
      });
      flvPlayer.attachMediaElement(video);
      flvPlayer.load();
      var p = flvPlayer.play();
      if (p && p.catch) p.catch(function () {});
      return;
    }
    // HLS: the browser's own player where there is one (macOS WebKit), else hls.js.
    if (video.canPlayType('application/vnd.apple.mpegurl')) {
      video.src = url;
    } else if (window.Hls && window.Hls.isSupported()) {
      hlsPlayer = new window.Hls({ liveDurationInfinity: true });
      hlsPlayer.on(window.Hls.Events.ERROR, function (evt, data) {
        if (data && data.fatal) failed(gen, 'The camera stream could not be played.');
      });
      hlsPlayer.loadSource(url);
      hlsPlayer.attachMedia(video);
    } else {
      failed(gen, 'This viewer cannot play HLS streams.');
      return;
    }
    var q = video.play();
    if (q && q.catch) q.catch(function () {});
  }

  function start() {
    if (!address) { showIdle(); return; }
    stop();
    playing = true;
    var gen = generation;
    var url = address;
    var kind = kindOf(url);
    // A cloud printer opens its stream on request; a LAN printer streams already and the panel
    // ignores the request for it.
    send('rtsp_player_continue');
    showOverlay('Connecting to the camera...', { busy: true });
    stallTimer = setTimeout(function () {
      if (gen === generation)
        showOverlay('No picture from ' + url + ' yet - still trying...', { busy: true, dim: true });
    }, 12000);
    startTimer = setTimeout(function () {
      startTimer = null;
      if (gen !== generation) return;
      if (kind === 'mjpeg') startMjpeg(gen, url);
      else startVideo(gen, url, kind);
    }, kind === 'mjpeg' ? 0 : 1000);
  }

  function handle(msg) {
    if (typeof msg === 'string') {
      try { msg = JSON.parse(msg); } catch (e) { return; }
    }
    if (!msg || typeof msg !== 'object') return;
    var command = msg.command;
    if (command === 'modify_rtsp_player_address') {
      var next = typeof msg.address === 'string' ? msg.address : '';
      if (next === address) return;
      var wasPlaying = playing;
      stop();
      address = next;
      if (wasPlaying && address) start();
      else showIdle();
    } else if (command === 'close_rtsp') {
      stop();
      address = '';
      showIdle();
    }
  }

  playBtn.addEventListener('click', function (e) { e.stopPropagation(); start(); });
  // Clicking the picture pauses it, as FlashForge's player does; the play button resumes.
  img.addEventListener('click', function () { if (playing) { stop(); showIdle(); } });
  video.addEventListener('click', function () { if (playing) { stop(); showIdle(); } });
  fsBtn.addEventListener('click', function (e) { e.stopPropagation(); send('full_screen'); });

  // PrinterCameraPanel calls window.postMessage({...}) through RunScript, as FlashForge's page
  // expects; take the call over rather than relying on cross-window message events.
  window.postMessage = handle;
  window.addEventListener('message', function (e) { if (e && e.data) handle(e.data); });

  showOverlay("Waiting for the printer's camera...");
})();
