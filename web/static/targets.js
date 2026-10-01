// RTS2 target list - vanilla JS, no build step, same conventions as
// target.js. Level zero of the program work (see ../programs.md): every
// target of both telescopes in one sortable, filterable table, so the
// relative priorities and durations can be compared. Read-only; a row
// opens the target editor.
//
// Like target.js, the page is served by one telescope and reaches the
// other through its proxy prefix. Site detection and the peer prefix use
// the same localStorage keys as target.js, so a prefix set there applies
// here too.

const SITES = {
	d50: { label: 'D50', peerPathDefault: '/sbt' },
	sbt: { label: 'SBT', peerPathDefault: '/d50' },
};
// Fixed column order, independent of which telescope serves the page.
const SITE_ORDER = ['d50', 'sbt'];

const SITE_KEY = 'rts2-target-site';
const PEER_PREFIX_KEY = 'rts2-target-peer-prefix';

function storageGet (key) {
	try {
		return localStorage.getItem (key);
	} catch (e) {
		return null;
	}
}

// See target.js's detectSite() for why "lascaux50" is checked explicitly.
function detectSite () {
	const stored = storageGet (SITE_KEY);
	if (stored && SITES[stored])
		return stored;
	const h = location.hostname;
	return (h.includes ('d50') || h.includes ('lascaux50')) ? 'd50' : 'sbt';
}

const currentSite = detectSite ();

function peerPrefix () {
	const stored = storageGet (PEER_PREFIX_KEY);
	if (stored !== null)
		return stored.trim ().replace (/\/$/, '');
	return SITES[currentSite].peerPathDefault;
}

function apiUrl (site, path) {
	if (site === currentSite)
		return path;
	const prefix = peerPrefix ();
	return prefix ? `${prefix}/${path}` : path;
}

async function fetchJson (url) {
	const res = await fetch (url);
	const body = await res.json ();
	return { ok: res.ok, body };
}

function setStatus (el, ok, text) {
	el.className = 'status-line ' + (ok ? 'ok' : 'err');
	el.textContent = text;
}

// --- Coordinates (same parsing/display as target.js) -------------------------

function decToSexagesimal (deg, isRa, secDecimals) {
	if (deg === null || deg === undefined || typeof deg !== 'number' || isNaN (deg))
		return '';
	const sign = deg < 0 ? '-' : (isRa ? '' : '+');
	let a = Math.abs (deg);
	if (isRa)
		a /= 15;
	let h = Math.floor (a);
	let mFull = (a - h) * 60;
	let m = Math.floor (mFull);
	let s = (mFull - m) * 60;
	const factor = Math.pow (10, secDecimals);
	s = Math.round (s * factor) / factor;
	if (s >= 60) { s -= 60; m += 1; }
	if (m >= 60) { m -= 60; h += 1; }
	const pad2 = (n) => String (n).padStart (2, '0');
	const sWidth = secDecimals > 0 ? secDecimals + 3 : 2;
	return `${sign}${pad2 (h)}:${pad2 (m)}:${s.toFixed (secDecimals).padStart (sWidth, '0')}`;
}

function parseCoordinate (str, isRa) {
	str = (str || '').trim ();
	if (str === '')
		return null;
	// Seconds are optional here (unlike the editor) - "-12:06" is a
	// normal thing to type into a search.
	const sex = str.match (/^([+-]?)\s*(\d+)[:\s]+([\d.,]+)(?:[:\s]+([\d.,]+))?$/);
	if (sex) {
		const sign = sex[1] === '-' ? -1 : 1;
		const val = parseFloat (sex[2]) + parseFloat (sex[3].replace (',', '.')) / 60 + (sex[4] ? parseFloat (sex[4].replace (',', '.')) / 3600 : 0);
		if (isNaN (val))
			return null;
		return sign * (isRa ? val * 15 : val);
	}
	const val = parseFloat (str.replace (',', '.'));
	return isNaN (val) ? null : val;
}

function separationArcmin (ra1, dec1, ra2, dec2) {
	const r = Math.PI / 180;
	const s = Math.sin ((dec2 - dec1) * r / 2) ** 2
		+ Math.cos (dec1 * r) * Math.cos (dec2 * r) * Math.sin ((ra2 - ra1) * r / 2) ** 2;
	return 2 * Math.asin (Math.min (1, Math.sqrt (s))) / r * 60;
}

