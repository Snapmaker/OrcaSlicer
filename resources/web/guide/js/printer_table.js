// Printer Selection as one table, shared by guide/24 (sidebar "Select/Remove printers") and
// guide/21 (the setup wizard's printer page).
//
// One row per printer model (all its nozzle variants together, as before): Enabled, icon, Brand,
// Model, X/Y/Z (mm) and Toolheads. Every column sorts (click the title: up, down, back to the
// original order) and filters (the funnel: a value list for Enabled, Brand and Toolheads, text for
// Model, min/max for the sizes), plus one search box over brand and model. All / Clear all act
// on the rows currently shown.
//
// The rows come from the slicer's "response_userguide_profile" (WebGuideDialog.cpp): per model
// vendor, model, name, cover, nozzle_diameter ("0.4;0.2"), nozzle_selected (non-empty = enabled)
// and size_x / size_y / size_z / extruders (null when unknown, shown as a dash).
//
// The body is virtualised (only the rows in view exist in the DOM), so ~1000 models stay light
// and the cover images load only for rows that scroll into view.
var PrinterTable = (function () {
	'use strict';

	var VENDOR_PRIORITY = ['Snapmaker'];
	var BRAND_NAMES = { BBL: 'Bambu Lab', Custom: 'Custom Printer', Other: 'Orca colosseum' };
	var OVERSCAN = 8;

	// ---- text: the page language when LangText has the key, English otherwise ----
	function lang() {
		var l = null;
		try { l = (typeof GetQueryString === 'function') ? GetQueryString('lang') : null; } catch (e) { }
		if (!l) { try { l = localStorage.getItem('BambuWebLang'); } catch (e) { } }
		return l;
	}
	function T(key, fallback, args) {
		var s = fallback;
		try {
			var L = (typeof LangText !== 'undefined') ? LangText : null;
			var l = lang();
			if (L && l && L[l] && L[l][key]) s = L[l][key];
			else if (L && L.en && L.en[key]) s = L.en[key];
		} catch (e) { }
		if (args) s = s.replace(/\{(\w+)\}/g, function (m, k) { return args.hasOwnProperty(k) ? args[k] : m; });
		return s;
	}

	var COLS = [
		{ key: 'checked', tid: 'pt_enabled', label: 'Enabled', w: '88px', kind: 'bool', filter: 'set', cls: 'check' },
		{ key: 'icon', label: '', w: '40px', kind: 'none', cls: 'icon' },
		{ key: 'brand', tid: 'pt_brand', label: 'Brand', w: 'minmax(96px, 1fr)', kind: 'text', filter: 'set', listSearch: true },
		{ key: 'name', tid: 'pt_model', label: 'Model', w: 'minmax(150px, 2.2fr)', kind: 'text', filter: 'text' },
		{ key: 'x', tid: 'pt_x', label: 'X mm', w: '84px', kind: 'num', filter: 'range', cls: 'num',
			tipTid: 'pt_x_tip', tip: 'Bed width, from the printer\'s default (0.4 mm nozzle) preset' },
		{ key: 'y', tid: 'pt_y', label: 'Y mm', w: '84px', kind: 'num', filter: 'range', cls: 'num',
			tipTid: 'pt_y_tip', tip: 'Bed depth, from the printer\'s default (0.4 mm nozzle) preset' },
		{ key: 'z', tid: 'pt_z', label: 'Z mm', w: '84px', kind: 'num', filter: 'range', cls: 'num',
			tipTid: 'pt_z_tip', tip: 'Printable height, from the printer\'s default (0.4 mm nozzle) preset' },
		{ key: 'th', tid: 'pt_toolheads', label: 'Toolheads', w: '104px', kind: 'num', filter: 'set', cls: 'num',
			tipTid: 'pt_toolheads_tip', tip: 'Number of extruders / toolheads' }
	];
	function col(key) { for (var i = 0; i < COLS.length; i++) if (COLS[i].key === key) return COLS[i]; return null; }

	var collator = (typeof Intl !== 'undefined' && Intl.Collator) ? new Intl.Collator(undefined, { numeric: true, sensitivity: 'base' }) : null;
	function cmpText(a, b) { return collator ? collator.compare(a, b) : (a < b ? -1 : a > b ? 1 : 0); }

	// ---- state ----
	var rows = [];          // every model
	var byKey = {};         // vendor + "\n" + model -> [rows] (a duplicate entry toggles along)
	var view = [];          // rows shown, in display order
	var state = {
		search: '',
		sort: { key: null, dir: 1 },
		f: { checked: {}, brand: {}, th: {}, name: '', x: { min: null, max: null }, y: { min: null, max: null }, z: { min: null, max: null } }
	};
	var el = {};            // DOM refs
	var rendered = {};      // row id -> element currently in the body
	var pop = null;         // open filter popover { node, col }
	var rowH = 36;
	var rafPending = false;

	// ---- values ----
	function num(v) { return (typeof v === 'number' && isFinite(v)) ? v : null; }
	function fmt(v) {
		if (v === null) return '—';
		var r = Math.round(v);
		return Math.abs(v - r) < 0.1 ? String(r) : v.toFixed(1);
	}
	// The key a "set" filter stores for a row.
	function setKey(key, r) {
		if (key === 'checked') return r.checked ? 'yes' : 'no';
		if (key === 'th') return r.th === null ? '' : String(r.th);
		return r[key];
	}
	function setLabel(key, k) {
		if (key === 'checked') return k === 'yes' ? T('pt_on', 'Enabled') : T('pt_off', 'Not enabled');
		if (key === 'th') return k === '' ? '—' : k;
		return k;
	}
	function sortVal(key, r) {
		if (key === 'checked') return r.checked ? 0 : 1;
		if (key === 'brand' || key === 'name') return r[key] || '';
		return r[key];
	}
	function tokens(s) { return (s || '').toLowerCase().match(/\S+/g) || []; }
	function isEmpty(o) { for (var k in o) if (o.hasOwnProperty(k)) return false; return true; }

	function passes(r, except) {
		var f = state.f;
		if (state.search) {
			var hay = r.hay, t = tokens(state.search);
			for (var i = 0; i < t.length; i++) if (hay.indexOf(t[i]) < 0) return false;
		}
		var sets = ['checked', 'brand', 'th'];
		for (var s = 0; s < sets.length; s++) {
			var k = sets[s];
			if (k !== except && f[k][setKey(k, r)]) return false;
		}
		if (except !== 'name' && f.name) {
			var n = r.name.toLowerCase(), nt = tokens(f.name);
			for (var j = 0; j < nt.length; j++) if (n.indexOf(nt[j]) < 0) return false;
		}
		var rng = ['x', 'y', 'z'];
		for (var q = 0; q < rng.length; q++) {
			var rk = rng[q], lim = f[rk];
			if (rk === except || (lim.min === null && lim.max === null)) continue;
			var v = r[rk];
			if (v === null) return false;
			if (lim.min !== null && v < lim.min) return false;
			if (lim.max !== null && v > lim.max) return false;
		}
		return true;
	}
	function filterActive(key) {
		var f = state.f;
		if (key === 'checked' || key === 'brand' || key === 'th') return !isEmpty(f[key]);
		if (key === 'name') return !!f.name;
		if (key === 'x' || key === 'y' || key === 'z') return f[key].min !== null || f[key].max !== null;
		return false;
	}
	function anyFilter() {
		if (state.search) return true;
		for (var i = 0; i < COLS.length; i++) if (filterActive(COLS[i].key)) return true;
		return false;
	}

	function compare(a, b) {
		var k = state.sort.key;
		if (!k) return a.order - b.order;
		var va = sortVal(k, a), vb = sortVal(k, b);
		if (va === null && vb === null) return a.order - b.order;
		if (va === null) return 1;      // unknown values last in either direction
		if (vb === null) return -1;
		var c = (typeof va === 'string') ? cmpText(va, vb) : va - vb;
		if (c !== 0) return c * state.sort.dir;
		if (k !== 'name') { c = cmpText(a.name, b.name); if (c !== 0) return c; }
		return a.order - b.order;
	}

	function recompute(keepScroll) {
		var out = [];
		for (var i = 0; i < rows.length; i++) if (passes(rows[i], null)) out.push(rows[i]);
		out.sort(compare);
		view = out;
		el.spacer.style.height = (view.length * rowH) + 'px';
		if (!keepScroll) el.body.scrollTop = 0;
		el.empty.style.display = (rows.length && !view.length) ? 'block' : 'none';
		renderRows(true);
		renderHead();
		renderChips();
		renderCounts();
	}

	// ---- skeleton ----
	function h(tag, cls, text) {
		var n = document.createElement(tag);
		if (cls) n.className = cls;
		if (text !== undefined && text !== null) n.textContent = text;
		return n;
	}
	var FUNNEL = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M1 2h14l-5.5 6.5V14l-3 -1.5V8.5z"/></svg>';
	var SEARCH = '<svg id="search-icon" width="16px" height="16px" aria-hidden="true"><path d="M6.5,2A4.505,4.505,0,0,0,2,6.5a.5.5,0,0,0,1,0A3.5,3.5,0,0,1,6.5,3a.5.5,0,0,0,0-1Z"/><path d="M14.854,14.146l-3.423-3.422a6.518,6.518,0,1,0-.707.707l3.422,3.423a.5.5,0,0,0,.708-.708ZM1,6.5A5.5,5.5,0,1,1,6.5,12,5.507,5.507,0,0,1,1,6.5Z"/></svg>';

	function init(root) {
		el.root = root;
		root.textContent = '';

		var bar = h('div', 'pt-toolbar');
		var sw = h('div', 'search');
		el.search = h('input', 'searchTerm');
		el.search.type = 'text';
		el.search.placeholder = T('pt_search', 'Search brand or model');
		el.search.setAttribute('aria-label', T('pt_search', 'Search brand or model'));
		el.search.addEventListener('input', function () { state.search = el.search.value; recompute(false); });
		sw.appendChild(el.search);
		sw.insertAdjacentHTML('beforeend', SEARCH);
		bar.appendChild(sw);
		el.count = h('span', 'pt-count');
		el.count.setAttribute('role', 'status');
		el.count.setAttribute('aria-live', 'polite');
		bar.appendChild(el.count);
		bar.appendChild(h('div', 'pt-spacer'));
		el.scope = h('span', 'pt-scope');
		bar.appendChild(el.scope);
		el.all = h('div', 'SmallBtn_Green', T('t11', 'All'));
		el.none = h('div', 'SmallBtn', T('t12', 'Clear all'));
		[el.all, el.none].forEach(function (b, i) {
			b.setAttribute('role', 'button');
			b.tabIndex = 0;
			var act = function () { setShown(i === 0); };
			b.addEventListener('click', act);
			b.addEventListener('keydown', function (e) { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); act(); } });
		});
		bar.appendChild(el.all);
		bar.appendChild(el.none);
		root.appendChild(bar);

		el.chips = h('div', 'pt-chips');
		root.appendChild(el.chips);

		var grid = h('div', 'pt-grid');
		grid.setAttribute('role', 'grid');
		grid.setAttribute('aria-label', T('t10', 'Printer Selection'));
		grid.style.setProperty('--pt-cols', COLS.map(function (c) { return c.w; }).join(' '));
		el.grid = grid;
		el.head = h('div', 'pt-row pt-head');
		el.head.setAttribute('role', 'row');
		COLS.forEach(function (c) {
			var th = h('div', 'pt-th' + (c.cls === 'num' ? ' num' : ''));
			th.setAttribute('role', 'columnheader');
			th.dataset.key = c.key;
			if (c.kind !== 'none') {
				var b = h('button', 'pt-sort');
				b.type = 'button';
				var lbl = h('span', 'lbl', T(c.tid, c.label));
				var arrow = h('span', 'arrow');
				arrow.setAttribute('aria-hidden', 'true');
				if (c.cls === 'num') { b.appendChild(arrow); b.appendChild(lbl); } else { b.appendChild(lbl); b.appendChild(arrow); }
				b.title = (c.tip ? T(c.tipTid, c.tip) + '. ' : '') + T('pt_sort_tip', 'Click to sort');
				b.addEventListener('click', function () { cycleSort(c.key); });
				th.appendChild(b);
				c.arrow = arrow;
				if (c.filter) {
					var fb = h('button', 'pt-filter-btn');
					fb.type = 'button';
					fb.innerHTML = FUNNEL;
					fb.title = T('pt_filter_tip', 'Filter {col}', { col: T(c.tid, c.label) });
					fb.setAttribute('aria-label', fb.title);
					fb.setAttribute('aria-haspopup', 'dialog');
					fb.addEventListener('click', function (e) { e.stopPropagation(); togglePopover(c, th); });
					th.appendChild(fb);
					c.fbtn = fb;
				}
			}
			c.th = th;
			el.head.appendChild(th);
		});
		grid.appendChild(el.head);

		el.body = h('div', 'pt-body ZScrol');
		el.body.setAttribute('role', 'rowgroup');
		el.spacer = h('div', 'pt-spacer-y');
		el.body.appendChild(el.spacer);
		el.body.addEventListener('scroll', schedule);
		el.body.addEventListener('click', onBodyClick);
		el.body.addEventListener('change', onBodyChange);
		el.body.addEventListener('keydown', onBodyKey);
		grid.appendChild(el.body);
		el.empty = h('div', 'pt-empty', T('pt_none_match', 'No printer matches the search and filters.'));
		el.empty.style.display = 'none';
		grid.appendChild(el.empty);
		root.appendChild(grid);

		window.addEventListener('resize', function () { closePopover(); schedule(); });
		document.addEventListener('mousedown', function (e) {
			if (pop && !pop.node.contains(e.target) && !(pop.col.fbtn && pop.col.fbtn.contains(e.target))) closePopover();
		}, true);
		recompute(false);
	}

	// ---- data ----
	function load(resp) {
		var list = (resp && resp.model) || [];
		var pri = [], rest = [];
		for (var i = 0; i < list.length; i++) {
			var m = list[i];
			if (!m || typeof m.model !== 'string' || typeof m.vendor !== 'string') continue;
			(VENDOR_PRIORITY.indexOf(m.vendor) >= 0 ? pri : rest).push(m);
		}
		pri.sort(function (a, b) { return VENDOR_PRIORITY.indexOf(a.vendor) - VENDOR_PRIORITY.indexOf(b.vendor); });
		var all = pri.concat(rest);
		rows = [];
		byKey = {};
		for (var n = 0; n < all.length; n++) {
			var o = all[n];
			var brand = BRAND_NAMES.hasOwnProperty(o.vendor) ? BRAND_NAMES[o.vendor] : o.vendor;
			var name = (typeof o.name === 'string' && o.name) ? o.name : o.model;
			var th = num(o.extruders);
			var r = {
				id: n, order: n, vendor: o.vendor, model: o.model, name: name, brand: brand,
				cover: typeof o.cover === 'string' ? o.cover : '',
				nozzles: typeof o.nozzle_diameter === 'string' ? o.nozzle_diameter : '',
				x: num(o.size_x), y: num(o.size_y), z: num(o.size_z), th: (th !== null && th >= 1) ? Math.round(th) : null,
				checked: typeof o.nozzle_selected === 'string' && o.nozzle_selected !== ''
			};
			r.hay = (brand + '\u0000' + o.vendor + '\u0000' + name).toLowerCase();
			rows.push(r);
			var key = o.vendor + '\n' + o.model;
			(byKey[key] = byKey[key] || []).push(r);
		}
		clearRendered();
		recompute(false);
	}

	// What the page sends with "save_userguide_models": one entry per enabled model, keyed by
	// vendor + model, with all its nozzle variants (unchanged from the tile page, #345).
	function selection() {
		var data = {}, count = 0;
		for (var i = 0; i < rows.length; i++) {
			var r = rows[i];
			if (!r.checked) continue;
			var key = r.vendor + '\n' + r.model;
			if (data.hasOwnProperty(key)) continue;
			data[key] = { model: r.model, nozzle_diameter: r.nozzles, vendor: r.vendor };
			count++;
		}
		return { count: count, data: data };
	}

	function setChecked(r, on) {
		var same = byKey[r.vendor + '\n' + r.model] || [r];
		for (var i = 0; i < same.length; i++) {
			same[i].checked = on;
			var node = rendered[same[i].id];
			if (node) paintChecked(node, same[i]);
		}
	}
	// All / Clear all: the rows shown only. The view is not re-filtered here, so an "Enabled"
	// filter keeps the rows on screen until the filters change.
	function setShown(on) {
		for (var i = 0; i < view.length; i++) setChecked(view[i], on);
		renderCounts();
		renderChips();
	}

	// ---- body ----
	function clearRendered() {
		for (var id in rendered) if (rendered.hasOwnProperty(id)) rendered[id].remove();
		rendered = {};
	}
	function schedule() {
		if (rafPending) return;
		rafPending = true;
		(window.requestAnimationFrame || setTimeout)(function () { rafPending = false; renderRows(false); });
	}
	function paintChecked(node, r) {
		var cb = node.firstChild.firstChild;
		cb.checked = r.checked;
		node.classList.toggle('on', r.checked);
		node.setAttribute('aria-selected', r.checked ? 'true' : 'false');
	}
	function makeRow(r) {
		var row = h('div', 'pt-row');
		row.setAttribute('role', 'row');
		row.dataset.id = r.id;
		var c0 = h('div', 'pt-td check');
		c0.setAttribute('role', 'gridcell');
		var cb = h('input');
		cb.type = 'checkbox';
		cb.setAttribute('aria-label', T('pt_enable_row', 'Enable {name}', { name: r.name }));
		c0.appendChild(cb);
		row.appendChild(c0);
		var c1 = h('div', 'pt-td icon');
		c1.setAttribute('role', 'gridcell');
		if (r.cover) {
			var img = h('img');
			img.alt = '';
			img.decoding = 'async';
			img.draggable = false;
			img.onerror = function () { this.onerror = null; this.replaceWith(h('div', 'noimg')); };
			img.src = r.cover;
			c1.appendChild(img);
		} else c1.appendChild(h('div', 'noimg'));
		row.appendChild(c1);
		var cells = [
			[r.brand, ''], [r.name, ''], [fmt(r.x), 'num'], [fmt(r.y), 'num'], [fmt(r.z), 'num'],
			[r.th === null ? '—' : String(r.th), 'num']
		];
		var vals = [r.brand, r.name, r.x, r.y, r.z, r.th];
		for (var i = 0; i < cells.length; i++) {
			var td = h('div', 'pt-td' + (cells[i][1] ? ' ' + cells[i][1] : '') + (vals[i] === null ? ' na' : ''), cells[i][0]);
			td.setAttribute('role', 'gridcell');
			if (i < 2) td.title = cells[i][0];
			row.appendChild(td);
		}
		paintChecked(row, r);
		return row;
	}
	function renderRows(all) {
		if (!el.body) return;
		var top = el.body.scrollTop, hgt = el.body.clientHeight || 400;
		var first = Math.max(0, Math.floor(top / rowH) - OVERSCAN);
		var last = Math.min(view.length - 1, Math.ceil((top + hgt) / rowH) + OVERSCAN);
		var want = {};
		for (var p = first; p <= last; p++) want[view[p].id] = p;
		for (var id in rendered) {
			if (rendered.hasOwnProperty(id) && !want.hasOwnProperty(id)) { rendered[id].remove(); delete rendered[id]; }
		}
		for (var q = first; q <= last; q++) {
			var r = view[q];
			var node = rendered[r.id];
			if (!node) { node = makeRow(r); rendered[r.id] = node; el.body.appendChild(node); }
			else if (all) paintChecked(node, r);
			node.style.transform = 'translateY(' + (q * rowH) + 'px)';
			node.dataset.pos = q;
			node.setAttribute('aria-rowindex', q + 2);
		}
		// Keep the header columns over the body columns when the body shows a scrollbar.
		el.head.style.paddingRight = Math.max(0, el.body.offsetWidth - el.body.clientWidth) + 'px';
	}
	function rowOf(target) {
		var n = target;
		while (n && n !== el.body) { if (n.classList && n.classList.contains('pt-row')) return n; n = n.parentNode; }
		return null;
	}
	function onBodyClick(e) {
		if (e.target.tagName === 'INPUT') return; // the checkbox's own change event handles it
		var node = rowOf(e.target);
		if (!node) return;
		var r = rows[+node.dataset.id];
		setChecked(r, !r.checked);
		renderCounts();
		renderChips();
	}
	function onBodyChange(e) {
		var node = rowOf(e.target);
		if (!node) return;
		setChecked(rows[+node.dataset.id], e.target.checked);
		renderCounts();
		renderChips();
	}
	function onBodyKey(e) {
		if (e.key !== 'ArrowDown' && e.key !== 'ArrowUp' && e.key !== 'Home' && e.key !== 'End') return;
		var node = rowOf(e.target);
		if (!node) return;
		e.preventDefault();
		var pos = +node.dataset.pos;
		pos = e.key === 'ArrowDown' ? pos + 1 : e.key === 'ArrowUp' ? pos - 1 : e.key === 'Home' ? 0 : view.length - 1;
		focusPos(Math.max(0, Math.min(view.length - 1, pos)));
	}
	function focusPos(pos) {
		var b = el.body, y = pos * rowH;
		if (y < b.scrollTop) b.scrollTop = y;
		else if (y + rowH > b.scrollTop + b.clientHeight) b.scrollTop = y + rowH - b.clientHeight;
		renderRows(false);
		var node = rendered[view[pos].id];
		if (node) node.firstChild.firstChild.focus();
	}

	// ---- header, chips, counts ----
	function cycleSort(key) {
		var s = state.sort;
		if (s.key !== key) { s.key = key; s.dir = 1; }
		else if (s.dir === 1) s.dir = -1;
		else { s.key = null; s.dir = 1; }
		recompute(false);
	}
	function renderHead() {
		COLS.forEach(function (c) {
			if (!c.th || c.kind === 'none') return;
			var on = state.sort.key === c.key;
			c.arrow.textContent = on ? (state.sort.dir === 1 ? '▲' : '▼') : '';
			c.th.setAttribute('aria-sort', on ? (state.sort.dir === 1 ? 'ascending' : 'descending') : 'none');
			if (c.fbtn) c.fbtn.classList.toggle('on', filterActive(c.key));
		});
	}
	function universe(key) {
		var seen = {}, out = [];
		for (var i = 0; i < rows.length; i++) {
			var k = setKey(key, rows[i]);
			if (!seen.hasOwnProperty(k)) { seen[k] = 1; out.push(k); }
		}
		return out;
	}
	function chipText(c) {
		var f = state.f, name = T(c.tid, c.label);
		if (c.filter === 'set') {
			var inc = universe(c.key).filter(function (k) { return !f[c.key][k]; });
			inc.sort(function (a, b) { return c.kind === 'num' ? (a === '' ? 1 : b === '' ? -1 : a - b) : cmpText(setLabel(c.key, a), setLabel(c.key, b)); });
			var shown = inc.length <= 3 ? inc.map(function (k) { return setLabel(c.key, k); }).join(', ')
				: T('pt_n_of_m', '{n} of {m}', { n: inc.length, m: universe(c.key).length });
			return name + ': ' + (inc.length ? shown : T('pt_nothing', 'none'));
		}
		if (c.filter === 'text') return name + ': “' + f.name + '”';
		var lim = f[c.key];
		if (lim.min !== null && lim.max !== null) return name + ': ' + fmt(lim.min) + '–' + fmt(lim.max);
		if (lim.min !== null) return name + ' ≥ ' + fmt(lim.min);
		return name + ' ≤ ' + fmt(lim.max);
	}
	function clearFilter(key) {
		var f = state.f;
		if (key === 'checked' || key === 'brand' || key === 'th') f[key] = {};
		else if (key === 'name') f.name = '';
		else f[key] = { min: null, max: null };
	}
	function renderChips() {
		el.chips.textContent = '';
		COLS.forEach(function (c) {
			if (!filterActive(c.key)) return;
			var chip = h('span', 'pt-chip');
			var t = chipText(c);
			var s = h('span', null, t);
			s.title = t;
			chip.appendChild(s);
			var x = h('button', null, '✕');
			x.type = 'button';
			x.title = T('pt_remove_filter', 'Remove this filter');
			x.setAttribute('aria-label', x.title + ': ' + t);
			x.addEventListener('click', function () { clearFilter(c.key); closePopover(); recompute(false); });
			chip.appendChild(x);
			el.chips.appendChild(chip);
		});
		if (anyFilter()) {
			var clr = h('button', 'pt-link', T('pt_clear_filters', 'Clear search and filters'));
			clr.type = 'button';
			clr.addEventListener('click', function () {
				COLS.forEach(function (c) { clearFilter(c.key); });
				state.search = '';
				el.search.value = '';
				closePopover();
				recompute(false);
			});
			el.chips.appendChild(clr);
		}
	}
	function renderCounts() {
		var on = 0;
		for (var i = 0; i < rows.length; i++) if (rows[i].checked) on++;
		var shownOn = 0;
		for (var j = 0; j < view.length; j++) if (view[j].checked) shownOn++;
		el.count.textContent = view.length === rows.length
			? T('pt_count_all', '{n} printers · {on} enabled', { n: rows.length, on: on })
			: T('pt_count', '{shown} of {n} shown · {on} enabled', { shown: view.length, n: rows.length, on: on });
		var scope = view.length === rows.length
			? T('pt_scope_all', 'All / Clear all: every printer')
			: T('pt_scope', 'All / Clear all: the {shown} shown', { shown: view.length });
		el.scope.textContent = scope;
		var tip = T('pt_scope_tip', 'Applies to the {shown} printers shown by the search and filters ({on} of them enabled)', { shown: view.length, on: shownOn });
		el.all.title = tip;
		el.none.title = tip;
	}

	// ---- filter popover ----
	function closePopover() {
		if (!pop) return false;
		var c = pop.col;
		pop.node.remove();
		pop = null;
		if (c.fbtn) c.fbtn.setAttribute('aria-expanded', 'false');
		return true;
	}
	function togglePopover(c, th) {
		var was = pop && pop.col === c;
		closePopover();
		if (!was) openPopover(c, th);
	}
	function openPopover(c, th) {
		var node = h('div', 'pt-pop');
		node.setAttribute('role', 'dialog');
		node.setAttribute('aria-label', T('pt_filter_tip', 'Filter {col}', { col: T(c.tid, c.label) }));
		node.appendChild(h('div', 'pt-pop-title', T('pt_filter_tip', 'Filter {col}', { col: T(c.tid, c.label) })));
		var focusEl = null;
		if (c.filter === 'set') focusEl = buildSetFilter(c, node);
		else if (c.filter === 'text') focusEl = buildTextFilter(node);
		else focusEl = buildRangeFilter(c, node);
		node.addEventListener('keydown', function (e) {
			if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); closePopover(); if (c.fbtn) c.fbtn.focus(); }
		});
		document.body.appendChild(node);
		pop = { node: node, col: c };
		if (c.fbtn) c.fbtn.setAttribute('aria-expanded', 'true');
		var rc = th.getBoundingClientRect(), w = node.offsetWidth;
		var left = Math.max(8, Math.min(rc.left, window.innerWidth - w - 8));
		node.style.left = left + 'px';
		node.style.top = (rc.bottom + 2) + 'px';
		node.style.maxHeight = Math.max(160, window.innerHeight - rc.bottom - 12) + 'px';
		node.style.overflowY = 'auto';
		if (focusEl) focusEl.focus();
	}
	function buildSetFilter(c, node) {
		var f = state.f[c.key];
		var counts = {};
		for (var i = 0; i < rows.length; i++) {
			if (!passes(rows[i], c.key)) continue;
			var k = setKey(c.key, rows[i]);
			counts[k] = (counts[k] || 0) + 1;
		}
		var keys = universe(c.key).filter(function (k) { return counts[k] || f[k]; });
		keys.sort(function (a, b) {
			if (c.key === 'checked') return a === 'yes' ? -1 : 1;
			if (c.kind === 'num') return a === '' ? 1 : b === '' ? -1 : (+a) - (+b);
			return cmpText(setLabel(c.key, a), setLabel(c.key, b));
		});
		var q = null;
		if (c.listSearch) {
			q = h('input');
			q.type = 'text';
			q.placeholder = T('pt_find', 'Find…');
			q.setAttribute('aria-label', T('pt_find', 'Find…'));
			node.appendChild(q);
		}
		var list = h('div', 'pt-opts');
		var boxes = [];
		keys.forEach(function (k) {
			var lab = h('label', 'pt-opt');
			var cb = h('input');
			cb.type = 'checkbox';
			cb.checked = !f[k];
			cb.addEventListener('change', function () {
				if (cb.checked) delete f[k]; else f[k] = 1;
				recompute(false);
			});
			lab.appendChild(cb);
			lab.appendChild(h('span', null, setLabel(c.key, k)));
			lab.appendChild(h('span', 'n', String(counts[k] || 0)));
			lab.dataset.text = setLabel(c.key, k).toLowerCase();
			list.appendChild(lab);
			boxes.push({ k: k, cb: cb, lab: lab });
		});
		node.appendChild(list);
		if (q) q.addEventListener('input', function () {
			var t = q.value.toLowerCase();
			boxes.forEach(function (b) { b.lab.style.display = b.lab.dataset.text.indexOf(t) >= 0 ? '' : 'none'; });
		});
		var btns = h('div', 'pt-pop-btns');
		var visible = function () { return boxes.filter(function (b) { return b.lab.style.display !== 'none'; }); };
		var selAll = h('button', 'pt-link', T('pt_select_all', 'Select all'));
		selAll.type = 'button';
		selAll.addEventListener('click', function () { visible().forEach(function (b) { b.cb.checked = true; delete f[b.k]; }); recompute(false); });
		var selNone = h('button', 'pt-link', T('pt_select_none', 'Select none'));
		selNone.type = 'button';
		selNone.addEventListener('click', function () { visible().forEach(function (b) { b.cb.checked = false; f[b.k] = 1; }); recompute(false); });
		btns.appendChild(selAll);
		btns.appendChild(selNone);
		node.appendChild(btns);
		return q || (boxes.length ? boxes[0].cb : null);
	}
	function buildTextFilter(node) {
		var inp = h('input');
		inp.type = 'text';
		inp.value = state.f.name;
		inp.placeholder = T('pt_contains', 'Contains…');
		inp.setAttribute('aria-label', T('pt_contains', 'Contains…'));
		inp.addEventListener('input', function () { state.f.name = inp.value.trim(); recompute(false); });
		inp.addEventListener('keydown', function (e) { if (e.key === 'Enter') closePopover(); });
		node.appendChild(inp);
		return inp;
	}
	function buildRangeFilter(c, node) {
		var lim = state.f[c.key];
		var box = h('div', 'pt-range');
		var mk = function (which, label) {
			var id = 'pt-' + c.key + '-' + which;
			var l = h('label', null, label);
			l.htmlFor = id;
			var inp = h('input');
			inp.type = 'number';
			inp.id = id;
			inp.min = '0';
			inp.step = '1';
			inp.value = lim[which] === null ? '' : String(lim[which]);
			inp.placeholder = which === 'min' ? T('pt_any', 'any') : T('pt_any', 'any');
			inp.addEventListener('input', function () {
				var v = inp.value.trim() === '' ? null : parseFloat(inp.value);
				lim[which] = (v === null || isNaN(v)) ? null : v;
				recompute(false);
			});
			inp.addEventListener('keydown', function (e) { if (e.key === 'Enter') closePopover(); });
			box.appendChild(l);
			box.appendChild(inp);
			return inp;
		};
		var first = mk('min', T('pt_min', 'At least'));
		mk('max', T('pt_max', 'At most'));
		node.appendChild(box);
		node.appendChild(h('div', 'pt-hint', T('pt_range_hint', 'In mm. Printers without this value are hidden while a limit is set.')));
		return first;
	}

	return {
		init: init,
		load: load,
		selection: selection,
		closePopover: closePopover,
		// for tests
		_state: function () { return { rows: rows, view: view, state: state }; }
	};
})();
