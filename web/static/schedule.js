// RTS2 schedule - one night of both telescopes: what was observed, what
// is planned, and (where it is recorded) when the system, the executor or
// the queuer was not running. Vanilla JS, no build step, same as the
// other pages.
//
// Data: /api/db/schedule of each telescope (its own database - D50 and
// SBT each keep their own queues_targets and observations), reached the
// same way target.js reaches the peer (a same-origin proxy path). The
// daemon computes every alt/az; this page only draws.
//
// Views share one selection: hovering or clicking an entry in the
// timeline, the elevation plot or the list highlights the same target
// everywhere. The timeline shows both telescopes; the elevation and sky
// plots one, picked by the "plots for" switch.

const SITES = {
	sbt: { label: 'SBT', peerPathDefault: '/d50' },
	d50: { label: 'D50', peerPathDefault: '/sbt' },
};
// D50 first: the bigger telescope on top, the way the scheduler plots it
const SITE_ORDER = ['d50', 'sbt'];

const SITE_KEY = 'rts2-target-site';		// shared with target.js
const PEER_PREFIX_KEY = 'rts2-target-peer-prefix';
const PLOT_SITE_KEY = 'rts2-schedule-plot-site';

function storageGet (key) {
	try {
		return localStorage.getItem (key);
	} catch (er) {
		return null;
	}
}

function storageSet (key, value) {
	try {
		localStorage.setItem (key, value);
	} catch (er) {
		// private window: the choice is just not remembered
	}
}

// The same detection target.js uses, see its comment: lascaux50 does
// not contain "d50".
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
	if (!res.ok)
		throw new Error (body.error || `HTTP ${res.status}`);
	return body;
}

// --- System/executor/queuer state lanes ---------------------------------
//
// "What was" has to show when nothing could have been observed, not only
// what was. That comes from rts2-recordd: every entry here that the
// telescope's recordd records (see /api/db/recvals) becomes a lane, the
// ones that are not recorded are left out. `@state` is recordd's name
// for a device's state, stored as value "state".
const STATE_SOURCES = [
	{ device: 'centrald', value: 'state', label: 'system', kind: 'server' },
	{ device: 'EXEC', value: 'enabled', label: 'executor', kind: 'bool' },
	{ device: 'SEL', value: 'enabled', label: 'queuer', kind: 'bool' },
];

// a periodically sampled value with no sample for this long is "not
// recorded" there - recordd, the device or the whole machine was down
const SAMPLE_GAP = 300;

// centrald's state word, kernel/include/status.h
const SERVERD_ONOFF_MASK = 0x30;
const SERVERD_STANDBY = 0x10;

// --- Observation state bits (kernel/include/rts2target.h) ----------------
const OBS_BIT_INTERUPED = 0x10;
const OBS_BIT_ACQUSITION_FAI = 0x20;

// --- Page state ------------------------------------------------------------

const dateEl = document.getElementById ('sch-date');
const statusEl = document.getElementById ('sch-status');
const tooltipEl = document.getElementById ('sch-tooltip');
const timelineCanvas = document.getElementById ('sch-timeline');
const elevCanvas = document.getElementById ('sch-elevation');
const polarCanvas = document.getElementById ('sch-polar');
const listBody = document.querySelector ('#sch-list tbody');

// data[site] = { night (the /api/db/schedule reply), horizon, lanes,
// entries, error }
let data = {};
let plotSite = SITES[storageGet (PLOT_SITE_KEY)] ? storageGet (PLOT_SITE_KEY) : currentSite;
// { site, target } - hover wins over the clicked selection
let hover = null;
let selected = null;
let refreshTimer = null;

// hit areas of the last drawing, for the mouse: [{x0, x1, y0, y1, entry}]
let timelineHits = [];
let elevHits = [];

function setStatus (ok, text) {
	statusEl.className = 'status-line ' + (ok ? 'ok' : 'err');
	statusEl.textContent = text;
}

function escapeHtml (s) {
	return String (s).replace (/&/g, '&amp;').replace (/</g, '&lt;').replace (/>/g, '&gt;');
}

function pad2 (n) {
	return String (n).padStart (2, '0');
}

function hhmm (t) {
	const d = new Date (t * 1000);
	return pad2 (d.getHours ()) + ':' + pad2 (d.getMinutes ());
}

function dateString (t) {
	const d = new Date (t * 1000);
	return d.getFullYear () + '-' + pad2 (d.getMonth () + 1) + '-' + pad2 (d.getDate ());
}

function css (name, fallback) {
	const v = getComputedStyle (document.documentElement).getPropertyValue (name).trim ();
	return v || fallback;
}

// A colour per target, stable across views and reloads: the golden-angle
// walk spreads consecutive ids apart.
function targetColor (id, alpha) {
	const hue = (id * 137.508) % 360;
	return `hsla(${hue.toFixed (0)}, 65%, 45%, ${alpha === undefined ? 1 : alpha})`;
}