// --- sinfo (mirrors sch/database.py's parse_sinfo) ----------------------------

function parseSinfo (s) {
	const out = {};
	(s || '').split (/\s+/).filter (Boolean).forEach ((tok) => {
		const i = tok.indexOf ('=');
		if (i > 0)
			out[tok.slice (0, i)] = tok.slice (i + 1);
	});
	return out;
}

// --- Data --------------------------------------------------------------------

// rows: [{ id, name, type, comment, ra, dec, reqType, d50, sbt, dist }],
// where d50/sbt is null when the target doesn't exist there, else
// { type, enabled, sched, priority, duration, obsCount, lastObs }.
let rows = [];
let siteErrors = {};
let siteCounts = {};

// Mirrors the two target queries in sch/database.py: enabled type 'O'
// with tar_id 1000-49999, and enabled type 'G' never observed.
function isScheduled (id, rec) {
	if (!rec || !rec.enabled)
		return false;
	if (rec.type === 'O')
		return id >= 1000 && id <= 49999;
	if (rec.type === 'G')
		return rec.obsCount === 0;
	return false;
}

async function loadSite (site) {
	const r = await fetchJson (apiUrl (site, 'api/db/target-list'));
	if (!r.ok)
		throw new Error (r.body.error || 'error');
	return r.body;
}

async function loadAll () {
	setStatus (statusEl, true, 'loading…');
	const results = await Promise.allSettled (SITE_ORDER.map (loadSite));
	const byId = new Map ();
	siteErrors = {};
	siteCounts = {};
	SITE_ORDER.forEach ((site, i) => {
		const res = results[i];
		if (res.status !== 'fulfilled') {
			siteErrors[site] = res.reason && res.reason.message ? res.reason.message : String (res.reason);
			return;
		}
		siteCounts[site] = res.value.length;
		for (const t of res.value) {
			let row = byId.get (t.id);
			if (!row) {
				row = { id: t.id, name: t.name, type: t.type, comment: t.comment, ra: t.ra, dec: t.dec, reqTypes: {}, d50: null, sbt: null };
				byId.set (t.id, row);
			} else if (site === currentSite) {
				// The telescope serving the page wins for the shared fields.
				Object.assign (row, { name: t.name, type: t.type, comment: t.comment, ra: t.ra, dec: t.dec });
			}
			const sinfo = parseSinfo (t.sinfo);
			const dur = parseInt (sinfo.duration, 10);
			row[site] = {
				type: t.type,
				enabled: t.enabled,
				priority: t.priority,
				duration: isNaN (dur) ? null : dur,
				obsCount: t.obsCount,
				lastObs: t.lastObs,
			};
			if (sinfo.type)
				row.reqTypes[site] = sinfo.type;
		}
	});
	rows = Array.from (byId.values ());
	for (const row of rows) {
		for (const site of SITE_ORDER)
			if (row[site])
				row[site].sched = isScheduled (row.id, row[site]);
		const types = Array.from (new Set (Object.values (row.reqTypes)));
		row.reqType = types.join ('/');
	}
	fillTypeSelect ();

	const parts = SITE_ORDER.map ((s) => siteErrors[s]
		? `${SITES[s]['label']}: ${siteErrors[s]}`
		: `${SITES[s]['label']}: ${siteCounts[s]} targets`);
	setStatus (statusEl, Object.keys (siteErrors).length === 0, parts.join (', '));
	render ();
}

// --- Filters (kept in the address bar, so a filtered view can be linked) -----

const statusEl = document.getElementById ('list-status');
const summaryEl = document.getElementById ('list-summary');
const theadEl = document.querySelector ('#target-table thead');
const tbodyEl = document.querySelector ('#target-table tbody');
const showAllBtn = document.getElementById ('show-all-btn');

const F = {
	q: document.getElementById ('q'),
	nearRa: document.getElementById ('near-ra'),
	nearDec: document.getElementById ('near-dec'),
	nearR: document.getElementById ('near-r'),
	enabled: document.getElementById ('enabled'),
	type: document.getElementById ('type'),
	eligible: document.getElementById ('eligible'),
};

const RENDER_LIMIT = 500;
let showAll = false;
let sortKey = 'id';
let sortDir = 1;

