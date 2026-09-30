// The Home tab (hosted by src/slic3r/GUI/HomePanel.cpp).
//
// The page only draws. It asks the slicer for everything with window.wx.postMessage(JSON) and the
// slicer answers through window.HomeApp.receive({type, ...}):
//   init    {strings, section}             translated strings, the section to open
//   recent  {items: [{path, name, folder, exists, time, image}]}
//   history {enabled, items: [card]}       cards as HomeTabLogic.cpp history_card() builds them
//   thumbs  {images: {id: dataUri}}        Print History thumbnails, asked for as cards scroll in
//   library {folders: [{path, recursive, category, vendor, scanned, online, files}],
//            items: [item], hidden, scanning, files_seen, scanned_at}
//                                          items as LibraryIndex.cpp page_item() builds them (no paths
//                                          of files: the page acts on ids)
//   library_progress {scanning, files}     a scan is running
//   library_thumbs {images: {id: dataUri}} Library covers, asked for as cards scroll in
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
    sort: { recent: 'newest', library: 'newest', history: 'newest' },
    lib: null,          // the last 'library' message
    libThumbs: {},
    libAsked: new Set(),
    libFilter: { category: new Set(), vendor: new Set(), type: new Set() },
    libShown: [],
    libRendered: 0,
    libSeen: 0,
  };
  const SECTIONS = ['recent', 'library', 'history'];
  const SORTS = {
    recent: ['newest', 'oldest', 'name'],
    library: ['newest', 'oldest', 'added', 'name', 'size'],
    history: ['newest', 'oldest', 'name'],
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
  function thumbAsker(command, have, asked) {
    let timer = 0;
    const queue = [];
    return function (id) {
      if (have()[id] || asked().has(id)) return;
      asked().add(id);
      queue.push(id);
      if (!timer) timer = setTimeout(() => {
        timer = 0;
        while (queue.length) post(command, { ids: queue.splice(0, 40) });
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
    $('title').textContent = t(section, { recent: 'Recent', library: 'Library', history: 'Print History' }[section]);
    fillSort(section);
    $('refresh').classList.toggle('spin', section === 'library' && !!state.lib && state.lib.scanning);
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

  function libMatchesExcept(i, skip) {
    const f = state.libFilter;
    if (skip !== 'category' && f.category.size && !f.category.has(i.category || '')) return false;
    if (skip !== 'vendor' && f.vendor.size && !f.vendor.has(i.vendor || '')) return false;
    if (skip !== 'type' && f.type.size && !f.type.has(i.type)) return false;
    return matches([i.name, i.title, i.designer, i.category, i.vendor, folderLabel(i)].concat(i.plate_names || []), state.search);
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
    const groups = [
      { key: 'category', label: t('category', 'Category'), value: (i) => i.category || '', name: (v) => v || t('none', 'None') },
      { key: 'vendor', label: t('vendor', 'Vendor'), value: (i) => i.vendor || '', name: (v) => v || t('none', 'None') },
      { key: 'type', label: t('type', 'Type'), value: (i) => i.type, name: (v) => TYPE_LABELS[v] || v },
    ];
    let any = false;
    for (const g of groups) {
      const all = new Set(items.map(g.value));
      const sel = state.libFilter[g.key];
      for (const v of [...sel]) if (!all.has(v)) sel.delete(v);
      // A group is worth showing when it can tell files apart.
      if (all.size < 2) continue;
      const counts = new Map();
      for (const i of items) if (libMatchesExcept(i, g.key)) counts.set(g.value(i), (counts.get(g.value(i)) || 0) + 1);
      const row = el('div', 'filter-row');
      row.appendChild(el('span', 'label', g.label));
      const values = [...all].sort((a, b) => (a === '') - (b === '') || String(g.name(a)).localeCompare(String(g.name(b)), undefined, { sensitivity: 'base' }));
      for (const v of values) {
        const c = el('button', 'chip' + (sel.has(v) ? ' active' : ''), g.name(v));
        c.type = 'button';
        c.setAttribute('aria-pressed', sel.has(v) ? 'true' : 'false');
        c.appendChild(el('span', 'count', counts.get(v) || 0));
        c.addEventListener('click', () => {
          if (sel.has(v)) sel.delete(v); else sel.add(v);
          renderLibrary();
        });
        row.appendChild(c);
      }
      box.appendChild(row);
      any = true;
    }
    const active = Object.values(state.libFilter).some((f) => f.size);
    if (any && active) {
      const row = el('div', 'filter-row');
      row.appendChild(el('span', 'label', ''));
      const c = el('button', 'chip clear', t('clear_filters', 'Clear filters'));
      c.type = 'button';
      c.addEventListener('click', () => { for (const f of Object.values(state.libFilter)) f.clear(); renderLibrary(); });
      row.appendChild(c);
      box.appendChild(row);
    }
  }

  function libraryCard(i) {
    const badges = [{ text: TYPE_LABELS[i.type] || i.type }];
    if (i.sliced) badges.push({ text: t('sliced', 'Sliced'), cls: 'accent' });
    if (i.plates > 1) badges.push({ text: i.plates + ' ' + t('plates', 'plates') });
    const is3mf = i.type === '3mf';
    const menu = [];
    if (is3mf) menu.push({ label: t('open', 'Open'), run: () => post('library_open', { id: i.id }) });
    menu.push({ label: t('add_to_plate', 'Add to current project'), run: () => post('library_import', { id: i.id }) });
    menu.push({ label: t('show_in_folder', 'Show in folder'), run: () => post('library_reveal', { id: i.id }) });
    menu.push('-');
    menu.push({ label: t('hide', 'Hide from Library'), run: () => post('library_hide', { id: i.id }) });
    const meta = el('div', 'meta');
    for (const x of [fmtDate(i.mtime), fmtSize(i.size)]) if (x) meta.appendChild(el('span', '', x));
    const tags = [i.category, i.vendor].filter(Boolean).join(' · ');
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

  function renderFolders() {
    const list = $('folder-list');
    // Re-rendering under a field being typed in would lose the text: wait for its change event.
    if (list.contains(document.activeElement) && document.activeElement.tagName === 'INPUT' && document.activeElement.type === 'text') return;
    list.textContent = '';
    const folders = state.lib ? state.lib.folders : [];
    fillDatalist('dl-category', folders.map((f) => f.category));
    fillDatalist('dl-vendor', folders.map((f) => f.vendor));
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
  $('lib-manage').addEventListener('click', openFolders);
  $('lib-empty-add').addEventListener('click', () => post('library_add_folder'));
  $('folder-add').addEventListener('click', () => post('library_add_folder'));
  $('folders-done').addEventListener('click', closeFolders);
  $('folders').addEventListener('click', (e) => { if (e.target === $('folders')) closeFolders(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('folders').hidden) closeFolders(); });

  applyStrings();
  showSection('recent', false);
  post('home_ready');
})();