function compassAz (azLibnova) {
	return (azLibnova + 180) % 360;
}

/** Canvas sized to its CSS box at the device's pixel ratio. */
function setupCanvas (canvas, cssHeight) {
	const dpr = window.devicePixelRatio || 1;
	const w = canvas.clientWidth;
	canvas.style.height = cssHeight + 'px';
	canvas.width = Math.round (w * dpr);
	canvas.height = Math.round (cssHeight * dpr);
	const ctx = canvas.getContext ('2d');
	ctx.setTransform (dpr, 0, 0, dpr, 0, 0);
	ctx.clearRect (0, 0, w, cssHeight);
	return { ctx, w, h: cssHeight };
}

// --- Loading -----------------------------------------------------------------

function isHighlighted (site, target) {
	const h = hover || selected;
	return h && h.site === site && h.target === target;
}

function anyHighlight () {
	return hover || selected;
}

/** Track value of target `id` at time t, interpolated on the night's grid:
 * [alt, az, horizon] or null. */
function trackAt (night, id, t) {
	const tar = night.targets[id];
	if (!tar || !tar.track)
		return null;
	const times = night.times;
	if (t <= times[0])
		return tar.track[0];
	if (t >= times[times.length - 1])
		return tar.track[times.length - 1];
	const step = (times[times.length - 1] - times[0]) / (times.length - 1);
	const i = Math.min (times.length - 2, Math.floor ((t - times[0]) / step));
	const f = (t - times[i]) / (times[i + 1] - times[i]);
	const a = tar.track[i], b = tar.track[i + 1];
	if (a[0] === null || b[0] === null)
		return a;
	let daz = b[1] - a[1];
	if (daz > 180) daz -= 360;
	if (daz < -180) daz += 360;
	return [a[0] + f * (b[0] - a[0]), (a[1] + f * daz + 360) % 360, a[2] + f * (b[2] - a[2])];
}

function moonAt (night, t) {
	const times = night.times;
	const step = (times[times.length - 1] - times[0]) / (times.length - 1);
	const i = Math.max (0, Math.min (times.length - 1, Math.round ((t - times[0]) / step)));
	return night.moon[i];
}

/** Angular distance of two alt/az positions, degrees. */
function separation (alt1, az1, alt2, az2) {
	const r = Math.PI / 180;
	const c = Math.sin (alt1 * r) * Math.sin (alt2 * r) + Math.cos (alt1 * r) * Math.cos (alt2 * r) * Math.cos ((az1 - az2) * r);
	return Math.acos (Math.max (-1, Math.min (1, c))) / r;
}

/** One list of what happened and what is planned, both kinds of entry
 * shaped alike: {site, kind: 'obs'|'plan', target, start, end, ...}. */
function buildEntries (site, night) {
	const entries = [];
	for (const o of night.observations) {
		// an observation still running has no end yet
		const end = o.end !== null ? o.end : Math.max (o.start || o.slew, night.now);
		entries.push ({
			site, kind: 'obs', target: o.target, start: o.start !== null ? o.start : o.slew, end,
			slew: o.slew, state: o.state, images: o.images, goodImages: o.goodImages, obsId: o.id,
			running: o.end === null,
		});
	}
	for (const q of night.queue) {
		entries.push ({ site, kind: 'plan', target: q.target, start: q.start, end: q.end, queue: q.queue, qid: q.qid });
	}
	entries.sort ((a, b) => a.start - b.start);
	return entries;
}

/** A recorded value as a step function over the night:
 * [{from, to, value}] with value null where nothing was recorded. */
function buildLane (source, rec, from, to) {
	const pts = rec.points.map (p => [p[0], p[1]]);
	const segs = [];
	const periodic = source.kind !== 'server';
	for (let i = 0; i < pts.length; i++) {
		const t0 = pts[i][0];
		let t1 = i + 1 < pts.length ? pts[i + 1][0] : Math.min (to, Date.now () / 1000);
		if (periodic && t1 - t0 > SAMPLE_GAP) {
			segs.push ({ from: t0, to: t0 + SAMPLE_GAP, value: pts[i][1] });
			segs.push ({ from: t0 + SAMPLE_GAP, to: t1, value: null });
			continue;
		}
		segs.push ({ from: t0, to: t1, value: pts[i][1] });
	}
	return segs.filter (s => s.to > from && s.from < to);
}

async function loadLanes (site, night) {
	let recvals;
	try {
		recvals = await fetchJson (apiUrl (site, 'api/db/recvals'));
	} catch (er) {
		return [];
	}
	const lanes = [];
	for (const src of STATE_SOURCES) {
		if (!recvals.some (r => r.device === src.device && r.value === src.value))
			continue;
		// a day back, so the state in force at sunset is known; points=0
		// is every sample, a state must not be averaged
		const from = night.sunset - 86400;
		try {
			const rec = await fetchJson (apiUrl (site, `api/db/records?device=${encodeURIComponent (src.device)}&value=${encodeURIComponent (src.value)}&from=${from}&to=${night.sunrise}&points=0`));
			lanes.push ({ source: src, segs: buildLane (src, rec, night.sunset, night.sunrise) });
		} catch (er) {
			// listed but unreadable: no lane, rather than a wrong one
		}
	}
	return lanes;
}