function fillTypeSelect () {
	const want = F.type.value || new URLSearchParams (location.search).get ('type') || '';
	const types = Array.from (new Set (rows.map ((r) => r.type))).sort ();
	F.type.innerHTML = '<option value="">all</option>';
	for (const t of types) {
		const n = rows.filter ((r) => r.type === t).length;
		const o = document.createElement ('option');
		o.value = t;
		o.textContent = `${t} (${n})`;
		F.type.appendChild (o);
	}
	F.type.value = types.includes (want) ? want : '';
}

function readUrlFilters () {
	const p = new URLSearchParams (location.search);
	F.q.value = p.get ('q') || '';
	F.nearRa.value = p.get ('ra') || '';
	F.nearDec.value = p.get ('dec') || '';
	if (p.has ('r'))
		F.nearR.value = p.get ('r');
	if (p.has ('enabled') && [...F.enabled.options].some ((o) => o.value === p.get ('enabled')))
		F.enabled.value = p.get ('enabled');
	F.eligible.checked = p.get ('scheduled') === '1';
	if (p.has ('sort')) {
		const s = p.get ('sort');
		sortDir = s.startsWith ('-') ? -1 : 1;
		sortKey = s.replace (/^-/, '');
	}
}

function writeUrlFilters () {
	const p = new URLSearchParams ();
	if (F.q.value.trim ())
		p.set ('q', F.q.value.trim ());
	if (F.nearRa.value.trim () || F.nearDec.value.trim ()) {
		p.set ('ra', F.nearRa.value.trim ());
		p.set ('dec', F.nearDec.value.trim ());
		p.set ('r', F.nearR.value);
	}
	if (F.enabled.value !== 'any')
		p.set ('enabled', F.enabled.value);
	if (F.type.value)
		p.set ('type', F.type.value);
	if (F.eligible.checked)
		p.set ('scheduled', '1');
	if (sortKey !== 'id' || sortDir !== 1)
		p.set ('sort', (sortDir < 0 ? '-' : '') + sortKey);
	const qs = p.toString ();
	history.replaceState (null, '', location.pathname + (qs ? '?' + qs : ''));
}

function nearFilter () {
	const ra = parseCoordinate (F.nearRa.value, true);
	const dec = parseCoordinate (F.nearDec.value, false);
	const r = parseFloat (F.nearR.value);
	if (ra === null || dec === null)
		return null;
	return { ra, dec, r: isNaN (r) || r <= 0 ? 10 : r };
}

function textFilter () {
	const q = F.q.value.trim ().toLowerCase ();
	if (q === '')
		return null;
	let m = q.match (/^(\d+)\s*-\s*(\d+)$/);
	if (m)
		return (row) => row.id >= +m[1] && row.id <= +m[2];
	const words = q.split (/\s+/);
	const isId = /^\d+$/.test (q);
	return (row) => {
		if (isId && row.id === +q)
			return true;
		const hay = ((row.name || '') + ' ' + (row.name || '').replace (/\s+/g, '') + ' ' + (row.comment || '')).toLowerCase ();
		return words.every ((w) => hay.includes (w));
	};
}

function enabledFilter () {
	const on = (row, s) => !!(row[s] && row[s].enabled);
	switch (F.enabled.value) {
		case 'd50': return (row) => on (row, 'd50');
		case 'sbt': return (row) => on (row, 'sbt');
		case 'both': return (row) => on (row, 'd50') && on (row, 'sbt');
		case 'none': return (row) => !on (row, 'd50') && !on (row, 'sbt');
		case 'all': return null;
		default: return (row) => on (row, 'd50') || on (row, 'sbt');
	}
}

function filteredRows () {
	const tests = [];
	const text = textFilter ();
	if (text)
		tests.push (text);
	const en = enabledFilter ();
	if (en)
		tests.push (en);
	if (F.type.value)
		tests.push ((row) => row.type === F.type.value);
	if (F.eligible.checked)
		tests.push ((row) => SITE_ORDER.some ((s) => row[s] && row[s].sched));

	const near = nearFilter ();
	for (const row of rows)
		row.dist = (near && row.ra !== null && row.dec !== null)
			? separationArcmin (near.ra, near.dec, row.ra, row.dec) : null;
	if (near)
		tests.push ((row) => row.dist !== null && row.dist <= near.r);

	return rows.filter ((row) => tests.every ((t) => t (row)));
}

// --- Table -------------------------------------------------------------------

