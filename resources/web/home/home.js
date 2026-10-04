// The Home tab (hosted by src/slic3r/GUI/HomePanel.cpp).
//
// The page only draws. It asks the slicer for everything with window.wx.postMessage(JSON) and the
// slicer answers through window.HomeApp.receive({type, ...}):
//   init    {strings, section, models}     translated strings, the section to open, whether Models is offered
//   models  {enabled}                      Preferences > "Model browser (beta)" changed
//   recent  {items: [{path, name, folder, exists, time, image}]}
//   history {enabled, items: [card]}       cards as HomeTabLogic.cpp history_card() builds them
//   thumbs  {images: {id: dataUri}}        Print History thumbnails, asked for as cards scroll in
//   library {folders: [{path, recursive, category, vendor, scanned, online, files}],
//            items: [item], hidden, scanning, files_seen, scanned_at}
//                                          items as LibraryIndex.cpp page_item() builds them (no paths
//                                          of files: the page acts on ids)
//   library_progress {scanning, files}     a scan is running
//   library_thumbs {images: {id: dataUri}} Library covers, asked for as cards scroll in
//   library_plates {id, plates: [{index, name, image}]}  a 3MF's plates, for its detail view
//   vendors {connectors: [...], items: [...]}  HomeVendors.cpp send_state(): connector specs (never
//                                          a secret), and items keyed by opaque keys
//   vendor_thumbs {images: {key: dataUri}} vendor thumbnails (PNG, JPEG, GIF or WebP)
//   vendor_notice {text, error}            a result to show for a moment
//   vendor_saved {id} / vendor_invalid {error}  the connector editor's save went through or not
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
    sort: { recent: 'newest', library: 'newest', history: 'newest', vendors: 'newest' },
    lib: null,          // the last 'library' message
    libThumbs: {},
    libAsked: new Set(),
    libFilter: { category: new Set(), vendor: new Set(), license: new Set(), designer: new Set(), type: new Set(), folder: new Set(), added: new Set() },
    libExpanded: new Set(), // filter groups showing all their values
    libMoreFilters: false,  // show the groups past the first three
    libShown: [],
    libRendered: 0,
    libSeen: 0,
    ven: null,          // the last 'vendors' message
    venThumbs: {},
    venAsked: new Set(),
    venFilter: { connector: new Set(), tag: new Set(), license: new Set() },
    venShown: [],
    venRendered: 0,
  };
  const SECTIONS = ['recent', 'library', 'history', 'vendors'];
  const SORTS = {
    recent: ['newest', 'oldest', 'name'],
    library: ['newest', 'oldest', 'added', 'name', 'size'],
    history: ['newest', 'oldest', 'name'],
    vendors: ['newest', 'oldest', 'name'],
  };
  const SORT_LABELS = { newest: ['sort_newest', 'Newest first'], oldest: ['sort_oldest', 'Oldest first'],
    added: ['sort_added', 'Recently added'], name: ['sort_name', 'Name'], size: ['sort_size', 'Largest first'] };
  const LIB_PAGE = 120; // cards added at a time as the grid scrolls

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
  function fmtSize(b) {
    if (!b || b <= 0) return '';
    if (b < 1024 * 1024) return Math.max(1, Math.round(b / 1024)) + ' KB';
    if (b < 1024 * 1024 * 1024) return (b / 1024 / 1024).toFixed(b < 10 * 1024 * 1024 ? 1 : 0) + ' MB';
    return (b / 1024 / 1024 / 1024).toFixed(1) + ' GB';
  }
  function leaf(path) {
    const parts = String(path || '').split(/[\\/]/).filter(Boolean);
    return parts.length ? parts[parts.length - 1] : String(path || '');
  }
  function stem(name) {
    return String(name || '').replace(/(\.gcode)?\.(3mf|stl|step|stp|obj|amf)$/i, '');
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

  function sortBy(items, mode, nameOf, timeOf, extra) {
    const out = items.slice();
    if (extra && extra[mode]) out.sort(extra[mode]);
    else if (mode === 'name') out.sort((a, b) => nameOf(a).localeCompare(nameOf(b), undefined, { numeric: true, sensitivity: 'base' }));
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
  // Thumbnails are asked for in batches as their cards come near the screen.
  function thumbAsker(command, have, asked, field) {
    let timer = 0;
    const queue = [];
    return function (id) {
      if (have()[id] || asked().has(id)) return;
      asked().add(id);
      queue.push(id);
      if (!timer) timer = setTimeout(() => {
        timer = 0;
        while (queue.length) post(command, { [field || 'ids']: queue.splice(0, 40) });
      }, 30);
    };
  }
  let thumbObserver = null;
  const askThumb = thumbAsker('history_thumbs', () => state.thumbs, () => state.asked);

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

  function applyThumbs(images, store, grid) {
    for (const id of Object.keys(images)) {
      if (typeof images[id] !== 'string' || images[id].indexOf('data:image/png;base64,') !== 0) continue;
      store[id] = images[id];
      const c = document.querySelector('#' + grid + ' .card[data-id="' + CSS.escape(id) + '"] .thumb');
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
    if (state.section === 'history') renderHistory();
    else if (state.section === 'library') renderLibrary();
    else if (state.section === 'vendors') renderVendors();
    else renderRecent();
  }

  function fillSort(section) {
    const sel = $('sort');
    sel.textContent = '';
    for (const mode of SORTS[section]) {
      const o = el('option', '', t(SORT_LABELS[mode][0], SORT_LABELS[mode][1]));
      o.value = mode;
      sel.appendChild(o);
    }
    sel.value = state.sort[section];
  }

  function showSection(section, persist) {
    if (SECTIONS.indexOf(section) < 0) section = 'recent';
    state.section = section;
    for (const b of document.querySelectorAll('.rail-item')) {
      const on = b.dataset.section === section;
      b.classList.toggle('active', on);
      b.setAttribute('aria-current', on ? 'page' : 'false');
    }
    for (const id of SECTIONS) $(id).hidden = section !== id;
    $('title').textContent = t(section, { recent: 'Recent', library: 'Library', history: 'Print History', vendors: 'Vendors' }[section]);
    fillSort(section);
    $('refresh').classList.toggle('spin', section === 'library' && !!state.lib && state.lib.scanning);
    closeMenu();
    render();
    if (persist) post('home_section', { section: section });
  }

  // The slicer's UI theme (docs/themes.md): CSS variables over the light or dark ones. Only the
  // page's own variables, only #RRGGBB values.
  function applyTheme(theme) {
    if (!theme || typeof theme !== 'object') return;
    const root = document.documentElement;
    for (const [name, value] of Object.entries(theme)) {
      if (/^--[a-z-]+$/.test(name) && typeof value === 'string' && /^#[0-9a-fA-F]{6}$/.test(value))
        root.style.setProperty(name, value);
    }
  }

  function applyStrings() {
    for (const e of document.querySelectorAll('[data-i18n]')) {
      const s = state.strings[e.dataset.i18n];
      if (s) e.textContent = s;
    }
    for (const b of document.querySelectorAll('.rail-item')) b.title = t(b.dataset.section || b.dataset.action);
    $('search').placeholder = t('search', 'Search');
    $('refresh').title = t('refresh', 'Refresh');
    $('refresh').setAttribute('aria-label', t('refresh', 'Refresh'));
    $('history-off-text').textContent = t('history_off', 'The G-code archive is off.');
  }

  function stopSpin() { $('refresh').classList.remove('spin'); }

  // ---- Library ----
  const TYPE_LABELS = { '3mf': '3MF', stl: 'STL', step: 'STEP', obj: 'OBJ', amf: 'AMF' };
  let libThumbObserver = null;
  let libMoreObserver = null;
  const askLibThumb = thumbAsker('library_thumbs', () => state.libThumbs, () => state.libAsked);

  function receiveLibrary(msg) {
    const lib = {
      folders: Array.isArray(msg.folders) ? msg.folders : [],
      items: Array.isArray(msg.items) ? msg.items : [],
      hidden: Number(msg.hidden) || 0,
      scanning: !!msg.scanning,
      scanned_at: Number(msg.scanned_at) || 0,
    };
    state.libSeen = Number(msg.files_seen) || 0;
    // A cover that failed to arrive, or changed with its file, is asked for again.
    const keep = {};
    const prev = state.lib ? new Map(state.lib.items.map((i) => [i.id, i])) : new Map();
    for (const i of lib.items) {
      const p = prev.get(i.id);
      if (state.libThumbs[i.id] && p && p.mtime === i.mtime && p.size === i.size) keep[i.id] = state.libThumbs[i.id];
    }
    state.libThumbs = keep;
    state.libAsked = new Set(Object.keys(keep));
    const keepScroll = state.lib !== null;
    state.lib = lib;
    if (state.section === 'library') {
      renderLibrary(keepScroll);
      if (!lib.scanning) stopSpin();
    }
    if (!$('folders').hidden) renderFolders();
  }

  function folderLabel(i) {
    return leaf(i.root) + (i.rel_dir ? '/' + i.rel_dir : '');
  }

  // When a file was added to the Library, in buckets for the Added filter.
  const ADDED_BUCKETS = [['week', 7], ['month', 30], ['year', 365]];
  function addedBucket(i) {
    const days = (Date.now() / 1000 - (i.added || 0)) / 86400;
    for (const [key, n] of ADDED_BUCKETS) if (days < n) return key;
    return 'older';
  }
  // The Library's filter groups, in the order they show: `value` gives a file's value in the group.
  function libGroups() {
    const none = (v) => v || t('none', 'None');
    const ADDED = { week: t('added_week', 'Last 7 days'), month: t('added_month', 'Last 30 days'),
      year: t('added_year', 'Last 12 months'), older: t('added_older', 'Older') };
    return [
      { key: 'category', label: t('category', 'Category'), value: (i) => i.category || '', name: none },
      { key: 'vendor', label: t('vendor', 'Vendor'), value: (i) => i.vendor || '', name: none },
      { key: 'license', label: t('license', 'License'), value: (i) => i.license || '', name: (v) => v || t('license_not_set', 'Not set') },
      { key: 'designer', label: t('designer', 'Designer'), value: (i) => i.designer || '', name: (v) => v || t('unknown', 'Unknown') },
      { key: 'type', label: t('type', 'Type'), value: (i) => i.type, name: (v) => TYPE_LABELS[v] || v },
      { key: 'folder', label: t('folder', 'Folder'), value: (i) => i.root || '', name: (v) => leaf(v) || v, title: (v) => v },
      { key: 'added', label: t('added', 'Added'), value: addedBucket, name: (v) => ADDED[v] || v, order: ['week', 'month', 'year', 'older'] },
    ];
  }
  let LIB_GROUPS = null;

  function libMatchesExcept(i, skip) {
    const f = state.libFilter;
    if (!LIB_GROUPS) LIB_GROUPS = libGroups();
    for (const g of LIB_GROUPS) if (skip !== g.key && f[g.key].size && !f[g.key].has(g.value(i))) return false;
    return matches([i.name, i.title, i.designer, i.category, i.vendor, i.license, folderLabel(i)].concat(i.plate_names || []), state.search);
  }

  function renderLibraryStatus() {
    const box = $('lib-status');
    box.textContent = '';
    const lib = state.lib;
    if (!lib) return;
    const parts = [];
    if (lib.scanning) parts.push(t('scanning', 'Scanning...') + (state.libSeen ? ' ' + state.libSeen + ' ' + t('files', 'files') : ''));
    else if (lib.scanned_at) parts.push(lib.items.length + ' ' + t('files', 'files') + ' · ' + t('scanned', 'Scanned') + ' ' + fmtDate(lib.scanned_at));
    box.appendChild(el('span', '', parts.join('')));
    if (lib.hidden > 0) {
      box.appendChild(el('span', '', ' · ' + lib.hidden + ' ' + t('hidden', 'hidden')));
      const b = el('button', 'link', t('show_hidden', 'Show them again'));
      b.type = 'button';
      b.addEventListener('click', () => post('library_unhide_all'));
      box.appendChild(b);
    }
    $('refresh').classList.toggle('spin', lib.scanning);

    const off = $('lib-offline');
    off.textContent = '';
    for (const f of lib.folders) if (f.scanned && !f.online) {
      const d = el('div', '', f.path + ': ' + t('offline', 'Not reachable, showing the files from the last scan'));
      d.title = f.path;
      off.appendChild(d);
    }
    off.hidden = !off.childNodes.length;
  }

  function renderLibraryFilters() {
    const box = $('lib-filters');
    box.textContent = '';
    const items = state.lib ? state.lib.items : [];
    LIB_GROUPS = libGroups();
    let any = false, shownRows = 0, folded = 0;
    for (const g of LIB_GROUPS) {
      const all = new Set(items.map(g.value));
      const sel = state.libFilter[g.key];
      for (const v of [...sel]) if (!all.has(v)) sel.delete(v);
      // A group is worth showing when it can tell files apart.
      if (all.size < 2) continue;
      // Past the first three, a group waits behind "More filters" unless something in it is picked.
      if (shownRows >= 3 && !state.libMoreFilters && !sel.size) { ++folded; continue; }
      ++shownRows;
      const counts = new Map();
      for (const i of items) if (libMatchesExcept(i, g.key)) counts.set(g.value(i), (counts.get(g.value(i)) || 0) + 1);
      const row = el('div', 'filter-row');
      row.appendChild(el('span', 'label', g.label));
      const values = g.order ? g.order.filter((v) => all.has(v))
        : [...all].sort((a, b) => (a === '') - (b === '') || String(g.name(a)).localeCompare(String(g.name(b)), undefined, { sensitivity: 'base' }));
      // A long group shows its first values (and any picked) until "more" is pressed.
      const MAX_CHIPS = 12;
      const open = state.libExpanded.has(g.key) || values.length <= MAX_CHIPS + 2;
      const shown = open ? values : values.filter((v, k) => k < MAX_CHIPS || sel.has(v));
      for (const v of shown) {
        const c = el('button', 'chip' + (sel.has(v) ? ' active' : ''), g.name(v));
        c.type = 'button';
        if (g.title) c.title = g.title(v);
        c.setAttribute('aria-pressed', sel.has(v) ? 'true' : 'false');
        c.appendChild(el('span', 'count', counts.get(v) || 0));
        c.addEventListener('click', () => {
          if (sel.has(v)) sel.delete(v); else sel.add(v);
          renderLibrary();
        });
        row.appendChild(c);
      }
      if (values.length > MAX_CHIPS + 2) {
        const m = el('button', 'chip clear', open ? t('fewer', 'Fewer') : '+' + (values.length - shown.length) + ' ' + t('more', 'more'));
        m.type = 'button';
        m.addEventListener('click', () => {
          if (open) state.libExpanded.delete(g.key); else state.libExpanded.add(g.key);
          renderLibraryFilters();
        });
        row.appendChild(m);
      }
      box.appendChild(row);
      any = true;
    }
    const active = Object.values(state.libFilter).some((f) => f.size);
    if (any && (active || folded || state.libMoreFilters)) {
      const row = el('div', 'filter-row');
      row.appendChild(el('span', 'label', ''));
      if (folded || state.libMoreFilters) {
        const m = el('button', 'chip clear', folded ? t('more_filters', 'More filters') : t('fewer_filters', 'Fewer filters'));
        m.type = 'button';
        m.addEventListener('click', () => { state.libMoreFilters = !!folded; renderLibraryFilters(); });
        row.appendChild(m);
      }
      if (active) {
        const c = el('button', 'chip clear', t('clear_filters', 'Clear filters'));
        c.type = 'button';
        c.addEventListener('click', () => { for (const f of Object.values(state.libFilter)) f.clear(); renderLibrary(); });
        row.appendChild(c);
      }
      box.appendChild(row);
    }
  }

  function libraryCard(i) {
    const badges = [{ text: TYPE_LABELS[i.type] || i.type }];
    if (i.sliced) badges.push({ text: t('sliced', 'Sliced'), cls: 'accent' });
    if (i.plates > 1) badges.push({ text: i.plates + ' ' + t('plates', 'plates') });
    const menu = [{ label: t('details', 'Details'), run: () => openLibDetail(i) }];
    for (const m of libActions(i)) menu.push(m);
    menu.push('-');
    menu.push({ label: t('hide', 'Hide from Library'), run: () => post('library_hide', { id: i.id }) });
    const meta = el('div', 'meta');
    for (const x of [fmtDate(i.mtime), fmtSize(i.size)]) if (x) meta.appendChild(el('span', '', x));
    const tags = [i.category, i.vendor, i.license].filter(Boolean).join(' · ');
    const c = card({
      name: i.title || stem(i.name),
      image: state.libThumbs[i.id] || '',
      badges: badges,
      lines: [i.designer ? t('by', 'by') + ' ' + i.designer : '', folderLabel(i), tags],
      meta: meta,
      onOpen: () => post('library_open', { id: i.id }),
      menu: menu,
    });
    c.dataset.id = i.id;
    c.title = i.name;
    if (i.has_thumbnail && !state.libThumbs[i.id]) {
      if (libThumbObserver) libThumbObserver.observe(c); else askLibThumb(i.id);
    }
    return c;
  }

  function renderMoreLibrary() {
    const grid = $('library-grid');
    const end = Math.min(state.libShown.length, state.libRendered + LIB_PAGE);
    const frag = document.createDocumentFragment();
    for (let k = state.libRendered; k < end; ++k) frag.appendChild(libraryCard(state.libShown[k]));
    grid.appendChild(frag);
    state.libRendered = end;
  }

  function renderLibrary(keepScroll) {
    const section = $('library');
    const scroll = keepScroll ? section.scrollTop : 0;
    renderLibraryStatus();
    renderLibraryFilters();
    const grid = $('library-grid'), empty = $('library-empty'), emptyText = $('library-empty-text');
    grid.textContent = '';
    if (libThumbObserver) libThumbObserver.disconnect();
    if (libMoreObserver) libMoreObserver.disconnect();
    state.libShown = [];
    state.libRendered = 0;
    $('lib-empty-add').hidden = true;
    if (state.lib === null) { emptyText.textContent = t('loading', 'Loading...'); empty.hidden = false; return; }
    const lib = state.lib;
    const byName = (a, b) => (a.title || stem(a.name)).localeCompare(b.title || stem(b.name), undefined, { numeric: true, sensitivity: 'base' });
    state.libShown = sortBy(lib.items.filter((i) => libMatchesExcept(i, '')), state.sort.library,
      (i) => i.title || stem(i.name), (i) => i.mtime || 0,
      { added: (a, b) => (b.added || 0) - (a.added || 0) || byName(a, b), size: (a, b) => (b.size || 0) - (a.size || 0) });
    if ('IntersectionObserver' in window) {
      libThumbObserver = new IntersectionObserver((entries) => {
        for (const e of entries) if (e.isIntersecting) { askLibThumb(e.target.dataset.id); libThumbObserver.unobserve(e.target); }
      }, { root: section, rootMargin: '400px' });
      libMoreObserver = new IntersectionObserver((entries) => {
        if (entries.some((e) => e.isIntersecting) && state.libRendered < state.libShown.length) renderMoreLibrary();
      }, { root: section, rootMargin: '800px' });
    }
    renderMoreLibrary();
    // Put the reader back where they were, rendering as far as that needs.
    while (keepScroll && state.libRendered < state.libShown.length && grid.scrollHeight < scroll + section.clientHeight) renderMoreLibrary();
    if (keepScroll) section.scrollTop = scroll;
    if (libMoreObserver) libMoreObserver.observe($('lib-more'));
    else while (state.libRendered < state.libShown.length) renderMoreLibrary();

    empty.hidden = state.libShown.length > 0;
    if (!lib.folders.length) {
      emptyText.textContent = t('library_no_folders', 'Add the folders where you keep your models to browse them here.');
      $('lib-empty-add').hidden = false;
    } else if (!lib.items.length) {
      emptyText.textContent = lib.scanning ? t('scanning', 'Scanning...') : t('library_empty', 'No model files in your Library folders yet.');
    } else {
      emptyText.textContent = t('no_match', 'Nothing matches your search.');
    }
  }

  function libActions(i) {
    const a = [];
    if (i.type === '3mf') a.push({ label: t('open', 'Open'), run: () => post('library_open', { id: i.id }) });
    a.push({ label: t('add_to_plate', 'Add to current project'), run: () => post('library_import', { id: i.id }) });
    a.push({ label: t('show_in_folder', 'Show in folder'), run: () => post('library_reveal', { id: i.id }) });
    return a;
  }

  // ---- Library detail, with the plate strip ----
  let libDetailId = '';
  let libDetailCover = ''; // the cover, shown again when the picked plate is picked a second time
  function setLibDetailThumb(uri) {
    const box = $('lib-detail-thumb');
    box.textContent = '';
    if (uri) { const img = el('img'); img.alt = ''; img.src = uri; box.appendChild(img); } else box.appendChild(svg(ICON_CUBE_BIG));
  }
  function openLibDetail(i) {
    closeMenu();
    libDetailId = i.id;
    libDetailCover = state.libThumbs[i.id] || '';
    setLibDetailThumb(libDetailCover);
    if (i.has_thumbnail && !libDetailCover) askLibThumb(i.id);
    $('lib-detail-name').textContent = i.title || stem(i.name);
    $('lib-detail-sub').textContent = [i.designer ? t('by', 'by') + ' ' + i.designer : '', TYPE_LABELS[i.type] || i.type,
      fmtSize(i.size), fmtDate(i.mtime), i.sliced ? t('sliced', 'Sliced') : ''].filter(Boolean).join(' · ');
    const where = $('lib-detail-where');
    where.textContent = [i.name, folderLabel(i), i.category, i.vendor, i.license ? t('license', 'License') + ': ' + i.license : ''].filter(Boolean).join(' · ');
    where.title = i.root + (i.rel_dir ? '/' + i.rel_dir : '');
    const actions = $('lib-detail-actions');
    actions.textContent = '';
    libActions(i).forEach((m, k) => {
      const b = el('button', 'btn' + (k === 0 ? ' primary' : ''), m.label);
      b.type = 'button';
      b.addEventListener('click', () => { closeLibDetail(); m.run(); });
      actions.appendChild(b);
    });
    const strip = $('lib-detail-plates');
    strip.textContent = '';
    $('lib-detail-plates-wrap').hidden = !(i.type === '3mf' && i.plates > 0);
    if (i.type === '3mf' && i.plates > 0) {
      strip.appendChild(el('div', 'hint', t('loading', 'Loading...')));
      post('library_plates', { id: i.id });
    }
    $('lib-detail').hidden = false;
  }
  function closeLibDetail() { $('lib-detail').hidden = true; libDetailId = ''; }
  function receiveLibPlates(msg) {
    if (msg.id !== libDetailId || $('lib-detail').hidden) return;
    const strip = $('lib-detail-plates');
    strip.textContent = '';
    const plates = Array.isArray(msg.plates) ? msg.plates : [];
    if (!plates.length) { $('lib-detail-plates-wrap').hidden = true; return; }
    for (const p of plates) {
      const img = typeof p.image === 'string' && p.image.indexOf('data:image/png;base64,') === 0 ? p.image : '';
      const b = el('button', 'plate');
      b.type = 'button';
      const th = el('div', 'pthumb');
      if (img) { const im = el('img'); im.alt = ''; im.src = img; th.appendChild(im); } else th.appendChild(svg(ICON_CUBE));
      b.appendChild(th);
      const nm = el('div', 'pname');
      nm.appendChild(el('b', '', t('plate', 'Plate') + ' ' + (Number(p.index) || '')));
      if (p.name) nm.appendChild(document.createTextNode(' · ' + p.name));
      b.title = nm.textContent;
      b.appendChild(nm);
      b.addEventListener('click', () => {
        const on = !b.classList.contains('active');
        for (const x of strip.querySelectorAll('.plate.active')) x.classList.remove('active');
        b.classList.toggle('active', on);
        setLibDetailThumb(on && img ? img : libDetailCover);
      });
      strip.appendChild(b);
    }
  }

  // ---- toast ----
  let toastTimer = 0;
  function toast(text, error) {
    const box = $('toast');
    box.textContent = text;
    box.classList.toggle('error', !!error);
    box.hidden = !text;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { box.hidden = true; }, error ? 9000 : 5000);
  }

  // ---- Vendors ----
  let venThumbObserver = null;
  let venMoreObserver = null;
  const askVenThumb = thumbAsker('vendor_thumbs', () => state.venThumbs, () => state.venAsked, 'keys');
  const ICON_CUBE_BIG = ICON_CUBE;

  function isoSeconds(s) {
    const n = Date.parse(s || '');
    return isNaN(n) ? 0 : Math.floor(n / 1000);
  }
  function connectorOf(id) {
    return state.ven ? state.ven.connectors.find((c) => c.id === id) : null;
  }

  function receiveVendors(msg) {
    const ven = {
      connectors: Array.isArray(msg.connectors) ? msg.connectors : [],
      items: Array.isArray(msg.items) ? msg.items : [],
    };
    const keys = new Set(ven.items.map((i) => i.key));
    for (const k of Object.keys(state.venThumbs)) if (!keys.has(k)) delete state.venThumbs[k];
    state.venAsked = new Set(Object.keys(state.venThumbs));
    const keepScroll = state.ven !== null;
    state.ven = ven;
    if (state.section === 'vendors') renderVendors(keepScroll);
    if (!$('ven-editor').hidden && editor.id) renderEditorSlots();
    if (!$('ven-detail').hidden && detailKey) {
      const item = ven.items.find((i) => i.key === detailKey);
      if (item) openDetail(item); else closeDetail();
    }
  }

  function applyVendorThumbs(images) {
    for (const key of Object.keys(images)) {
      const uri = images[key];
      if (typeof uri !== 'string' || !/^data:image\/(png|jpeg|gif|webp);base64,/.test(uri)) continue;
      state.venThumbs[key] = uri;
      const c = document.querySelector('#vendors-grid .card[data-id="' + CSS.escape(key) + '"] .thumb');
      if (c) { c.textContent = ''; const img = el('img'); img.alt = ''; img.src = uri; c.appendChild(img); }
      if (detailKey === key) setDetailThumb(uri);
    }
  }

  function venMatchesExcept(i, skip) {
    const f = state.venFilter;
    if (skip !== 'connector' && f.connector.size && !f.connector.has(i.connector)) return false;
    if (skip !== 'tag' && f.tag.size && !(i.tags || []).some((x) => f.tag.has(x))) return false;
    if (skip !== 'license' && f.license.size && !f.license.has(i.license || '')) return false;
    const c = connectorOf(i.connector);
    return matches([i.name, i.designer, i.description, i.license, c && c.name, c && c.vendor].concat(i.tags || [])
      .concat((i.subs || []).map((s) => s.name + ' ' + s.variant)), state.search);
  }

  function connectorCard(c) {
    const box = el('div', 'conn');
    const top = el('div', 'top');
    const title = el('div', 'title', c.name);
    title.title = c.name + (c.vendor ? ' · ' + c.vendor : '');
    top.appendChild(title);
    if (c.vendor && c.vendor !== c.name) top.appendChild(el('span', 'badge', c.vendor));
    const more = el('button', 'more');
    more.type = 'button';
    more.setAttribute('aria-label', 'More');
    more.appendChild(svg(ICON_MORE));
    const menu = [
      { label: t('full_sync', 'Fetch everything again'), run: () => post('vendor_sync', { id: c.id, full: true }) },
      { label: t('test', 'Test connection'), run: () => post('vendor_test', { id: c.id }) },
      { label: t('edit', 'Edit'), run: () => openEditor(c) },
      { label: t('export', 'Export'), run: () => post('vendor_export_spec', { id: c.id }) },
    ];
    if ((c.slots || []).some((s) => s.set)) menu.push({ label: t('forget', 'Forget credentials'), run: () => post('vendor_forget', { id: c.id }) });
    menu.push('-');
    menu.push({ label: t('delete_connector', 'Remove connector'), danger: true, run: () => post('vendor_delete', { id: c.id }) });
    more.addEventListener('click', (e) => { e.stopPropagation(); if (menuFor === more) closeMenu(); else openMenu(more, menu); });
    top.appendChild(more);
    box.appendChild(top);

    const lines = [];
    if (c.syncing) lines.push(t('syncing', 'Refreshing...'));
    else if (c.synced_at) lines.push(c.count + ' ' + t('models', 'models') + ' · ' + t('synced', 'Fetched') + ' ' + fmtDate(c.synced_at));
    else lines.push(t('never_synced', 'Not fetched yet'));
    for (const l of lines) box.appendChild(el('div', 'line', l));
    const q = c.quota || {};
    if (q.limit) box.appendChild(el('div', 'line' + (Number(q.used) >= Number(q.limit) ? ' warn' : ''),
      t('api_calls', 'API calls') + ' ' + (q.used || '?') + ' / ' + q.limit + (q.resets ? ' · ' + t('resets', 'resets') + ' ' + fmtDate(isoSeconds(q.resets) || 0) || q.resets : '')));
    if (c.downloads_limited) box.appendChild(el('div', 'line', t('limited_downloads', 'Downloads count against a limit')));
    const missing = (c.slots || []).filter((s) => !s.set);
    if (missing.length) box.appendChild(el('div', 'line warn', t('credentials', 'Credentials') + ': ' + missing.map((s) => s.label).join(', ') + ' ' + t('not_set', 'not set')));
    if (c.last_error) { const e = el('div', 'line err', c.last_error); box.appendChild(e); }

    const row = el('div', 'row');
    const sync = el('button', 'btn primary', c.syncing ? t('syncing', 'Refreshing...') : t('sync', 'Refresh list'));
    sync.type = 'button';
    sync.disabled = !!c.syncing;
    sync.addEventListener('click', () => post('vendor_sync', { id: c.id }));
    row.appendChild(sync);
    for (const slot of c.slots || []) {
      const b = el('button', 'btn', (slot.set ? t('change', 'Change') : t('set', 'Set')) + ' ' + slot.label);
      b.type = 'button';
      b.addEventListener('click', () => post('vendor_secret', { id: c.id, slot: slot.key }));
      row.appendChild(b);
    }
    box.appendChild(row);
    return box;
  }

  function renderVendorFilters() {
    const box = $('ven-filters');
    box.textContent = '';
    const ven = state.ven;
    if (!ven) return;
    const groups = [
      { key: 'connector', label: t('connector', 'Connector'), values: (i) => [i.connector], name: (v) => (connectorOf(v) || { name: v }).name },
      { key: 'tag', label: t('category', 'Category'), values: (i) => i.tags || [], name: (v) => v },
      { key: 'license', label: t('license', 'License'), values: (i) => [i.license || ''], name: (v) => v || t('license_not_set', 'Not set') },
    ];
    let any = false;
    for (const g of groups) {
      const all = new Map();
      for (const i of ven.items) for (const v of g.values(i)) all.set(v, 0);
      const sel = state.venFilter[g.key];
      for (const v of [...sel]) if (!all.has(v)) sel.delete(v);
      if (all.size < 2) continue;
      for (const i of ven.items) if (venMatchesExcept(i, g.key)) for (const v of g.values(i)) all.set(v, all.get(v) + 1);
      const row = el('div', 'filter-row');
      row.appendChild(el('span', 'label', g.label));
      const values = [...all.entries()].sort((a, b) => b[1] - a[1] || String(g.name(a[0])).localeCompare(String(g.name(b[0])))).slice(0, 40);
      for (const [v, n] of values) {
        const c = el('button', 'chip' + (sel.has(v) ? ' active' : ''), g.name(v));
        c.type = 'button';
        c.setAttribute('aria-pressed', sel.has(v) ? 'true' : 'false');
        c.appendChild(el('span', 'count', n));
        c.addEventListener('click', () => { if (sel.has(v)) sel.delete(v); else sel.add(v); renderVendors(); });
        row.appendChild(c);
      }
      box.appendChild(row);
      any = true;
    }
    if (any && Object.values(state.venFilter).some((f) => f.size)) {
      const row = el('div', 'filter-row');
      row.appendChild(el('span', 'label', ''));
      const c = el('button', 'chip clear', t('clear_filters', 'Clear filters'));
      c.type = 'button';
      c.addEventListener('click', () => { for (const f of Object.values(state.venFilter)) f.clear(); renderVendors(); });
      row.appendChild(c);
      box.appendChild(row);
    }
  }

  function itemMenu(i) {
    const menu = [];
    if (i.has_page) {
      menu.push({ label: t('open_page', 'Open vendor page'), run: () => post('vendor_open', { key: i.key }) });
      menu.push({ label: t('copy_link', 'Copy link'), run: () => post('vendor_copy', { key: i.key }) });
    }
    const subs = (i.subs || []).filter((s) => s.can_download);
    if (i.can_download) menu.push({ label: t('download', 'Download'), download: true, run: () => post('vendor_download', { key: i.key }) });
    else if (subs.length === 1) menu.push({ label: t('download', 'Download'), download: true, run: () => post('vendor_download', { key: i.key, sub: subs[0].id }) });
    return menu;
  }

  function vendorCard(i) {
    const c0 = connectorOf(i.connector);
    const badges = [];
    if ((i.subs || []).length > 1) badges.push({ text: i.subs.length + ' ' + t('files', 'files') });
    const meta = el('div', 'meta');
    const upd = fmtDate(isoSeconds(i.updated));
    if (upd) meta.appendChild(el('span', '', upd));
    const c = card({
      name: i.name,
      image: state.venThumbs[i.key] || '',
      badges: badges,
      lines: [i.designer ? t('by', 'by') + ' ' + i.designer : '', [c0 && c0.name].concat((i.tags || []).slice(0, 3)).filter(Boolean).join(' · '), i.license || ''],
      meta: meta,
      onOpen: () => openDetail(i),
      menu: itemMenu(i),
    });
    c.dataset.id = i.key;
    if (i.has_thumb && !state.venThumbs[i.key]) {
      if (venThumbObserver) venThumbObserver.observe(c); else askVenThumb(i.key);
    }
    return c;
  }

  function renderMoreVendors() {
    const end = Math.min(state.venShown.length, state.venRendered + LIB_PAGE);
    const frag = document.createDocumentFragment();
    for (let k = state.venRendered; k < end; ++k) frag.appendChild(vendorCard(state.venShown[k]));
    $('vendors-grid').appendChild(frag);
    state.venRendered = end;
  }

  function renderVendors(keepScroll) {
    const section = $('vendors');
    const scroll = keepScroll ? section.scrollTop : 0;
    const grid = $('vendors-grid'), empty = $('vendors-empty'), emptyText = $('vendors-empty-text');
    grid.textContent = '';
    $('ven-connectors').textContent = '';
    if (venThumbObserver) venThumbObserver.disconnect();
    if (venMoreObserver) venMoreObserver.disconnect();
    state.venShown = [];
    state.venRendered = 0;
    if (state.ven === null) { emptyText.textContent = t('loading', 'Loading...'); empty.hidden = false; return; }
    const ven = state.ven;
    for (const c of ven.connectors) $('ven-connectors').appendChild(connectorCard(c));
    renderVendorFilters();
    $('ven-status').textContent = ven.connectors.length ? ven.items.length + ' ' + t('models', 'models') : '';
    $('ven-csv').disabled = !ven.items.length;
    state.venShown = sortBy(ven.items.filter((i) => venMatchesExcept(i, '')), state.sort.vendors,
      (i) => i.name || '', (i) => isoSeconds(i.updated));
    if ('IntersectionObserver' in window) {
      venThumbObserver = new IntersectionObserver((entries) => {
        for (const e of entries) if (e.isIntersecting) { askVenThumb(e.target.dataset.id); venThumbObserver.unobserve(e.target); }
      }, { root: section, rootMargin: '400px' });
      venMoreObserver = new IntersectionObserver((entries) => {
        if (entries.some((e) => e.isIntersecting) && state.venRendered < state.venShown.length) renderMoreVendors();
      }, { root: section, rootMargin: '800px' });
    }
    renderMoreVendors();
    while (keepScroll && state.venRendered < state.venShown.length && grid.scrollHeight < scroll + section.clientHeight) renderMoreVendors();
    if (keepScroll) section.scrollTop = scroll;
    if (venMoreObserver) venMoreObserver.observe($('ven-more'));
    else while (state.venRendered < state.venShown.length) renderMoreVendors();
    empty.hidden = state.venShown.length > 0;
    emptyText.textContent = !ven.connectors.length ? t('vendors_empty', 'Connect a vendor\'s API to browse the models you have access to.')
      : !ven.items.length ? t('vendor_no_items', 'Nothing fetched yet. Set the credentials, then Refresh list.')
        : t('no_match', 'Nothing matches your search.');
  }

  // ---- item detail ----
  let detailKey = '';
  function setDetailThumb(uri) {
    const box = $('ven-detail-thumb');
    box.textContent = '';
    if (uri) { const img = el('img'); img.alt = ''; img.src = uri; box.appendChild(img); } else box.appendChild(svg(ICON_CUBE_BIG));
  }
  function openDetail(i) {
    closeMenu();
    detailKey = i.key;
    const c = connectorOf(i.connector);
    setDetailThumb(state.venThumbs[i.key] || '');
    if (i.has_thumb && !state.venThumbs[i.key]) askVenThumb(i.key);
    $('ven-detail-name').textContent = i.name;
    $('ven-detail-sub').textContent = [i.designer ? t('by', 'by') + ' ' + i.designer : '', c && c.name, fmtDate(isoSeconds(i.updated))]
      .concat(i.tags || []).concat(i.license ? [t('license', 'License') + ': ' + i.license] : []).filter(Boolean).join(' · ');
    $('ven-detail-desc').textContent = i.description || '';
    const actions = $('ven-detail-actions');
    actions.textContent = '';
    for (const m of itemMenu(i)) {
      // The files table has a Download per file.
      if (m === '-' || (m.download && (i.subs || []).length)) continue;
      const b = el('button', 'btn', m.label);
      b.type = 'button';
      b.addEventListener('click', m.run);
      actions.appendChild(b);
    }
    const files = $('ven-detail-files');
    files.textContent = '';
    if ((i.subs || []).length) {
      const table = el('table', 'files');
      const head = el('tr');
      for (const h of [t('files_label', 'Files'), '', t('plates_label', 'Plates'), '', '', '', '']) head.appendChild(el('th', '', h));
      head.children[1].textContent = t('variant', 'Variant');
      head.children[3].textContent = t('print_time', 'Time');
      head.children[4].textContent = t('size', 'Size');
      const thead = el('thead'); thead.appendChild(head); table.appendChild(thead);
      const body = el('tbody');
      for (const s of i.subs) {
        const tr = el('tr');
        tr.appendChild(el('td', '', s.name || s.id));
        tr.appendChild(el('td', '', s.variant || ''));
        tr.appendChild(el('td', '', s.plates || ''));
        tr.appendChild(el('td', '', s.print_time_s ? fmtDuration(s.print_time_s) : s.print_time_text || ''));
        tr.appendChild(el('td', '', fmtSize(s.size)));
        const sw = el('td');
        const box = el('span', 'swatches');
        for (const col of (s.colours || []).slice(0, 12)) { const d = el('span', 'swatch'); d.style.background = col; d.title = col; box.appendChild(d); }
        sw.appendChild(box);
        tr.appendChild(sw);
        const act = el('td');
        if (s.can_download) {
          const b = el('button', 'btn', t('download', 'Download'));
          b.type = 'button';
          b.addEventListener('click', () => post('vendor_download', { key: i.key, sub: s.id }));
          act.appendChild(b);
        }
        tr.appendChild(act);
        body.appendChild(tr);
      }
      table.appendChild(body);
      files.appendChild(table);
    }
    $('ven-detail').hidden = false;
  }
  function closeDetail() { $('ven-detail').hidden = true; detailKey = ''; }

  // ---- connector editor ----
  // One field per line of the template; `path` is where it lives in the connector's JSON.
  const EDITOR = [
    { group: 'Basics', fields: [
      { path: 'name', label: 'Name' },
      { path: 'vendor', label: 'Vendor tag', list: 'dl-vendor' },
      { path: 'license', label: 'Your license for its models (when a model gives none)', list: 'dl-license', hint: 'Commercial' },
      { path: 'base_url', label: 'API address', wide: true, hint: 'https://api.vendor.example/v1' },
    ] },
    { group: 'Sign-in', headers: true, fields: [
      { path: 'auth.type', label: 'Type', options: [['none', 'None'], ['bearer', 'Bearer token'], ['header', 'Key in a header'], ['query', 'Key in the address'], ['basic', 'User name and password']] },
      { path: 'auth.name', label: 'Header or parameter name', hint: 'X-API-Key' },
    ] },
    { group: 'Model list', fields: [
      { path: 'list.path', label: 'Path', hint: '/library' },
      { path: 'list.items', label: 'Items in the answer at', hint: 'models' },
      { path: 'list.query', label: 'Fixed parameters (name=value per line)', query: true, wide: true },
    ] },
    { group: 'Pages', fields: [
      { path: 'list.paging.type', label: 'Paging', options: [['none', 'One page'], ['page', 'Page number'], ['offset', 'Offset'], ['cursor', 'Cursor token'], ['next', 'Next-page link']] },
      { path: 'list.paging.param', label: 'Page, offset or cursor parameter' },
      { path: 'list.paging.start', label: 'First page', number: true },
      { path: 'list.paging.size_param', label: 'Page size parameter', hint: 'limit' },
      { path: 'list.paging.size', label: 'Page size', number: true },
      { path: 'list.paging.has_more', label: '"More pages" flag at', hint: 'has_next' },
      { path: 'list.paging.total', label: 'Total count at', hint: 'total' },
      { path: 'list.paging.cursor', label: 'Next cursor or link at' },
      { path: 'list.since.param', label: 'Only changes since: parameter', hint: 'updated_since' },
      { path: 'list.since.field', label: 'Compared with item field', hint: 'files_updated_at' },
    ] },
    { group: 'Item fields', fields: [
      { path: 'fields.id', label: 'Id' }, { path: 'fields.name', label: 'Name' },
      { path: 'fields.thumbnail', label: 'Thumbnail URL' },
      { path: 'fields.page_url', label: 'Page URL (field, or template like https://site/m/{slug})' },
      { path: 'fields.designer', label: 'Designer' }, { path: 'fields.license', label: 'License' },
      { path: 'fields.tags', label: 'Tags or category' },
      { path: 'fields.updated', label: 'Updated' }, { path: 'fields.description', label: 'Description' },
    ] },
    { group: 'Files of an item (optional)', fields: [
      { path: 'files.path', label: 'Files at', hint: 'print_profiles' },
      { path: 'files.fields.id', label: 'Id' }, { path: 'files.fields.name', label: 'Name' },
      { path: 'files.fields.variant', label: 'Variant' }, { path: 'files.fields.size', label: 'Size (bytes)' },
      { path: 'files.fields.plates', label: 'Plates' }, { path: 'files.fields.print_time', label: 'Print time (s)' },
      { path: 'files.fields.colours', label: 'Colours list' }, { path: 'files.fields.colour', label: 'Colour in each', hint: 'hex' },
    ] },
    { group: 'Download (optional)', fields: [
      { path: 'download.path', label: 'Endpoint path', hint: '/models/{id}/download?profile={sub.id}', wide: true },
      { path: 'download.url_field', label: 'File link in its answer at', hint: 'download_url' },
      { path: 'download.direct_field', label: 'Or: direct link field of a file' },
      { path: 'download.limited', label: 'Downloads count against a limit (ask first)', check: true },
    ] },
    { group: 'Quota headers (optional)', fields: [
      { path: 'quota.used', label: 'Used', hint: 'X-Api-Calls-Used' },
      { path: 'quota.limit', label: 'Limit', hint: 'X-Api-Calls-Limit' },
      { path: 'quota.resets', label: 'Resets', hint: 'X-Api-Period-Resets' },
    ] },
  ];
  const NEW_SPEC = {
    name: '', vendor: '', license: '', base_url: 'https://', auth: { type: 'bearer', name: '' }, headers: [],
    list: { path: '/', items: '', query: {}, paging: { type: 'page', param: 'page', start: 1, size_param: 'limit', size: 100, has_more: '', total: '', cursor: '' },
      since: { param: '', field: '' } },
    fields: { id: 'id', name: 'name', thumbnail: 'thumbnail', page_url: '', designer: '', license: '', tags: '', updated: '', description: '' },
    files: { path: '', fields: {} }, download: { path: '', url_field: '', direct_field: '', limited: false },
    quota: { used: '', limit: '', resets: '' },
  };
  const editor = { id: '', spec: null, json: false };

  function getPath(o, path) { return path.split('.').reduce((a, k) => (a && typeof a === 'object' ? a[k] : undefined), o); }
  function setPath(o, path, v) {
    const ks = path.split('.');
    let cur = o;
    for (const k of ks.slice(0, -1)) { if (!cur[k] || typeof cur[k] !== 'object') cur[k] = {}; cur = cur[k]; }
    cur[ks[ks.length - 1]] = v;
  }

  function openEditor(c) {
    closeMenu();
    editor.id = c ? c.id : '';
    fillLicenses();
    editor.spec = JSON.parse(JSON.stringify(c ? c.spec : NEW_SPEC));
    editor.json = false;
    $('ven-editor-error').hidden = true;
    $('ven-editor-title').textContent = c ? c.name : t('add_connector', 'Add connector');
    renderEditor();
    $('ven-editor').hidden = false;
  }
  function closeEditor() { $('ven-editor').hidden = true; }
  function editorDone() { if (!$('ven-editor').hidden) closeEditor(); }
  function editorError(text) {
    const e = $('ven-editor-error');
    e.textContent = text;
    e.hidden = !text;
    if (text) e.scrollIntoView({ block: 'nearest' });
  }

  function renderEditorSlots() {
    const box = document.getElementById('ven-editor-slots');
    if (!box) return;
    box.textContent = '';
    const c = connectorOf(editor.id);
    if (!c) { box.appendChild(el('p', 'hint', 'Save the connector first, then set its credentials here or on its card.')); return; }
    if (!(c.slots || []).length) { box.appendChild(el('p', 'hint', t('none', 'None'))); return; }
    for (const s of c.slots) {
      const row = el('div', 'slot');
      row.appendChild(el('span', 'what', s.label + ': ' + (s.set ? (c.secure ? t('saved_secure', 'saved in your system\'s credential store') : t('saved_session', 'kept until EdgeSlicer closes')) : t('not_set', 'not set'))));
      const b = el('button', 'btn', s.set ? t('change', 'Change') : t('set', 'Set'));
      b.type = 'button';
      b.addEventListener('click', () => post('vendor_secret', { id: c.id, slot: s.key }));
      row.appendChild(b);
      box.appendChild(row);
    }
  }

  function renderHeaders(box) {
    box.textContent = '';
    const list = Array.isArray(editor.spec.headers) ? editor.spec.headers : (editor.spec.headers = []);
    list.forEach((h, n) => {
      const row = el('div', 'hdr-row');
      const name = el('label'); name.appendChild(el('span', '', 'Header'));
      const ni = el('input'); ni.type = 'text'; ni.value = h.name || ''; ni.addEventListener('input', () => { h.name = ni.value.trim(); });
      name.appendChild(ni); row.appendChild(name);
      const val = el('label'); val.appendChild(el('span', '', 'Value'));
      const vi = el('input'); vi.type = 'text'; vi.value = h.secret ? '' : h.value || ''; vi.disabled = !!h.secret;
      vi.placeholder = h.secret ? 'set on the connector card' : '';
      vi.addEventListener('input', () => { h.value = vi.value; });
      val.appendChild(vi); row.appendChild(val);
      const sec = el('label', 'check'); const cb = el('input'); cb.type = 'checkbox'; cb.checked = !!h.secret;
      cb.addEventListener('change', () => { h.secret = cb.checked; if (h.secret) delete h.value; renderHeaders(box); });
      sec.appendChild(cb); sec.appendChild(el('span', '', 'Secret')); row.appendChild(sec);
      const rm = el('button', 'btn', t('remove', 'Remove')); rm.type = 'button';
      rm.addEventListener('click', () => { list.splice(n, 1); renderHeaders(box); });
      row.appendChild(rm);
      box.appendChild(row);
    });
    const add = el('button', 'btn', '+ Header'); add.type = 'button';
    add.addEventListener('click', () => { list.push({ name: '', value: '' }); renderHeaders(box); });
    box.appendChild(add);
  }

  function renderEditor() {
    const body = $('ven-editor-body'), text = $('ven-editor-json');
    body.hidden = editor.json;
    text.hidden = !editor.json;
    $('ven-editor-mode').textContent = editor.json ? t('edit_form', 'Edit as form') : t('edit_json', 'Edit as JSON');
    if (editor.json) { text.value = JSON.stringify(editor.spec, null, 2); return; }
    body.textContent = '';
    const creds = el('fieldset', 'group');
    creds.appendChild(el('legend', '', t('credentials', 'Credentials')));
    const slots = el('div', 'slots'); slots.id = 'ven-editor-slots';
    creds.appendChild(slots);
    for (const g of EDITOR) {
      const fs = el('fieldset', 'group');
      fs.appendChild(el('legend', '', g.group));
      const grid = el('div', 'form-grid');
      for (const f of g.fields) {
        const l = el('label', f.check ? 'check' : 'field');
        if (f.wide) l.classList.add('wide');
        let input;
        const v = getPath(editor.spec, f.path);
        if (f.options) {
          input = el('select');
          for (const [value, label] of f.options) { const op = el('option', '', label); op.value = value; input.appendChild(op); }
          input.value = v || f.options[0][0];
          input.addEventListener('change', () => setPath(editor.spec, f.path, input.value));
        } else if (f.check) {
          input = el('input'); input.type = 'checkbox'; input.checked = !!v;
          input.addEventListener('change', () => setPath(editor.spec, f.path, input.checked));
        } else if (f.query) {
          input = el('textarea');
          input.spellcheck = false;
          input.value = Object.entries(v || {}).map(([k, x]) => k + '=' + x).join('\n');
          input.addEventListener('input', () => {
            const q = {};
            for (const line of input.value.split('\n')) { const at = line.indexOf('='); if (at > 0) q[line.slice(0, at).trim()] = line.slice(at + 1).trim(); }
            setPath(editor.spec, f.path, q);
          });
        } else {
          input = el('input');
          input.type = f.number ? 'number' : 'text';
          input.value = v === undefined || v === null ? '' : String(v);
          if (f.hint) input.placeholder = f.hint;
          if (f.list) input.setAttribute('list', f.list);
          input.spellcheck = false;
          input.addEventListener('input', () => setPath(editor.spec, f.path, f.number ? Number(input.value) : input.value.trim()));
        }
        if (f.check) { l.appendChild(input); l.appendChild(el('span', '', f.label)); }
        else { l.appendChild(el('span', '', f.label)); l.appendChild(input); }
        grid.appendChild(l);
      }
      fs.appendChild(grid);
      if (g.headers) {
        fs.appendChild(el('p', 'hint', 'Extra headers. Mark a header secret to keep its value in the credential store.'));
        const hb = el('div');
        renderHeaders(hb);
        fs.appendChild(hb);
      }
      body.appendChild(fs);
      if (g.group === 'Basics') body.appendChild(creds);
    }
    renderEditorSlots();
  }

  function readEditorJson() {
    try { editor.spec = JSON.parse($('ven-editor-json').value); return true; }
    catch (e) { editorError('JSON: ' + e.message); return false; }
  }
  function toggleEditorMode() {
    if (editor.json && !readEditorJson()) return;
    editorError('');
    editor.json = !editor.json;
    renderEditor();
  }
  function saveEditor() {
    if (editor.json && !readEditorJson()) return;
    editorError('');
    const spec = JSON.parse(JSON.stringify(editor.spec));
    if (editor.id) spec.id = editor.id; else delete spec.id;
    post('vendor_save', { spec: spec });
  }

  // ---- Library folders ----
  function openFolders() {
    closeMenu();
    renderFolders();
    $('folders').hidden = false;
    $('folders-done').focus();
  }
  function closeFolders() {
    // An edit still in a field is saved on its change event, which blur fires first.
    if (document.activeElement && $('folders').contains(document.activeElement)) document.activeElement.blur();
    $('folders').hidden = true;
  }

  function fillDatalist(id, values) {
    const dl = $(id);
    dl.textContent = '';
    for (const v of [...new Set(values.filter(Boolean))].sort()) {
      const o = el('option');
      o.value = v;
      dl.appendChild(o);
    }
  }

  // Licence names to pick from: the common kinds, then any already in use.
  function fillLicenses() {
    const used = (state.lib ? state.lib.folders.map((f) => f.license).concat(state.lib.items.map((i) => i.license)) : [])
      .concat(state.ven ? state.ven.connectors.map((c) => c.spec && c.spec.license) : []);
    fillDatalist('dl-license', [t('license_commercial', 'Commercial'), t('license_personal', 'Personal use only'),
      'CC BY', 'CC BY-SA', 'CC BY-NC', 'CC BY-NC-SA', 'CC0'].concat(used));
  }

  function renderFolders() {
    const list = $('folder-list');
    // Re-rendering under a field being typed in would lose the text: wait for its change event.
    if (list.contains(document.activeElement) && document.activeElement.tagName === 'INPUT' && document.activeElement.type === 'text') return;
    list.textContent = '';
    const folders = state.lib ? state.lib.folders : [];
    fillDatalist('dl-category', folders.map((f) => f.category));
    fillDatalist('dl-vendor', folders.map((f) => f.vendor).concat(state.ven ? state.ven.connectors.map((c) => c.vendor) : []));
    fillLicenses();
    if (!folders.length) list.appendChild(el('p', 'hint', t('library_no_folders', 'Add the folders where you keep your models to browse them here.')));
    for (const f of folders) {
      const row = el('div', 'folder');
      const p = el('div', 'path', f.path);
      p.title = f.path;
      row.appendChild(p);
      const st = el('div', 'state' + (f.scanned && !f.online ? ' off' : ''),
        !f.scanned ? t('not_scanned', 'Not scanned yet')
          : !f.online ? t('offline', 'Not reachable, showing the files from the last scan')
            : f.files + ' ' + t('files', 'files'));
      st.title = st.textContent;
      row.appendChild(st);
      const field = (key, label, listId) => {
        const l = el('label', 'field');
        l.appendChild(el('span', '', label));
        const i = el('input');
        i.type = 'text';
        i.maxLength = 80;
        i.value = f[key] || '';
        i.setAttribute('list', listId);
        i.addEventListener('change', () => {
          const v = i.value.trim();
          if (v === (f[key] || '')) return;
          f[key] = v;
          post('library_update_folder', { path: f.path, [key]: v });
        });
        i.addEventListener('keydown', (e) => { if (e.key === 'Enter') i.blur(); });
        l.appendChild(i);
        return l;
      };
      row.appendChild(field('category', t('category', 'Category'), 'dl-category'));
      row.appendChild(field('vendor', t('vendor', 'Vendor'), 'dl-vendor'));
      row.appendChild(field('license', t('license', 'License'), 'dl-license'));
      const end = el('div', 'row-end');
      const chk = el('label', 'check');
      const cb = el('input');
      cb.type = 'checkbox';
      cb.checked = f.recursive !== false;
      cb.addEventListener('change', () => post('library_update_folder', { path: f.path, recursive: cb.checked }));
      chk.appendChild(cb);
      chk.appendChild(el('span', '', t('include_subfolders', 'Include subfolders')));
      end.appendChild(chk);
      const rm = el('button', 'btn danger', t('remove', 'Remove'));
      rm.type = 'button';
      rm.addEventListener('click', () => post('library_remove_folder', { path: f.path }));
      end.appendChild(rm);
      row.appendChild(end);
      list.appendChild(row);
    }
  }

  window.HomeApp = {
    receive: function (msg) {
      if (!msg || typeof msg !== 'object') return;
      switch (msg.type) {
        case 'init':
          state.strings = msg.strings || {};
          applyTheme(msg.theme);
          applyStrings();
          $('rail-models').hidden = msg.models !== true;
          showSection(msg.section || state.section, false);
          break;
        case 'models':
          $('rail-models').hidden = msg.enabled !== true;
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
          applyThumbs(msg.images || {}, state.thumbs, 'history-grid');
          break;
        case 'library':
          receiveLibrary(msg);
          break;
        case 'library_progress':
          if (state.lib) {
            state.lib.scanning = !!msg.scanning;
            state.libSeen = Number(msg.files) || 0;
            if (state.section === 'library') renderLibraryStatus();
          }
          break;
        case 'library_thumbs':
          applyThumbs(msg.images || {}, state.libThumbs, 'library-grid');
          if (libDetailId && state.libThumbs[libDetailId] && !libDetailCover) {
            libDetailCover = state.libThumbs[libDetailId];
            if (!$('lib-detail-plates').querySelector('.plate.active')) setLibDetailThumb(libDetailCover);
          }
          break;
        case 'library_plates':
          receiveLibPlates(msg);
          break;
        case 'vendors':
          receiveVendors(msg);
          break;
        case 'vendor_thumbs':
          applyVendorThumbs(msg.images || {});
          break;
        case 'vendor_notice':
          toast(String(msg.text || ''), !!msg.error);
          break;
        case 'vendor_saved':
          editorDone();
          break;
        case 'vendor_invalid':
          editorError(String(msg.error || ''));
          break;
      }
    },
  };

  // ---- wiring ----
  if (/dark/i.test(navigator.userAgent)) document.documentElement.classList.add('dark');
  for (const b of document.querySelectorAll('.rail-item[data-section]'))
    b.addEventListener('click', () => showSection(b.dataset.section, true));
  // Models is not a section of this page: the slicer swaps in its own browser panel.
  $('rail-models').addEventListener('click', () => post('models_open'));
  let searchTimer = 0;
  $('search').addEventListener('input', () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => { state.search = $('search').value.trim(); render(); }, 120);
  });
  $('sort').addEventListener('change', () => { state.sort[state.section] = $('sort').value; render(); });
  $('refresh').addEventListener('click', () => {
    // Vendors' lists are metered: Refresh only redraws them; each connector has its own Refresh list.
    if (state.section === 'vendors') { post('vendor_state'); return; }
    $('refresh').classList.add('spin'); post('home_refresh'); setTimeout(stopSpin, 3000);
  });
  $('new-project').addEventListener('click', () => post('project_new'));
  $('open-project').addEventListener('click', () => post('project_open'));
  $('history-settings').addEventListener('click', () => post('history_settings'));
  $('lib-manage').addEventListener('click', openFolders);
  $('ven-add').addEventListener('click', () => openEditor(null));
  $('ven-import').addEventListener('click', () => post('vendor_import'));
  $('ven-csv').addEventListener('click', () => post('vendor_csv', { keys: state.venShown.map((i) => i.key) }));
  $('ven-detail-close').addEventListener('click', closeDetail);
  $('ven-detail').addEventListener('click', (e) => { if (e.target === $('ven-detail')) closeDetail(); });
  $('ven-editor-cancel').addEventListener('click', closeEditor);
  $('ven-editor-save').addEventListener('click', saveEditor);
  $('ven-editor-mode').addEventListener('click', toggleEditorMode);
  document.addEventListener('keydown', (e) => {
    if (e.key !== 'Escape') return;
    if (!$('ven-editor').hidden) closeEditor();
    else if (!$('ven-detail').hidden) closeDetail();
    else if (!$('lib-detail').hidden) closeLibDetail();
  });
  $('lib-detail-close').addEventListener('click', closeLibDetail);
  $('lib-detail').addEventListener('click', (e) => { if (e.target === $('lib-detail')) closeLibDetail(); });
  $('lib-empty-add').addEventListener('click', () => post('library_add_folder'));
  $('folder-add').addEventListener('click', () => post('library_add_folder'));
  $('folders-done').addEventListener('click', closeFolders);
  $('folders').addEventListener('click', (e) => { if (e.target === $('folders')) closeFolders(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('folders').hidden) closeFolders(); });

  applyStrings();
  showSection('recent', false);
  post('home_ready');
})();