async function loadSite (site, date) {
	const q = date ? `date=${encodeURIComponent (date)}&` : '';
	const night = await fetchJson (apiUrl (site, `api/db/schedule?${q}points=240`));
	const [horizon, lanes] = await Promise.all ([
		fetchJson (apiUrl (site, 'api/horizon')).catch (() => null),
		loadLanes (site, night),
	]);
	return { night, horizon, lanes, entries: buildEntries (site, night) };
}

async function load () {
	const date = dateEl.value;
	setStatus (true, 'loading…');
	const results = await Promise.all (SITE_ORDER.map (s =>
		loadSite (s, date).catch (er => ({ error: er.message }))));
	data = {};
	SITE_ORDER.forEach ((s, i) => { data[s] = results[i]; });

	const ok = SITE_ORDER.filter (s => !data[s].error);
	if (!ok.length) {
		setStatus (false, SITE_ORDER.map (s => `${SITES[s].label}: ${data[s].error}`).join ('; '));
		drawAll ();
		return;
	}
	if (data[plotSite].error)
		plotSite = ok[0];
	document.querySelector (`input[name="sch-site"][value="${plotSite}"]`).checked = true;

	const n = data[ok[0]].night;
	if (!date)
		dateEl.value = dateString (n.sunset);
	const parts = [`sunset ${hhmm (n.sunset)}`];
	if (n.nightStart)
		parts.push (`night ${hhmm (n.nightStart)}–${hhmm (n.nightEnd)}`);
	parts.push (`sunrise ${hhmm (n.sunrise)}`, `Moon ${Math.round (n.moonDisk * 100)} % lit`);
	for (const s of SITE_ORDER)
		if (data[s].error)
			parts.push (`${SITES[s].label} unavailable: ${data[s].error}`);
	setStatus (ok.length === SITE_ORDER.length, parts.join (', '));

	drawAll ();

	// tonight keeps itself current; a past or future night does not change
	clearTimeout (refreshTimer);
	if (n.now >= n.sunset - 3 * 3600 && n.now <= n.sunrise)
		refreshTimer = setTimeout (load, 60000);
}

// --- Drawing: shared time axis ------------------------------------------

const PAD = { left: 64, right: 12 };

function timeAxis (night, w) {
	const t0 = night.sunset, t1 = night.sunrise;
	return {
		t0, t1,
		x: t => PAD.left + (t - t0) / (t1 - t0) * (w - PAD.left - PAD.right),
		t: x => t0 + (x - PAD.left) / (w - PAD.left - PAD.right) * (t1 - t0),
	};
}

/** Twilight shading from the Sun's altitude: lighter the brighter. */
function drawTwilight (ctx, night, ax, y0, y1) {
	const times = night.times;
	for (let i = 0; i + 1 < times.length; i++) {
		const sun = night.sun[i];
		let a = 0;
		if (sun > 0) a = 0.28;
		else if (sun > -6) a = 0.2;
		else if (sun > -12) a = 0.13;
		else if (sun > -18) a = 0.06;
		if (!a)
			continue;
		ctx.fillStyle = `rgba(37, 99, 235, ${a})`;
		// whole pixels, or neighbouring slices overlap into stripes
		const x0 = Math.round (ax.x (times[i])), x1 = Math.round (ax.x (times[i + 1]));
		ctx.fillRect (x0, y0, x1 - x0, y1 - y0);
	}
}

function drawHourTicks (ctx, ax, y0, y1, labelY) {
	ctx.strokeStyle = css ('--rec-grid', '#e3e6ea');
	ctx.fillStyle = css ('--rec-fg', '#6b7280');
	ctx.font = '11px sans-serif';
	ctx.textAlign = 'center';
	ctx.textBaseline = 'top';
	ctx.lineWidth = 1;
	const first = Math.ceil (ax.t0 / 3600) * 3600;
	for (let t = first; t <= ax.t1; t += 3600) {
		const x = Math.round (ax.x (t)) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (x, y0);
		ctx.lineTo (x, y1);
		ctx.stroke ();
		if (labelY !== null)
			ctx.fillText (hhmm (t).slice (0, 2), x, labelY);
	}
}

function drawNow (ctx, night, ax, y0, y1) {
	if (night.now < ax.t0 || night.now > ax.t1)
		return;
	const x = Math.round (ax.x (night.now)) + 0.5;
	ctx.strokeStyle = css ('--bad', '#b91c1c');
	ctx.lineWidth = 1.5;
	ctx.beginPath ();
	ctx.moveTo (x, y0);
	ctx.lineTo (x, y1);
	ctx.stroke ();
}

// --- Timeline --------------------------------------------------------------