// Per-site columns; `get` reads from the site record (null = no target there).
const SITE_COLS = [
	{ key: 'on', title: 'On', get: (r) => r ? (r.enabled ? (r.sched ? 2 : 1) : 0) : null },
	{ key: 'prio', title: 'Prio', get: (r) => r ? r.priority : null },
	{ key: 'dur', title: 'Dur', get: (r) => r ? r.duration : null },
	{ key: 'obs', title: 'Obs', get: (r) => r ? r.obsCount : null },
	{ key: 'last', title: 'Last obs', get: (r) => r ? r.lastObs : null },
];

function sortValue (row, key) {
	const dot = key.indexOf ('.');
	if (dot > 0) {
		const site = key.slice (0, dot);
		const col = SITE_COLS.find ((c) => c.key === key.slice (dot + 1));
		return col && SITES[site] ? col.get (row[site]) : null;
	}
	const v = row[key];
	return (typeof v === 'string') ? v.toLowerCase () : v;
}

function compareRows (a, b) {
	const va = sortValue (a, sortKey);
	const vb = sortValue (b, sortKey);
	// Missing values always sort last, whichever the direction.
	const na = va === null || va === undefined || va === '';
	const nb = vb === null || vb === undefined || vb === '';
	if (na || nb)
		return na && nb ? a.id - b.id : (na ? 1 : -1);
	if (va < vb)
		return -sortDir;
	if (va > vb)
		return sortDir;
	return a.id - b.id;
}

function formatDuration (s) {
	if (s === null || s === undefined)
		return '';
	return s >= 3600 && s % 60 === 0 ? `${Math.floor (s / 3600)}h${String ((s % 3600) / 60).padStart (2, '0')}` : `${s}s`;
}

function formatDate (t) {
	if (t === null || t === undefined)
		return '';
	return new Date (t * 1000).toISOString ().slice (0, 10);
}

function th (text, key, extra) {
	const el = document.createElement ('th');
	el.textContent = text;
	if (extra)
		Object.assign (el, extra);
	if (key) {
		el.dataset.sort = key;
		el.className = 'sortable' + (sortKey === key ? (sortDir > 0 ? ' sorted-asc' : ' sorted-desc') : '');
	}
	return el;
}

function renderHead (withDist) {
	theadEl.innerHTML = '';
	const tr1 = document.createElement ('tr');
	const tr2 = document.createElement ('tr');
	const base = [['ID', 'id'], ['Name', 'name'], ['Type', 'type'], ['RA', 'ra'], ['Dec', 'dec']];
	if (withDist)
		base.push (['Dist (′)', 'dist']);
	for (const [t, k] of base)
		tr1.appendChild (th (t, k, { rowSpan: 2 }));
	for (const site of SITE_ORDER) {
		tr1.appendChild (th (SITES[site].label, null, { colSpan: SITE_COLS.length, className: 'site-group' }));
		SITE_COLS.forEach ((c, i) => {
			const el = th (c.title, `${site}.${c.key}`);
			if (i === 0)
				el.classList.add ('site-first');
			tr2.appendChild (el);
		});
	}
	const rt = th ('Req. type', 'reqType', { rowSpan: 2 });
	rt.classList.add ('site-first');
	tr1.appendChild (rt);
	theadEl.append (tr1, tr2);
}

function td (text, className, title) {
	const el = document.createElement ('td');
	el.textContent = text;
	if (className)
		el.className = className;
	if (title)
		el.title = title;
	return el;
}

