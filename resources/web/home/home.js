// The Home tab (hosted by src/slic3r/GUI/HomePanel.cpp).
//
// The page only draws. It asks the slicer for everything with window.wx.postMessage(JSON) and the
// slicer answers through window.HomeApp.receive({type, ...}):
//   init    {strings, section}             translated strings, the section to open
//   recent  {items: [{path, name, folder, exists, time, image}]}
//   history {enabled, items: [card]}       cards as HomeTabLogic.cpp history_card() builds them
//   thumbs  {images: {id: dataUri}}        Print History thumbnails, asked for as cards scroll in
// Text from files and printers is only ever set with textContent, never parsed as HTML.
(function () {
  'use strict';

  const state = {
    strings: {},
    section: 'recent',
    recent: null,
    history: null,
    historyEnabled: true,
    thumbs: {},
    asked: new Set(),
    printer: '',
    search: '',
    sort: { recent: 'newest', history: 'newest' },
  };

  const $ = (id) => document.getElementById(id);
  const t = (key, fallback) => state.strings[key] || fallback || key;

  function post(command, extra) {
    const msg = Object.assign({ command: command }, extra || {});
    if (window.wx && typeof window.wx.postMessage === 'function')
      window.wx.postMessage(JSON.stringify(msg));
  }

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }

  function svg(path) {
    const ns = 'http://www.w3.org/2000/svg';
    const s = document.createElementNS(ns, 'svg');
    s.setAttribute('viewBox', '0 0 24 24');
    s.setAttribute('aria-hidden', 'true');
    const p = document.createElementNS(ns, 'path');
    p.setAttribute('d', path);
    s.appendChild(p);
    return s;
  }
  const ICON_CUBE = 'M12 2 3 7v10l9 5 9-5V7zm0 2.3 6.7 3.7L12 11.7 5.3 8zM5 9.7l6 3.3v6.7l-6-3.3zm8 10v-6.7l6-3.3v6.7z';
  const ICON_MORE = 'M12 8a2 2 0 1 0 0-4 2 2 0 0 0 0 4zm0 2a2 2 0 1 0 0 4 2 2 0 0 0 0-4zm0 6a2 2 0 1 0 0 4 2 2 0 0 0 0-4z';

  // ---- formatting ----
  function fmtDate(seconds) {
    if (!seconds) return '';
    const d = new Date(seconds * 1000);
    const now = new Date();
    const time = d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
    if (d.toDateString() === now.toDateString()) return time;
    const opts = { month: 'short', day: 'numeric' };
    if (d.getFullYear() !== now.getFullYear()) opts.year = 'numeric';
    return d.toLocaleDateString([], opts) + ' ' + time;
  }
  function fmtDuration(s) {
    if (!s || s <= 0) return '';
    const h = Math.floor(s / 3600), m = Math.round((s % 3600) / 60);
    return h > 0 ? h + 'h ' + m + 'm' : m + 'm';
  }
  function fmtWeight(g) {
    if (!g || g <= 0) return '';
    return g >= 1000 ? (g / 1000).toFixed(2) + ' kg' : (g < 10 ? g.toFixed(1) : Math.round(g)) + ' g';
  }

  // ---- menu ----
  let menuFor = null;
  function closeMenu() {
    $('menu').hidden = true;
    menuFor = null;
  }
  function openMenu(anchor, entries) {
    const menu = $('menu');
    menu.textContent = '';
    for (const e of entries) {
      if (e === '-') { menu.appendChild(el('hr')); continue; }
      const b = el('button', e.danger ? 'danger' : '', e.label);
      b.type = 'button';
      b.setAttribute('role', 'menuitem');
      b.addEventListener('click', (ev) => { ev.stopPropagation(); closeMenu(); e.run(); });
      menu.appendChild(b);
    }
    menu.hidden = false;
    const r = anchor.getBoundingClientRect();
    const w = menu.offsetWidth, h = menu.offsetHeight;
    menu.style.left = Math.max(4, Math.min(r.right - w, window.innerWidth - w - 4)) + 'px';
    menu.style.top = (r.bottom + h + 4 > window.innerHeight ? Math.max(4, r.top - h - 4) : r.bottom + 4) + 'px';
    menuFor = anchor;
    const first = menu.querySelector('button');
    if (first) first.focus();
  }
  document.addEventListener('click', (e) => { if (menuFor && !$('menu').contains(e.target)) closeMenu(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeMenu(); });
  window.addEventListener('resize', closeMenu);
  document.addEventListener('scroll', closeMenu, true);

  function card(opts) {
    const c = el('div', 'card' + (opts.missing ? ' missing' : ''));
    c.tabIndex = 0;
    c.setAttribute('role', 'button');
    const thumb = el('div', 'thumb');
    if (opts.image) {
      const img = el('img');
      img.alt = '';
      img.src = opts.image;
      thumb.appendChild(img);
    } else {
      thumb.appendChild(svg(ICON_CUBE));
    }
    c.appendChild(thumb);
    if (opts.badges && opts.badges.length) {
      const b = el('div', 'badges');
      for (const x of opts.badges) b.appendChild(el('span', 'badge' + (x.cls ? ' ' + x.cls : ''), x.text));
      c.appendChild(b);
    }
    const info = el('div', 'info');
    const name = el('div', 'name', opts.name);
    name.title = opts.name;
    info.appendChild(name);
    for (const line of opts.lines || []) {
      if (!line) continue;
      const s = el('div', 'sub', line);
      s.title = line;
      info.appendChild(s);
    }
    if (opts.meta) info.appendChild(opts.meta);
    c.appendChild(info);
    if (opts.menu && opts.menu.length) {
      const more = el('button', 'more');
      more.type = 'button';
      more.setAttribute('aria-label', 'More');
      more.appendChild(svg(ICON_MORE));
      more.addEventListener('click', (e) => {
        e.stopPropagation();
        if (menuFor === more) closeMenu(); else openMenu(more, opts.menu);
      });
      c.appendChild(more);
    }
    const activate = () => { if (opts.onOpen) opts.onOpen(); };
    c.addEventListener('click', activate);
    c.addEventListener('keydown', (e) => { if ((e.key === 'Enter' || e.key === ' ') && e.target === c) { e.preventDefault(); activate(); } });
    c.addEventListener('contextmenu', (e) => {
      if (!opts.menu || !opts.menu.length) return;
      e.preventDefault();
      openMenu(c.querySelector('.more') || c, opts.menu);
    });
    return c;
  }

  function matches(fields, query) {
    if (!query) return true;
    const q = query.toLowerCase();
    return fields.some((f) => f && String(f).toLowerCase().indexOf(q) >= 0);
  }

  function sortBy(items, mode, nameOf, timeOf) {
    const out = items.slice();
    if (mode === 'name') out.sort((a, b) => nameOf(a).localeCompare(nameOf(b), undefined, { numeric: true, sensitivity: 'base' }));
    else if (mode === 'oldest') out.sort((a, b) => timeOf(a) - timeOf(b));
    else out.sort((a, b) => timeOf(b) - timeOf(a));
    return out;
  }

  // ---- Recent ----
  function renderRecent() {
    const grid = $('recent-grid'), empty = $('recent-empty');
    grid.textContent = '';
    if (state.recent === null) { empty.textContent = t('loading', 'Loading...'); empty.hidden = false; return; }
    const shown = sortBy(state.recent.filter((r) => matches([r.name, r.folder], state.search)),
      state.sort.recent, (r) => r.name || '', (r) => r.time || 0);
    for (const r of shown) {
      grid.appendChild(card({
        name: r.name,
        image: r.exists ? r.image : '',
        missing: !r.exists,
        badges: r.exists ? [] : [{ text: t('missing', 'File is missing'), cls: 'warn' }],
        lines: [r.folder, r.exists ? fmtDate(r.time) : ''],
        onOpen: () => post('recent_open', { path: r.path }),
        menu: [
          { label: t('open', 'Open'), run: () => post('recent_open', { path: r.path }) },
          { label: t('show_in_folder', 'Show in folder'), run: () => post('recent_reveal', { path: r.path }) },
          '-',
          { label: t('remove_from_list', 'Remove from list'), run: () => post('recent_forget', { path: r.path }) },
        ],
      }));
    }
    empty.hidden = shown.length > 0;
    empty.textContent = state.recent.length ? t('no_match', 'Nothing matches your search.') : t('recent_empty', 'Projects you open or save will show up here.');
  }

  // ---- Print History ----
  let thumbObserver = null;
  let askTimer = 0;
  const askQueue = [];
  function askThumb(id) {
    if (state.thumbs[id] || state.asked.has(id)) return;
    state.asked.add(id);
    askQueue.push(id);
    if (!askTimer) askTimer = setTimeout(() => {
      askTimer = 0;
      while (askQueue.length) post('history_thumbs', { ids: askQueue.splice(0, 40) });
    }, 30);
  }

  function historyMatches(h) {
    return (!state.printer || h.printer === state.printer) &&
      matches([h.title, h.plate_name, h.printer, h.model, h.file], state.search);
  }

  function renderPrinterChips() {
    const box = $('printer-chips');
    box.textContent = '';
    if (!state.history || !state.history.length) return;
    const counts = new Map();
    for (const h of state.history) if (h.printer) counts.set(h.printer, (counts.get(h.printer) || 0) + 1);
    if (counts.size < 2) { state.printer = ''; return; }
    if (state.printer && !counts.has(state.printer)) state.printer = '';
    const chip = (label, value, count) => {
      const c = el('button', 'chip' + (state.printer === value ? ' active' : ''), label);
      c.type = 'button';
      if (count !== undefined) c.appendChild(el('span', 'count', count));
      c.addEventListener('click', () => { state.printer = value; renderHistory(); });
      box.appendChild(c);
    };
    chip(t('all_printers', 'All printers'), '', state.history.length);
    for (const [name, n] of [...counts.entries()].sort((a, b) => b[1] - a[1])) chip(name, name, n);
  }

  function historyMeta(h) {
    const meta = el('div', 'meta');
    if (h.print_time_s) meta.appendChild(el('span', '', fmtDuration(h.print_time_s)));
    if (h.weight_g) meta.appendChild(el('span', '', fmtWeight(h.weight_g)));
    const colours = (h.filaments || []).filter((f) => f.colour);
    if (colours.length) {
      const sw = el('span', 'swatches');
      for (const f of colours.slice(0, 8)) {
        const s = el('span', 'swatch');
        s.style.background = f.colour;
        s.title = [f.type, fmtWeight(f.grams)].filter(Boolean).join(' ');
        sw.appendChild(s);
      }
      meta.appendChild(sw);
    }
    return meta;
  }

  function renderHistory() {
    renderPrinterChips();
    const grid = $('history-grid'), empty = $('history-empty');
    $('history-off').hidden = state.historyEnabled;
    grid.textContent = '';
    if (thumbObserver) thumbObserver.disconnect();
    if (state.history === null) { empty.textContent = t('loading', 'Loading...'); empty.hidden = false; return; }
    const shown = sortBy(state.history.filter(historyMatches), state.sort.history,
      (h) => h.title || '', (h) => h.time || 0);
    if ('IntersectionObserver' in window) {
      thumbObserver = new IntersectionObserver((entries) => {
        for (const e of entries) if (e.isIntersecting) { askThumb(e.target.dataset.id); thumbObserver.unobserve(e.target); }
      }, { root: $('history'), rootMargin: '400px' });
    }
    for (const h of shown) {
      const plate = h.plate ? t('plate', 'Plate') + ' ' + h.plate + (h.plate_name ? ' · ' + h.plate_name : '') : h.plate_name;
      const badges = [];
      badges.push(h.mode === 'print' ? { text: t('printed', 'Printed'), cls: 'accent' } : { text: t('uploaded', 'Uploaded') });
      if (h.source === 'phone') badges.push({ text: t('from_phone', 'from phone') });
      if (h.reprints > 0) badges.push({ text: t('reprinted', 'Reprinted') + ' ' + h.reprints + '×' });
      if (!h.file_present) badges.push({ text: t('file_gone', 'The archived file is gone'), cls: 'warn' });
      const menu = [];
      if (h.can_open) menu.push({ label: t('open_preview', 'Open in preview'), run: () => post('history_open', { id: h.id }) });
      if (h.project_present) menu.push({ label: t('open_source', 'Open source project'), run: () => post('history_project', { id: h.id }) });
      menu.push({ label: t('show_in_folder', 'Show in folder'), run: () => post('history_reveal', { id: h.id }) });
      menu.push('-');
      menu.push({ label: t('delete', 'Delete'), danger: true, run: () => post('history_delete', { id: h.id }) });
      const c = card({
        name: h.title || h.file,
        image: state.thumbs[h.id] || '',
        missing: !h.can_open && !h.project_present,
        badges: badges,
        lines: [plate, [h.printer, fmtDate(h.time)].filter(Boolean).join(' · ')],
        meta: historyMeta(h),
        onOpen: () => {
          if (h.can_open) post('history_open', { id: h.id });
          else if (h.project_present) post('history_project', { id: h.id });
        },
        menu: menu,
      });
      c.dataset.id = h.id;
      if (h.has_thumbnail && !state.thumbs[h.id]) {
        if (thumbObserver) thumbObserver.observe(c); else askThumb(h.id);
      }
      grid.appendChild(c);
    }
    empty.hidden = shown.length > 0;
    empty.textContent = state.history.length ? t('no_match', 'Nothing matches your search.') : t('history_empty', 'Files you send to a printer will show up here.');
  }

  function applyThumbs(images) {
    for (const id of Object.keys(images)) {
      if (typeof images[id] !== 'string' || images[id].indexOf('data:image/png;base64,') !== 0) continue;
      state.thumbs[id] = images[id];
      const c = document.querySelector('#history-grid .card[data-id="' + CSS.escape(id) + '"] .thumb');
      if (!c) continue;
      c.textContent = '';
      const img = el('img');
      img.alt = '';
      img.src = images[id];
      c.appendChild(img);
    }
  }

  // ---- sections ----
  function render() {
    if (state.section === 'history') renderHistory(); else renderRecent();
  }

  function showSection(section, persist) {
    if (section !== 'recent' && section !== 'history') section = 'recent';
    state.section = section;
    for (const b of document.querySelectorAll('.rail-item')) {
      const on = b.dataset.section === section;
      b.classList.toggle('active', on);
      b.setAttribute('aria-current', on ? 'page' : 'false');
    }
    $('recent').hidden = section !== 'recent';
    $('history').hidden = section !== 'history';
    $('title').textContent = t(section, section === 'history' ? 'Print History' : 'Recent');
    $('sort').value = state.sort[section];
    closeMenu();
    render();
    if (persist) post('home_section', { section: section });
  }

  function applyStrings() {
    for (const e of document.querySelectorAll('[data-i18n]')) {
      const s = state.strings[e.dataset.i18n];
      if (s) e.textContent = s;
    }
    for (const b of document.querySelectorAll('.rail-item')) b.title = t(b.dataset.section);
    $('search').placeholder = t('search', 'Search');
    $('refresh').title = t('refresh', 'Refresh');
    $('refresh').setAttribute('aria-label', t('refresh', 'Refresh'));
    $('history-off-text').textContent = t('history_off', 'The G-code archive is off.');
  }

  function stopSpin() { $('refresh').classList.remove('spin'); }

  window.HomeApp = {
    receive: function (msg) {
      if (!msg || typeof msg !== 'object') return;
      switch (msg.type) {
        case 'init':
          state.strings = msg.strings || {};
          applyStrings();
          showSection(msg.section || state.section, false);
          break;
        case 'recent':
          state.recent = Array.isArray(msg.items) ? msg.items : [];
          if (state.section === 'recent') { renderRecent(); stopSpin(); }
          break;
        case 'history':
          state.history = Array.isArray(msg.items) ? msg.items : [];
          state.historyEnabled = msg.enabled !== false;
          // A thumbnail that failed to arrive may be asked for again on the next listing.
          state.asked = new Set(Object.keys(state.thumbs));
          if (state.section === 'history') { renderHistory(); stopSpin(); }
          break;
        case 'thumbs':
          applyThumbs(msg.images || {});
          break;
      }
    },
  };

  // ---- wiring ----
  if (/dark/i.test(navigator.userAgent)) document.documentElement.classList.add('dark');
  for (const b of document.querySelectorAll('.rail-item'))
    b.addEventListener('click', () => showSection(b.dataset.section, true));
  let searchTimer = 0;
  $('search').addEventListener('input', () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => { state.search = $('search').value.trim(); render(); }, 120);
  });
  $('sort').addEventListener('change', () => { state.sort[state.section] = $('sort').value; render(); });
  $('refresh').addEventListener('click', () => { $('refresh').classList.add('spin'); post('home_refresh'); setTimeout(stopSpin, 3000); });
  $('new-project').addEventListener('click', () => post('project_new'));
  $('open-project').addEventListener('click', () => post('project_open'));
  $('history-settings').addEventListener('click', () => post('history_settings'));

  applyStrings();
  showSection('recent', false);
  post('home_ready');
})();