const LANE_H = 22;
const STATE_LANE_H = 9;

function stateColor (source, value) {
	if (value === null)
		return null;
	if (source.kind === 'server') {
		const onoff = value & SERVERD_ONOFF_MASK;
		if (onoff === 0)
			return 'rgba(21, 128, 61, 0.55)';
		return onoff === SERVERD_STANDBY ? 'rgba(180, 83, 9, 0.6)' : 'rgba(185, 28, 28, 0.6)';
	}
	return value ? 'rgba(21, 128, 61, 0.55)' : 'rgba(185, 28, 28, 0.6)';
}

function stateText (source, value) {
	if (value === null)
		return 'not recorded';
	if (source.kind === 'server') {
		const onoff = value & SERVERD_ONOFF_MASK;
		return onoff === 0 ? 'on' : onoff === SERVERD_STANDBY ? 'standby' : 'off';
	}
	return value ? 'enabled' : 'disabled';
}

function hatch (ctx) {
	const p = document.createElement ('canvas');
	p.width = p.height = 6;
	const c = p.getContext ('2d');
	c.strokeStyle = 'rgba(107, 114, 128, 0.45)';
	c.beginPath ();
	c.moveTo (0, 6);
	c.lineTo (6, 0);
	c.stroke ();
	return ctx.createPattern (p, 'repeat');
}

function drawEntryBlock (ctx, ax, e, y, h) {
	const x0 = ax.x (Math.max (e.start, ax.t0));
	const x1 = Math.max (x0 + 2, ax.x (Math.min (e.end, ax.t1)));
	const lit = isHighlighted (e.site, e.target);
	const dim = anyHighlight () && !lit;
	if (e.kind === 'obs') {
		ctx.fillStyle = targetColor (e.target, dim ? 0.35 : 0.9);
		ctx.fillRect (x0, y, x1 - x0, h);
		if (e.state & (OBS_BIT_INTERUPED | OBS_BIT_ACQUSITION_FAI)) {
			ctx.fillStyle = css ('--bad', '#b91c1c');
			ctx.fillRect (x0, y + h - 4, x1 - x0, 4);
		}
	} else {
		ctx.fillStyle = targetColor (e.target, dim ? 0.12 : 0.35);
		ctx.fillRect (x0, y, x1 - x0, h);
		ctx.strokeStyle = targetColor (e.target, dim ? 0.4 : 1);
		ctx.lineWidth = 1;
		ctx.setLineDash (e.queue === 2 ? [] : [3, 2]);
		ctx.strokeRect (x0 + 0.5, y + 0.5, x1 - x0 - 1, h - 1);
		ctx.setLineDash ([]);
	}
	if (lit) {
		ctx.strokeStyle = css ('--text', '#1c1e21');
		ctx.lineWidth = 2;
		ctx.strokeRect (x0, y - 1, x1 - x0, h + 2);
	}
	if (x1 - x0 > 30) {
		ctx.fillStyle = e.kind === 'obs' && !dim ? '#fff' : css ('--text', '#1c1e21');
		ctx.font = '10px sans-serif';
		ctx.textAlign = 'center';
		ctx.textBaseline = 'middle';
		ctx.fillText (String (e.target), (x0 + x1) / 2, y + h / 2);
	}
	timelineHits.push ({ x0, x1, y0: y, y1: y + h, entry: e });
}

function drawTimeline () {
	timelineHits = [];
	const sites = SITE_ORDER.filter (s => data[s] && !data[s].error);
	let height = 24;
	for (const s of sites)
		height += 18 + 2 * (LANE_H + 4) + data[s].lanes.length * (STATE_LANE_H + 3) + 8;
	const { ctx, w, h } = setupCanvas (timelineCanvas, Math.max (60, height));
	if (!sites.length)
		return;
	const ax = timeAxis (data[sites[0]].night, w);
	drawTwilight (ctx, data[sites[0]].night, ax, 0, h - 18);
	drawHourTicks (ctx, ax, 0, h - 18, h - 15);

	const unknown = hatch (ctx);
	let y = 4;
	for (const s of sites) {
		const d = data[s];
		ctx.fillStyle = css ('--text', '#1c1e21');
		ctx.font = 'bold 12px sans-serif';
		ctx.textAlign = 'left';
		ctx.textBaseline = 'top';
		ctx.fillText (SITES[s].label, 4, y);
		y += 16;

		const lane = (label, fn, lh) => {
			ctx.fillStyle = css ('--rec-fg', '#6b7280');
			ctx.font = '10px sans-serif';
			ctx.textAlign = 'right';
			ctx.textBaseline = 'middle';
			ctx.fillText (label, PAD.left - 6, y + lh / 2);
			fn (y, lh);
			y += lh + (lh === LANE_H ? 4 : 3);
		};

		lane ('observed', (ly, lh) => {
			for (const e of d.entries)
				if (e.kind === 'obs')
					drawEntryBlock (ctx, ax, e, ly, lh);
		}, LANE_H);
		lane ('planned', (ly, lh) => {
			for (const e of d.entries)
				if (e.kind === 'plan')
					drawEntryBlock (ctx, ax, e, ly, lh);
		}, LANE_H);
		for (const l of d.lanes) {
			lane (l.source.label, (ly, lh) => {
				// before the first sample: nothing known
				const firstT = l.segs.length ? l.segs[0].from : ax.t1;
				if (firstT > ax.t0) {
					ctx.fillStyle = unknown;
					ctx.fillRect (ax.x (ax.t0), ly, ax.x (Math.min (firstT, ax.t1)) - ax.x (ax.t0), lh);
				}
				for (const seg of l.segs) {
					const x0 = ax.x (Math.max (seg.from, ax.t0));
					const x1 = ax.x (Math.min (seg.to, ax.t1));
					const c = stateColor (l.source, seg.value);
					ctx.fillStyle = c || unknown;
					ctx.fillRect (x0, ly, Math.max (1, x1 - x0), lh);
					timelineHits.push ({ x0, x1, y0: ly, y1: ly + lh, lane: l.source, seg });
				}
			}, STATE_LANE_H);
		}
		y += 8;
	}
	drawNow (ctx, data[sites[0]].night, ax, 0, h - 18);
}