function renderRow (row, withDist) {
	const tr = document.createElement ('tr');
	tr.dataset.id = row.id;
	const idCell = document.createElement ('td');
	const a = document.createElement ('a');
	a.href = `target.html?id=${row.id}`;
	a.textContent = row.id;
	idCell.appendChild (a);
	idCell.className = 'num';
	tr.appendChild (idCell);
	tr.appendChild (td (row.name || '', 'name', row.comment || ''));
	tr.appendChild (td (row.type, 'center'));
	tr.appendChild (td (decToSexagesimal (row.ra, true, 1), 'mono'));
	tr.appendChild (td (decToSexagesimal (row.dec, false, 0), 'mono'));
	if (withDist)
		tr.appendChild (td (row.dist === null ? '' : row.dist.toFixed (1), 'num'));
	for (const site of SITE_ORDER) {
		const r = row[site];
		if (!r) {
			const el = td ((siteErrors[site] ? SITES[site].label + ' unavailable' : 'not on ' + SITES[site].label), 'absent site-first');
			el.colSpan = SITE_COLS.length;
			tr.appendChild (el);
			continue;
		}
		const on = r.enabled ? (r.sched ? '●' : '○') : '';
		tr.appendChild (td (on, 'center site-first ' + (r.sched ? 'on-sched' : 'on-idle'),
			r.enabled ? (r.sched ? 'enabled, scheduled' : 'enabled, but not a type/ID the scheduler takes') : 'disabled'));
		tr.appendChild (td (r.priority === null ? '' : r.priority, 'num' + (r.enabled ? '' : ' dim')));
		tr.appendChild (td (formatDuration (r.duration), 'num' + (r.enabled ? '' : ' dim'), r.duration === null ? '' : `${r.duration} s`));
		tr.appendChild (td (r.obsCount || '', 'num dim'));
		tr.appendChild (td (formatDate (r.lastObs), 'mono dim'));
	}
	const types = Object.values (row.reqTypes);
	tr.appendChild (td (row.reqType, 'site-first' + (new Set (types).size > 1 ? ' differs' : ''),
		new Set (types).size > 1 ? SITE_ORDER.filter ((s) => row.reqTypes[s]).map ((s) => `${SITES[s].label}: ${row.reqTypes[s]}`).join (', ') : ''));
	return tr;
}

function renderSummary (list) {
	const parts = [`${list.length} of ${rows.length} targets`];
	for (const site of SITE_ORDER) {
		if (siteErrors[site]) {
			parts.push (`${SITES[site].label}: unavailable`);
			continue;
		}
		const sched = list.filter ((r) => r[site] && r[site].sched);
		const secs = sched.reduce ((s, r) => s + (r[site].duration || 0), 0);
		const noDur = sched.filter ((r) => r[site].duration === null).length;
		parts.push (`${SITES[site].label}: ${sched.length} scheduled, ${(secs / 3600).toFixed (1)} h total duration`
			+ (noDur ? ` (${noDur} without duration)` : ''));
	}
	summaryEl.textContent = parts.join (' · ');
}

function render () {
	writeUrlFilters ();
	const withDist = nearFilter () !== null;
	if (sortKey === 'dist' && !withDist)
		sortKey = 'id';
	const list = filteredRows ().sort (compareRows);
	renderSummary (list);
	renderHead (withDist);
	const shown = showAll ? list : list.slice (0, RENDER_LIMIT);
	const frag = document.createDocumentFragment ();
	for (const row of shown)
		frag.appendChild (renderRow (row, withDist));
	tbodyEl.replaceChildren (frag);
	showAllBtn.hidden = shown.length === list.length;
	showAllBtn.textContent = `Show all ${list.length} (${list.length - shown.length} more)`;
}

// --- Events ------------------------------------------------------------------

theadEl.addEventListener ('click', (ev) => {
	const el = ev.target.closest ('th[data-sort]');
	if (!el)
		return;
	const key = el.dataset.sort;
	if (key === sortKey)
		sortDir = -sortDir;
	else {
		sortKey = key;
		// Priority, duration, observations and last observation are
		// mostly wanted biggest/latest first.
		sortDir = /\.(prio|dur|obs|last|on)$/.test (key) ? -1 : 1;
	}
	render ();
});

tbodyEl.addEventListener ('click', (ev) => {
	if (ev.target.closest ('a'))
		return;
	const tr = ev.target.closest ('tr[data-id]');
	if (tr)
		location.href = `target.html?id=${tr.dataset.id}`;
});

let renderTimer = null;
function scheduleRender () {
	clearTimeout (renderTimer);
	showAll = false;
	renderTimer = setTimeout (render, 150);
}

for (const el of [F.q, F.nearRa, F.nearDec, F.nearR])
	el.addEventListener ('input', scheduleRender);
for (const el of [F.enabled, F.type, F.eligible])
	el.addEventListener ('change', scheduleRender);
document.getElementById ('filter-form').addEventListener ('submit', (ev) => ev.preventDefault ());
document.getElementById ('reload-btn').addEventListener ('click', loadAll);
showAllBtn.addEventListener ('click', () => { showAll = true; render (); });

readUrlFilters ();
loadAll ();
