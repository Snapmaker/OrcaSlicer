// Printer cards: one renderer for the printer rows that RemoteAccess::api_printers builds (the same
// rows as the hub's /summary and the app's Devices page). Used by the slicer's Printers tab
// (monitor.html); meant to be shared with the hub page's Devices view (stream_center.html) so the
// two cannot drift - that page still has its own copy until the hub serves this file (phase 2).
//
// Transport-free: it draws what it is given and reports clicks through callbacks; it never fetches,
// posts or stores anything. Every printer-supplied string goes in as textContent.
//
//   PrinterCards.render(container, rows, {
//       jobs:     { <id>: { phase: 'running'|'done'|'error', text, error } },  // a command per card
//       onAction: function(row, action) {},   // 'pause' | 'resume' | 'stop' (stop is confirmed by the caller)
//       onOpen:   function(row) {}            // "Open Device page"
//   })
//   PrinterCards.sortRows(rows, 'status'|'name')  -> a sorted copy
//   PrinterCards.stateOf(row)                     -> { key, word }  key: error|paused|printing|idle|offline
//   PrinterCards.summary(rows)                    -> { printing, paused, error, idle, offline }
(function(root) {
    'use strict';

    function el(tag, cls, text) {
        var e = document.createElement(tag);
        if (cls) e.className = cls;
        if (text !== undefined && text !== null) e.textContent = String(text);
        return e;
    }
    function isNum(v) { return typeof v === 'number' && isFinite(v); }
    function str(v) { return typeof v === 'string' ? v : (v === undefined || v === null ? '' : String(v)); }
    function colour(c) { return /^#[0-9a-fA-F]{6}$/.test(str(c)) ? c : ''; }

    function fmtTime(s) {
        s = Math.max(0, Math.round(s || 0));
        var h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60);
        return h ? h + 'h ' + m + 'm' : m + 'm';
    }

    function errorText(p) {
        if (p.print_error && (p.print_error.message || p.print_error.code))
            return str(p.print_error.message) || ('Printer error ' + str(p.print_error.code));
        return '';
    }

    // The state word comes from the printer's own fields; the colour follows it.
    function stateOf(p) {
        if (!p || p.online === false) return { key: 'offline', word: 'Offline' };
        var st = str(p.print_status || p.status).toLowerCase();
        if (errorText(p) || /fail|error/.test(st)) return { key: 'error', word: 'Error' };
        if (/pause/.test(st)) return { key: 'paused', word: 'Paused' };
        if (p.printing === true || /running|printing|prepare|slicing/.test(st)) return { key: 'printing', word: 'Printing' };
        if (/finish|complete/.test(st)) return { key: 'idle', word: 'Finished' };
        return { key: 'idle', word: 'Idle' };
    }
    var RANK = { error: 0, paused: 1, printing: 2, idle: 3, offline: 4 };

    function nameOf(p) { return str(p.name) || str(p.id); }

    function sortRows(rows, by) {
        var list = (rows || []).slice();
        list.sort(function(a, b) {
            if (by !== 'name') {
                var d = RANK[stateOf(a).key] - RANK[stateOf(b).key];
                if (d) return d;
                // Among printing ones, the one that finishes first leads.
                if (stateOf(a).key === 'printing') {
                    var ta = isNum(a.left_time_s) ? a.left_time_s : 1e12, tb = isNum(b.left_time_s) ? b.left_time_s : 1e12;
                    if (ta !== tb) return ta - tb;
                }
            }
            return nameOf(a).localeCompare(nameOf(b), undefined, { numeric: true, sensitivity: 'base' });
        });
        return list;
    }

    function summary(rows) {
        var s = { printing: 0, paused: 0, error: 0, idle: 0, offline: 0 };
        (rows || []).forEach(function(p) { s[stateOf(p).key]++; });
        return s;
    }

    // Temperatures: the printer's own heater list where the row has one (bed, each nozzle, the
    // chamber), else the plain bed / nozzle fields. A heater without a number gets no chip.
    function temps(p) {
        var out = [];
        var hs = p.controls && Array.isArray(p.controls.heaters) ? p.controls.heaters : [];
        if (hs.length) {
            hs.forEach(function(h) {
                if (!h || !isNum(h.temp)) return;
                out.push({ label: str(h.label) || str(h.id), temp: h.temp, target: isNum(h.target) ? h.target : 0,
                           sensor: h.settable === false });
            });
            return out;
        }
        if (isNum(p.bed_temp)) out.push({ label: 'Bed', temp: p.bed_temp, target: isNum(p.bed_target) ? p.bed_target : 0 });
        var nz = (p.nozzles || []).filter(function(n) { return n && isNum(n.temp); });
        nz.forEach(function(n, i) { out.push({ label: nz.length > 1 ? 'Nozzle ' + (i + 1) : 'Nozzle', temp: n.temp, target: isNum(n.target) ? n.target : 0 }); });
        return out;
    }

    function fans(p) {
        var fs = p.controls && Array.isArray(p.controls.fans) ? p.controls.fans : [];
        return fs.filter(function(f) { return f && isNum(f.percent); })
                 .map(function(f) { return { label: str(f.label) || str(f.id), percent: Math.max(0, Math.min(100, Math.round(f.percent))) }; });
    }

    // Filament slots: AMS trays (grouped per unit), external spools, Snapmaker toolheads.
    function filaments(p) {
        var groups = [];
        (Array.isArray(p.ams) ? p.ams : []).forEach(function(u, ui) {
            if (!u) return;
            var name = 'AMS ' + (isNum(+u.id) && +u.id < 128 ? String.fromCharCode(65 + (+u.id)) : (ui + 1)) + (u.side ? ' (' + u.side + ')' : '');
            var slots = (Array.isArray(u.trays) ? u.trays : []).map(function(t, i) {
                return { label: name + ' slot ' + (i + 1), present: t && t.exists !== false && !!(t.type || colour(t.color)),
                         type: str(t && t.type), color: colour(t && t.color), remain: t && isNum(t.remain) && t.remain >= 0 ? t.remain : null };
            });
            var hum = isNum(u.humidity_pct) ? u.humidity_pct + '%' : '';
            if (slots.length) groups.push({ name: name, note: hum ? 'humidity ' + hum : '', slots: slots });
        });
        var ext = (Array.isArray(p.ext_spools) ? p.ext_spools : []).map(function(t, i, a) {
            return { label: 'External spool' + (t.side ? ' ' + t.side : (a.length > 1 ? ' ' + (i + 1) : '')), present: t.exists !== false && !!(t.type || colour(t.color)),
                     type: str(t.type), color: colour(t.color), remain: null };
        });
        if (ext.length) groups.push({ name: 'External', note: '', slots: ext });
        var heads = (Array.isArray(p.toolheads) ? p.toolheads : []).map(function(t, i) {
            var n = isNum(t.index) ? t.index + 1 : i + 1;
            return { label: 'Toolhead ' + n, present: !!t.loaded, type: str(t.type) + (t.sub_type && t.sub_type !== 'NONE' ? ' ' + t.sub_type : ''),
                     color: colour(t.color), remain: null };
        });
        if (heads.length) groups.push({ name: 'Toolheads', note: '', slots: heads });
        return groups;
    }

    function whyNot(p, action) {
        if (p.online === false) return 'This printer is offline.';
        if (p.kind === 'bambu' && p.connected === false) return 'The PC is not connected to this printer.';
        if (p.status_error) return 'The printer did not answer: ' + str(p.status_error);
        var st = str(p.print_status || p.status);
        if (action === 'resume') return st ? 'Only a paused print can be resumed (it reports ' + st + ').' : 'Nothing is paused.';
        if (action === 'pause') return st ? 'Only a running print can be paused (it reports ' + st + ').' : 'Nothing is printing.';
        return st ? 'Nothing to stop (it reports ' + st + ').' : 'Nothing is printing.';
    }

    function button(label, cls, key, name) {
        var b = el('button', 'pbtn' + (cls ? ' ' + cls : ''), label);
        b.type = 'button';
        b.setAttribute('data-fk', key);
        if (name) b.setAttribute('aria-label', name);
        return b;
    }
    // A button that cannot be pressed now stays focusable, so its reason (the title) can be found.
    function setEnabled(b, on, why) {
        if (on) { b.removeAttribute('aria-disabled'); b.classList.remove('off'); }
        else { b.setAttribute('aria-disabled', 'true'); b.classList.add('off'); }
        if (why) b.title = why;
    }

    function card(p, opts) {
        var st = stateOf(p), name = nameOf(p);
        var c = el('article', 'pcard st-' + st.key);
        c.setAttribute('data-printer-id', str(p.id));
        c.setAttribute('aria-label', name + ', ' + st.word);

        var head = el('div', 'phead');
        var titles = el('div', 'ptitles');
        titles.appendChild(el('h3', 'pname', name));
        var sub = [];
        if (p.model) sub.push(str(p.model));
        if (p.via === 'cloud') sub.push('cloud');
        else if (p.ip) sub.push(str(p.ip));
        if (sub.length) titles.appendChild(el('div', 'pmodel', sub.join(' · ')));
        head.appendChild(titles);
        head.appendChild(el('span', 'ppill', st.word));
        c.appendChild(head);

        // The job: name, progress, time left, layer.
        var printingish = st.key === 'printing' || st.key === 'paused' || (st.key === 'error' && p.printing);
        if (p.task || printingish) {
            var job = el('div', 'pjob');
            if (p.task) { var t = el('div', 'ptask', str(p.task)); t.title = str(p.task); job.appendChild(t); }
            if (printingish && isNum(p.percent)) {
                var pct = Math.max(0, Math.min(100, Math.round(p.percent)));
                var bar = el('div', 'pbar');
                bar.setAttribute('role', 'progressbar');
                bar.setAttribute('aria-valuemin', '0');
                bar.setAttribute('aria-valuemax', '100');
                bar.setAttribute('aria-valuenow', String(pct));
                bar.setAttribute('aria-label', 'Progress of ' + name);
                var fill = el('div', 'pfill'); fill.style.width = pct + '%'; bar.appendChild(fill);
                job.appendChild(bar);
                var parts = [pct + '%'];
                if (isNum(p.left_time_s) && p.left_time_s > 0) parts.push(fmtTime(p.left_time_s) + ' left');
                if (isNum(p.total_layers) && p.total_layers > 0) parts.push('layer ' + (isNum(p.layer) ? p.layer : 0) + '/' + p.total_layers);
                job.appendChild(el('div', 'pprog', parts.join(' · ')));
            }
            if (p.stage && str(p.stage) !== str(p.task)) job.appendChild(el('div', 'pstage', str(p.stage)));
            c.appendChild(job);
        }

        if (p.online !== false) {
            var ts = temps(p), fs = fans(p);
            if (ts.length || fs.length) {
                var chips = el('ul', 'pchips');
                chips.setAttribute('aria-label', 'Temperatures and fans');
                ts.forEach(function(h) {
                    var li = el('li', 'pchip');
                    li.appendChild(el('span', 'k', h.label));
                    li.appendChild(el('span', 'v', Math.round(h.temp) + '°' + (h.target > 0 ? ' / ' + Math.round(h.target) + '°' : '')));
                    chips.appendChild(li);
                });
                fs.forEach(function(f) {
                    var li = el('li', 'pchip fan');
                    li.appendChild(el('span', 'k', f.label + ' fan'));
                    li.appendChild(el('span', 'v', f.percent + '%'));
                    chips.appendChild(li);
                });
                c.appendChild(chips);
            }
            var fg = filaments(p);
            if (fg.length) {
                var fil = el('div', 'pfil');
                fg.forEach(function(g) {
                    var row = el('div', 'pfrow');
                    row.appendChild(el('span', 'pfname', g.name + (g.note ? ' · ' + g.note : '')));
                    var sws = el('ul', 'pswatches');
                    sws.setAttribute('aria-label', g.name);
                    g.slots.forEach(function(s) {
                        var li = el('li', 'psw' + (s.present ? '' : ' empty'));
                        var text = s.label + ': ' + (s.present ? (s.type || 'filament') + (s.color ? ' ' + s.color : '') + (s.remain !== null ? ', ' + s.remain + '% left' : '') : 'empty');
                        li.title = text;
                        li.setAttribute('aria-label', text);
                        if (s.present && s.color) li.style.background = s.color;
                        if (s.present && s.type) li.appendChild(el('span', 'pswt', s.type.split(' ')[0].slice(0, 6)));
                        if (s.remain !== null && s.remain <= 10 && s.present) li.classList.add('low');
                        sws.appendChild(li);
                    });
                    row.appendChild(sws);
                    fil.appendChild(row);
                });
                c.appendChild(fil);
            }
        } else if (isNum(p.seen_s) && p.seen_s > 0) {
            c.appendChild(el('div', 'pstage', 'Last seen ' + fmtTime(p.seen_s) + ' ago'));
        }

        var err = errorText(p);
        if (!err && p.hms && p.hms.count > 0 && p.hms.message) err = str(p.hms.message);
        if (err) c.appendChild(el('div', 'perr', err));
        if (p.login_required) c.appendChild(el('div', 'pnote', 'This printer asks for a login.'));

        // Pause / Resume / Stop, by the printer's own predicates; Open Device page.
        var jobs = (opts && opts.jobs) || {};
        var js = jobs[p.id];
        var busy = !!(js && js.phase === 'running');
        var acts = el('div', 'pacts');
        if (p.can_pause !== undefined || p.can_resume !== undefined || p.can_stop !== undefined) {
            [['pause', 'Pause', p.can_pause], ['resume', 'Resume', p.can_resume], ['stop', 'Stop', p.can_stop]].forEach(function(a) {
                var on = a[2] === true && !busy;
                var b = button(a[1], a[0] === 'stop' ? 'stop' : '', 'act:' + p.id + ':' + a[0], a[1] + ' ' + name);
                setEnabled(b, on, on ? (a[0] === 'stop' ? 'Cancel this print on the printer' : a[1] + ' this print')
                                     : (busy ? 'A command is already running' : whyNot(p, a[0])));
                b.addEventListener('click', function() {
                    if (b.getAttribute('aria-disabled') === 'true') return;
                    if (opts && opts.onAction) opts.onAction(p, a[0]);
                });
                acts.appendChild(b);
            });
        }
        var open = button('Open Device page', 'alt', 'open:' + p.id, 'Open the Device page of ' + name);
        open.addEventListener('click', function() { if (opts && opts.onOpen) opts.onOpen(p); });
        acts.appendChild(open);
        c.appendChild(acts);

        if (js) {
            var line = el('div', 'pjobline' + (js.phase === 'error' ? ' err' : js.phase === 'done' ? ' ok' : ''), js.phase === 'error' ? js.error : js.text);
            c.appendChild(line);
        }
        return c;
    }

    // Redraws the cards and keeps a keyboard user's place across the redraw.
    function render(container, rows, opts) {
        var a = document.activeElement;
        var key = a && container.contains(a) && a.getAttribute ? a.getAttribute('data-fk') : '';
        while (container.firstChild) container.removeChild(container.firstChild);
        (rows || []).forEach(function(p) { container.appendChild(card(p, opts)); });
        if (key) {
            var nodes = container.querySelectorAll('[data-fk]');
            for (var i = 0; i < nodes.length; i++)
                if (nodes[i].getAttribute('data-fk') === key) { try { nodes[i].focus({ preventScroll: true }); } catch (e) { nodes[i].focus(); } break; }
        }
    }

    root.PrinterCards = { render: render, card: card, sortRows: sortRows, stateOf: stateOf, summary: summary,
                          temps: temps, fans: fans, filaments: filaments, fmtTime: fmtTime, whyNot: whyNot };
})(typeof window !== 'undefined' ? window : this);