// --- Elevation plot ---------------------------------------------------------

const ELEV_PAD = { top: 10, bottom: 24 };

function drawElevation () {
	elevHits = [];
	const { ctx, w, h } = setupCanvas (elevCanvas, Math.min (Math.max (300, polarCanvas.clientWidth), 460));
	const d = data[plotSite];
	document.getElementById ('sch-elev-title').textContent = `Elevation, ${SITES[plotSite].label}`;
	if (!d || d.error)
		return;
	const night = d.night;
	const ax = timeAxis (night, w);
	const y = alt => ELEV_PAD.top + (90 - alt) / 90 * (h - ELEV_PAD.top - ELEV_PAD.bottom);
	const yBottom = y (0);

	drawTwilight (ctx, night, ax, ELEV_PAD.top, yBottom);
	drawHourTicks (ctx, ax, ELEV_PAD.top, yBottom, yBottom + 4);
	ctx.strokeStyle = css ('--rec-grid', '#e3e6ea');
	ctx.fillStyle = css ('--rec-fg', '#6b7280');
	ctx.textAlign = 'right';
	ctx.textBaseline = 'middle';
	for (let a = 0; a <= 90; a += 15) {
		const yy = Math.round (y (a)) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (PAD.left, yy);
		ctx.lineTo (w - PAD.right, yy);
		ctx.stroke ();
		ctx.fillText (a + '°', PAD.left - 6, yy);
	}

	const xs = night.times.map (ax.x);
	const polyline = (vals, style, width, dash) => {
		ctx.strokeStyle = style;
		ctx.lineWidth = width;
		ctx.setLineDash (dash || []);
		ctx.beginPath ();
		let pen = false;
		for (let i = 0; i < vals.length; i++) {
			if (vals[i] === null || vals[i] < 0) {
				pen = false;
				continue;
			}
			if (pen)
				ctx.lineTo (xs[i], y (vals[i]));
			else
				ctx.moveTo (xs[i], y (vals[i]));
			pen = true;
		}
		ctx.stroke ();
		ctx.setLineDash ([]);
	};

	// Moon
	polyline (night.moon.map (m => m[0]), css ('--sky-moon', '#b45309'), 1, [2, 3]);

	// every planned/observed target's whole night, faint: whether a slot
	// caught it near its best is read off these
	const h0 = anyHighlight ();
	const targetsShown = new Set (d.entries.map (e => e.target));
	for (const id of targetsShown) {
		const tar = night.targets[id];
		if (!tar || !tar.track)
			continue;
		const lit = isHighlighted (plotSite, id);
		if (h0 && !lit)
			continue;
		polyline (tar.track.map (p => p[0]), targetColor (id, lit ? 0.8 : 0.18), lit ? 1.5 : 1);
		if (lit)
			polyline (tar.track.map (p => p[2]), css ('--sky-horizon', '#6b7280'), 1.5, [5, 3]);
	}

	// the slots themselves, thick
	for (const e of d.entries) {
		const tar = night.targets[e.target];
		if (!tar || !tar.track)
			continue;
		const lit = isHighlighted (plotSite, e.target);
		const dim = h0 && !lit;
		const t0 = Math.max (e.start, ax.t0), t1 = Math.min (e.end, ax.t1);
		if (t1 <= t0)
			continue;
		ctx.strokeStyle = targetColor (e.target, dim ? 0.3 : 1);
		ctx.lineWidth = e.kind === 'obs' ? 4 : 3;
		ctx.setLineDash (e.kind === 'obs' ? [] : [6, 3]);
		ctx.beginPath ();
		const n = Math.max (2, Math.ceil ((ax.x (t1) - ax.x (t0)) / 4));
		let mid = null;
		for (let i = 0; i <= n; i++) {
			const t = t0 + (t1 - t0) * i / n;
			const p = trackAt (night, e.target, t);
			if (!p)
				continue;
			const px = ax.x (t), py = y (Math.max (0, p[0]));
			if (i === 0)
				ctx.moveTo (px, py);
			else
				ctx.lineTo (px, py);
			if (i === Math.floor (n / 2))
				mid = [px, py];
		}
		ctx.stroke ();
		ctx.setLineDash ([]);
		if (mid) {
			elevHits.push ({ x0: ax.x (t0), x1: ax.x (t1), y0: mid[1] - 8, y1: mid[1] + 8, entry: e });
			if (!dim) {
				ctx.fillStyle = css ('--text', '#1c1e21');
				ctx.font = '10px sans-serif';
				ctx.textAlign = 'center';
				ctx.textBaseline = 'bottom';
				ctx.fillText (String (e.target), mid[0], mid[1] - 4);
			}
		}
	}
	drawNow (ctx, night, ax, ELEV_PAD.top, yBottom);
}

// --- Sky (polar) plot -----------------------------------------------------

function drawPolar () {
	const size = Math.min (polarCanvas.clientWidth, 460);
	const { ctx, w, h } = setupCanvas (polarCanvas, size);
	const d = data[plotSite];
	document.getElementById ('sch-polar-title').textContent = `Sky, ${SITES[plotSite].label}`;
	if (!d || d.error)
		return;
	const night = d.night;
	const cx = w / 2, cy = h / 2, R = Math.min (w, h) / 2 - 18;
	// zenith in the middle, North up, East on the left: looking up
	const project = (alt, azLibnova) => {
		const r = R * (90 - Math.max (alt, 0)) / 90;
		const a = compassAz (azLibnova) * Math.PI / 180;
		return [cx - r * Math.sin (a), cy - r * Math.cos (a)];
	};

	ctx.strokeStyle = css ('--rec-grid', '#e3e6ea');
	ctx.lineWidth = 1;
	for (let a = 0; a < 90; a += 15) {
		ctx.beginPath ();
		ctx.arc (cx, cy, R * (90 - a) / 90, 0, 2 * Math.PI);
		ctx.stroke ();
	}
	ctx.fillStyle = css ('--rec-fg', '#6b7280');
	ctx.font = '11px sans-serif';
	ctx.textAlign = 'center';
	ctx.textBaseline = 'middle';
	for (const [label, az] of [['N', 180], ['E', 270], ['S', 0], ['W', 90]]) {
		const [x, y] = project (-7, az);
		ctx.fillText (label, x, y);
	}

	// ground: outside the horizon polygon
	const hz = d.horizon && d.horizon.horizon && d.horizon.horizon.length ? d.horizon.horizon.slice () : null;
	if (hz) {
		hz.sort ((a, b) => compassAz (a[0]) - compassAz (b[0]));
		ctx.beginPath ();
		ctx.arc (cx, cy, R, 0, 2 * Math.PI);
		hz.forEach ((p, i) => {
			const [x, y] = project (p[1], p[0]);
			if (i === 0) ctx.moveTo (x, y); else ctx.lineTo (x, y);
		});
		ctx.closePath ();
		ctx.fillStyle = css ('--sky-horizon-fill', 'rgba(107,114,128,0.25)');
		ctx.fill ('evenodd');
		ctx.strokeStyle = css ('--sky-horizon', '#6b7280');
		ctx.lineWidth = 1.5;
		ctx.stroke ();
	}

	const h0 = anyHighlight ();
	// the highlighted target's whole night, thin
	if (h0 && h0.site === plotSite) {
		const tar = night.targets[h0.target];
		if (tar && tar.track) {
			ctx.strokeStyle = targetColor (h0.target, 0.6);
			ctx.lineWidth = 1;
			ctx.beginPath ();
			let pen = false;
			for (const p of tar.track) {
				if (p[0] === null || p[0] < 0) { pen = false; continue; }
				const [x, y] = project (p[0], p[1]);
				if (pen) ctx.lineTo (x, y); else ctx.moveTo (x, y);
				pen = true;
			}
			ctx.stroke ();
		}
	}

	// slews: from where one entry ends to where the next starts, in the
	// order the telescope goes (observed up to now, then the plan)
	const seq = d.entries.filter (e => e.kind === 'obs' || e.end > night.now).sort ((a, b) => a.start - b.start);
	ctx.strokeStyle = css ('--rec-fg', '#6b7280');
	ctx.lineWidth = 0.8;
	ctx.setLineDash ([2, 3]);
	for (let i = 0; i + 1 < seq.length; i++) {
		const a = trackAt (night, seq[i].target, seq[i].end);
		const b = trackAt (night, seq[i + 1].target, seq[i + 1].start);
		if (!a || !b)
			continue;
		const [x0, y0] = project (a[0], a[1]);
		const [x1, y1] = project (b[0], b[1]);
		ctx.beginPath ();
		ctx.moveTo (x0, y0);
		ctx.lineTo (x1, y1);
		ctx.stroke ();
	}
	ctx.setLineDash ([]);

	// the slots
	for (const e of d.entries) {
		const lit = isHighlighted (plotSite, e.target);
		const dim = h0 && !lit;
		ctx.strokeStyle = targetColor (e.target, dim ? 0.25 : 1);
		ctx.lineWidth = lit ? 6 : (e.kind === 'obs' ? 4 : 3);
		ctx.setLineDash (e.kind === 'obs' ? [] : [4, 2]);
		ctx.beginPath ();
		const n = 12;
		let last = null;
		for (let i = 0; i <= n; i++) {
			const p = trackAt (night, e.target, e.start + (e.end - e.start) * i / n);
			if (!p)
				continue;
			const [x, y] = project (p[0], p[1]);
			if (i === 0) ctx.moveTo (x, y); else ctx.lineTo (x, y);
			last = [x, y];
		}
		ctx.stroke ();
		ctx.setLineDash ([]);
		if (last && !dim) {
			ctx.fillStyle = css ('--text', '#1c1e21');
			ctx.font = '10px sans-serif';
			ctx.textAlign = 'left';
			ctx.textBaseline = 'middle';
			ctx.fillText (String (e.target), last[0] + 4, last[1]);
		}
	}

	// the Moon now, or at midnight of a night that is not tonight
	const tMoon = night.now >= night.sunset && night.now <= night.sunrise ? night.now : (night.sunset + night.sunrise) / 2;
	const m = moonAt (night, tMoon);
	if (m && m[0] > 0) {
		const [x, y] = project (m[0], m[1]);
		ctx.fillStyle = css ('--sky-moon', '#b45309');
		ctx.beginPath ();
		ctx.arc (x, y, 6, 0, 2 * Math.PI);
		ctx.fill ();
		ctx.fillText (`Moon ${hhmm (tMoon)}`, x + 9, y);
	}
}

// --- List ------------------------------------------------------------------

function entryWhat (e, now) {
	if (e.kind === 'obs') {
		if (e.running)
			return 'running';
		if (e.state & OBS_BIT_ACQUSITION_FAI)
			return 'acquisition failed';
		if (e.state & OBS_BIT_INTERUPED)
			return 'interrupted';
		return 'observed';
	}
	const q = e.queue === 2 ? 'planned' : `planned (queue ${e.queue})`;
	return e.end < now ? q + ', past' : q;
}

function drawList () {
	const rows = [];
	for (const s of SITE_ORDER) {
		const d = data[s];
		if (!d || d.error)
			continue;
		for (const e of d.entries)
			rows.push ({ e, night: d.night });
	}
	rows.sort ((a, b) => a.e.start - b.e.start);
	listBody.innerHTML = '';
	for (const { e, night } of rows) {
		const tar = night.targets[e.target] || {};
		const a = trackAt (night, e.target, e.start);
		const b = trackAt (night, e.target, e.end);
		const mid = trackAt (night, e.target, (e.start + e.end) / 2);
		const moon = moonAt (night, (e.start + e.end) / 2);
		const moonSep = mid && moon && moon[0] > 0 ? separation (mid[0], mid[1], moon[0], moon[1]) : null;
		const tr = document.createElement ('tr');
		tr.className = 'sch-row-' + e.kind + (isHighlighted (e.site, e.target) ? ' sch-row-lit' : '');
		tr.innerHTML = `<td>${hhmm (e.start)}</td><td>${hhmm (e.end)}</td>`
			+ `<td class="num">${Math.round ((e.end - e.start) / 60)}</td>`
			+ `<td>${SITES[e.site].label}</td>`
			+ `<td>${escapeHtml (entryWhat (e, night.now))}</td>`
			+ `<td><span class="sch-swatch" style="background:${targetColor (e.target)}"></span>`
			+ `<a href="target.html?id=${e.target}">${e.target}</a> ${escapeHtml (tar.name || '')}</td>`
			+ `<td>${escapeHtml (tar.type || '')}</td>`
			+ `<td class="num">${a && b ? `${Math.round (a[0])}° → ${Math.round (b[0])}°` : ''}</td>`
			+ `<td class="num">${moonSep !== null ? Math.round (moonSep) + '°' : ''}</td>`
			+ `<td class="num">${e.kind === 'obs' ? `${e.goodImages}/${e.images}` : ''}</td>`;
		tr.addEventListener ('mouseenter', () => setHover ({ site: e.site, target: e.target }));
		tr.addEventListener ('mouseleave', () => setHover (null));
		tr.addEventListener ('click', (ev) => {
			if (ev.target.tagName !== 'A')
				toggleSelected ({ site: e.site, target: e.target });
		});
		listBody.appendChild (tr);
	}
}

function drawPlots () {
	drawTimeline ();
	drawElevation ();
	drawPolar ();
}

function drawAll () {
	drawPlots ();
	drawList ();
}

function refreshHighlight () {
	drawPlots ();
	for (const tr of listBody.children)
		tr.classList.remove ('sch-row-lit');
	const h = anyHighlight ();
	if (!h)
		return;
	// rows are in the same order drawList() built them in
	let i = 0;
	const rows = [];
	for (const s of SITE_ORDER)
		if (data[s] && !data[s].error)
			for (const e of data[s].entries)
				rows.push (e);
	rows.sort ((a, b) => a.start - b.start);
	for (const e of rows) {
		if (e.site === h.site && e.target === h.target)
			listBody.children[i].classList.add ('sch-row-lit');
		i++;
	}
}

// --- Interaction -----------------------------------------------------------

function setHover (h) {
	if ((h && hover && h.site === hover.site && h.target === hover.target) || (!h && !hover))
		return;
	hover = h;
	refreshHighlight ();
}

function toggleSelected (s) {
	if (selected && s && selected.site === s.site && selected.target === s.target)
		selected = null;
	else
		selected = s;
	if (s && s.site !== plotSite && data[s.site] && !data[s.site].error) {
		plotSite = s.site;
		document.querySelector (`input[name="sch-site"][value="${plotSite}"]`).checked = true;
	}
	refreshHighlight ();
}

function entryTooltip (e) {
	const d = data[e.site];
	const tar = d.night.targets[e.target] || {};
	let s = `<b>${e.target} ${escapeHtml (tar.name || '')}</b> (${SITES[e.site].label})<br>`
		+ `${hhmm (e.start)}–${hhmm (e.end)}, ${Math.round ((e.end - e.start) / 60)} min, ${escapeHtml (entryWhat (e, d.night.now))}`;
	if (e.kind === 'obs')
		s += `<br>observation ${e.obsId}, ${e.goodImages}/${e.images} good images`;
	return s;
}

function showTooltip (ev, html) {
	if (!html) {
		tooltipEl.hidden = true;
		return;
	}
	tooltipEl.innerHTML = html;
	tooltipEl.hidden = false;
	tooltipEl.style.left = Math.min (ev.clientX + 12, window.innerWidth - tooltipEl.offsetWidth - 8) + 'px';
	tooltipEl.style.top = (ev.clientY + 14) + 'px';
}

function hitAt (hits, canvas, ev) {
	const r = canvas.getBoundingClientRect ();
	const x = ev.clientX - r.left, y = ev.clientY - r.top;
	// last drawn is on top
	for (let i = hits.length - 1; i >= 0; i--) {
		const h = hits[i];
		if (x >= h.x0 - 1 && x <= h.x1 + 1 && y >= h.y0 && y <= h.y1)
			return h;
	}
	return null;
}

function wireCanvas (canvas, hitsFn) {
	canvas.addEventListener ('mousemove', (ev) => {
		const h = hitAt (hitsFn (), canvas, ev);
		if (h && h.entry) {
			setHover ({ site: h.entry.site, target: h.entry.target });
			showTooltip (ev, entryTooltip (h.entry));
		} else if (h && h.lane) {
			setHover (null);
			showTooltip (ev, `${escapeHtml (h.lane.label)}: ${stateText (h.lane, h.seg.value)}<br>${hhmm (h.seg.from)}–${hhmm (h.seg.to)}`);
		} else {
			setHover (null);
			showTooltip (ev, null);
		}
	});
	canvas.addEventListener ('mouseleave', () => {
		setHover (null);
		showTooltip (null, null);
	});
	canvas.addEventListener ('click', (ev) => {
		const h = hitAt (hitsFn (), canvas, ev);
		toggleSelected (h && h.entry ? { site: h.entry.site, target: h.entry.target } : null);
	});
}

wireCanvas (timelineCanvas, () => timelineHits);
wireCanvas (elevCanvas, () => elevHits);

function shiftDate (days) {
	const base = dateEl.value ? new Date (dateEl.value + 'T12:00:00') : new Date ();
	base.setDate (base.getDate () + days);
	dateEl.value = dateString (base.getTime () / 1000);
	selected = null;
	load ();
}

document.getElementById ('sch-prev').addEventListener ('click', () => shiftDate (-1));
document.getElementById ('sch-next').addEventListener ('click', () => shiftDate (1));
document.getElementById ('sch-tonight').addEventListener ('click', () => {
	dateEl.value = '';
	selected = null;
	load ();
});
dateEl.addEventListener ('change', () => {
	selected = null;
	load ();
});
for (const r of document.querySelectorAll ('input[name="sch-site"]')) {
	r.addEventListener ('change', () => {
		plotSite = r.value;
		storageSet (PLOT_SITE_KEY, plotSite);
		drawPlots ();
	});
}

let resizeTimer = null;
window.addEventListener ('resize', () => {
	clearTimeout (resizeTimer);
	resizeTimer = setTimeout (drawPlots, 150);
});

load ();
