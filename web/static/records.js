// RTS2 telemetry graphs - vanilla JS and a plain 2d canvas, no build
// step and no charting library, same as the rest of this frontend.
//
// The data comes from /api/db/records, which rts2-recordd wrote (see
// db/recordd/). This replaces classic rts2-xmlrpcd's /graph/DEVICE/VALUE,
// which rendered a PNG server-side with GraphicsMagick - dropped along
// with the rest of the server-side image rendering (web/STATUS.md), so
// the daemon ships numbers and the browser draws them. That also makes
// the plot interactive for free: hover reads out a sample, and a
// downsampled range can show the min/max spread behind each point
// instead of a bare average.

const valueEl = document.getElementById('rec-value');
const fromEl = document.getElementById('rec-from');
const toEl = document.getElementById('rec-to');
const formEl = document.getElementById('rec-form');
const statusEl = document.getElementById('rec-status');
const resultEl = document.getElementById('rec-result');
const titleEl = document.getElementById('rec-title');
const extentEl = document.getElementById('rec-extent');
const canvas = document.getElementById('rec-chart');
const tooltipEl = document.getElementById('rec-tooltip');
const ctx = canvas.getContext('2d');

// { device, value, points: [[t, avg, min, max, n], ...] }
let series = null;
// plot geometry from the last draw, needed to turn a mouse position back
// into a sample
let plot = null;

const PAD = { left: 64, right: 16, top: 16, bottom: 32 };

function setStatus (text, cls) {
	statusEl.textContent = text;
	statusEl.className = 'conn-status ' + cls;
}

function setResult (text, cls) {
	resultEl.textContent = text;
	resultEl.className = cls || '';
}

// <input type="datetime-local"> speaks local wall-clock time with no zone
// and no trailing Z, which is neither what Date's ISO output gives nor
// what its parser assumes - hence converting through the local offset in
// both directions rather than slicing toISOString().
function toInputValue (t) {
	const d = new Date (t * 1000);
	const local = new Date (d.getTime () - d.getTimezoneOffset () * 60000);
	return local.toISOString ().slice (0, 19);
}

function fromInputValue (v) {
	if (!v)
		return null;
	const t = new Date (v).getTime ();
	return isNaN (t) ? null : t / 1000;
}

function formatNumber (v) {
	if (v === null || !isFinite (v))
		return '-';
	if (Number.isInteger (v))
		return String (v);
	const a = Math.abs (v);
	if (a !== 0 && (a < 0.001 || a >= 100000))
		return v.toExponential (3);
	return v.toFixed (a < 1 ? 4 : 3).replace (/\.?0+$/, '');
}

function formatTime (t, withDate) {
	const d = new Date (t * 1000);
	const hh = String (d.getHours ()).padStart (2, '0');
	const mm = String (d.getMinutes ()).padStart (2, '0');
	const ss = String (d.getSeconds ()).padStart (2, '0');
	if (!withDate)
		return hh + ':' + mm + ':' + ss;
	const mo = String (d.getMonth () + 1).padStart (2, '0');
	const dd = String (d.getDate ()).padStart (2, '0');
	return d.getFullYear () + '-' + mo + '-' + dd + ' ' + hh + ':' + mm;
}

function setRangeHours (hours) {
	const now = Date.now () / 1000;
	fromEl.value = toInputValue (now - hours * 3600);
	toEl.value = toInputValue (now);
}

async function loadRecvals () {
	setStatus ('loading...', 'conn-unknown');
	try {
		const response = await fetch ('api/db/recvals');
		const recvals = await response.json ();
		if (recvals.error)
			throw new Error (recvals.error);

		valueEl.innerHTML = '';
		if (!recvals.length) {
			setStatus ('nothing recorded', 'conn-closed');
			setResult ('No values are being recorded yet. rts2-recordd stores them; '
				+ 'see its configuration file for which device values it samples.', 'err');
			draw ();
			return;
		}

		// Grouped by device, so a site recording a dozen values across
		// three devices still has a usable picker.
		let group = null;
		let lastDevice = null;
		for (const rv of recvals) {
			if (rv.device !== lastDevice) {
				group = document.createElement ('optgroup');
				group.label = rv.device;
				valueEl.appendChild (group);
				lastDevice = rv.device;
			}
			const option = document.createElement ('option');
			option.value = rv.device + ' ' + rv.value;
			option.textContent = rv.value;
			option.dataset.from = rv.from === null ? '' : rv.from;
			option.dataset.to = rv.to === null ? '' : rv.to;
			option.dataset.samples = rv.samples;
			group.appendChild (option);
		}

		setStatus (recvals.length + ' recorded value(s)', 'conn-open');
		showExtent ();
		await load ();
	} catch (er) {
		setStatus ('failed', 'conn-closed');
		setResult ('Cannot list recorded values: ' + er.message, 'err');
	}
}

function showExtent () {
	const option = valueEl.selectedOptions[0];
	if (!option) {
		extentEl.textContent = '';
		return;
	}
	const from = parseFloat (option.dataset.from);
	const to = parseFloat (option.dataset.to);
	if (!isFinite (from) || !isFinite (to)) {
		extentEl.textContent = 'no samples stored yet';
		return;
	}
	extentEl.textContent = option.dataset.samples + ' samples stored, from '
		+ formatTime (from, true) + ' to ' + formatTime (to, true);
}

async function load () {
	const option = valueEl.selectedOptions[0];
	if (!option)
		return;
	const [device, value] = option.value.split (' ');

	const from = fromInputValue (fromEl.value);
	const to = fromInputValue (toEl.value);
	if (from === null || to === null || !(from < to)) {
		setResult ('Pick a time range that starts before it ends.', 'err');
		return;
	}

	// One point per canvas pixel is as much as the plot can show; asking
	// for more just makes the response bigger.
	const points = Math.max (100, Math.round (canvas.clientWidth || 800));

	setResult ('loading...');
	try {
		const url = 'api/db/records?device=' + encodeURIComponent (device)
			+ '&value=' + encodeURIComponent (value)
			+ '&from=' + from + '&to=' + to + '&points=' + points;
		const response = await fetch (url);
		const data = await response.json ();
		if (data.error)
			throw new Error (data.error);

		series = data;
		titleEl.textContent = data.device + '.' + data.value;
		setResult (data.points.length
			? data.points.length + ' point(s)'
			: 'No samples in this range.', data.points.length ? 'ok' : 'err');
		draw ();
	} catch (er) {
		series = null;
		setResult ('Cannot load records: ' + er.message, 'err');
		draw ();
	}
}

/** "Nice" axis step - 1, 2 or 5 times a power of ten. */
function niceStep (raw) {
	const exp = Math.pow (10, Math.floor (Math.log10 (raw)));
	const norm = raw / exp;
	if (norm <= 1)
		return exp;
	if (norm <= 2)
		return 2 * exp;
	if (norm <= 5)
		return 5 * exp;
	return 10 * exp;
}

function draw () {
	// Canvas backing store in device pixels, CSS box in layout pixels -
	// without this the plot is blurry on any HiDPI screen.
	const ratio = window.devicePixelRatio || 1;
	const width = canvas.clientWidth;
	const height = canvas.clientHeight;
	canvas.width = Math.round (width * ratio);
	canvas.height = Math.round (height * ratio);
	ctx.setTransform (ratio, 0, 0, ratio, 0, 0);

	const style = getComputedStyle (document.body);
	const fg = style.getPropertyValue ('--rec-fg').trim () || '#e8e8e8';
	const grid = style.getPropertyValue ('--rec-grid').trim () || '#3a3a3a';
	const line = style.getPropertyValue ('--rec-line').trim () || '#4da3ff';
	const band = style.getPropertyValue ('--rec-band').trim () || 'rgba(77,163,255,0.25)';

	ctx.clearRect (0, 0, width, height);
	plot = null;

	if (!series || !series.points.length) {
		ctx.fillStyle = fg;
		ctx.font = '14px sans-serif';
		ctx.textAlign = 'center';
		ctx.fillText ('no data', width / 2, height / 2);
		return;
	}

	const points = series.points;
	const x0 = PAD.left, x1 = width - PAD.right;
	const y0 = PAD.top, y1 = height - PAD.bottom;
	if (x1 <= x0 || y1 <= y0)
		return;

	const tMin = series.from, tMax = series.to;
	let vMin = Infinity, vMax = -Infinity;
	for (const p of points) {
		for (const v of [p[1], p[2], p[3]]) {
			if (v === null || !isFinite (v))
				continue;
			if (v < vMin) vMin = v;
			if (v > vMax) vMax = v;
		}
	}
	if (!isFinite (vMin) || !isFinite (vMax)) {
		vMin = 0;
		vMax = 1;
	}
	if (vMin === vMax) {
		// a perfectly flat series still deserves a line through the
		// middle rather than a division by zero
		vMin -= 0.5;
		vMax += 0.5;
	} else {
		const margin = (vMax - vMin) * 0.05;
		vMin -= margin;
		vMax += margin;
	}

	const sx = t => x0 + (t - tMin) / (tMax - tMin) * (x1 - x0);
	const sy = v => y1 - (v - vMin) / (vMax - vMin) * (y1 - y0);

	// y grid + labels
	ctx.strokeStyle = grid;
	ctx.fillStyle = fg;
	ctx.lineWidth = 1;
	ctx.font = '12px sans-serif';
	ctx.textAlign = 'right';
	ctx.textBaseline = 'middle';
	const yStep = niceStep ((vMax - vMin) / 5);
	for (let v = Math.ceil (vMin / yStep) * yStep; v <= vMax; v += yStep) {
		const y = Math.round (sy (v)) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (x0, y);
		ctx.lineTo (x1, y);
		ctx.stroke ();
		ctx.fillText (formatNumber (v), x0 - 8, y);
	}

	// x grid + labels
	ctx.textAlign = 'center';
	ctx.textBaseline = 'top';
	const span = tMax - tMin;
	const withDate = span > 86400;
	const xStep = niceStep (span / 6);
	for (let t = Math.ceil (tMin / xStep) * xStep; t <= tMax; t += xStep) {
		const x = Math.round (sx (t)) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (x, y0);
		ctx.lineTo (x, y1);
		ctx.stroke ();
		// A centred label on the last tick hangs off the canvas and gets
		// clipped mid-digit ("16:10:5"); the outermost labels are aligned
		// inwards instead of being centred on their tick.
		const label = formatTime (t, withDate);
		const half = ctx.measureText (label).width / 2;
		ctx.textAlign = x + half > width ? 'right' : (x - half < 0 ? 'left' : 'center');
		ctx.fillText (label, x, y1 + 6);
	}
	ctx.textAlign = 'center';

	// min/max band, drawn only where a bucket actually holds more than
	// one sample - otherwise it is the line itself and adds nothing
	const bucketed = points.some (p => p[4] > 1);
	if (bucketed) {
		ctx.fillStyle = band;
		ctx.beginPath ();
		let started = false;
		for (const p of points) {
			if (p[3] === null || !isFinite (p[3]))
				continue;
			const x = sx (p[0]), y = sy (p[3]);
			started ? ctx.lineTo (x, y) : ctx.moveTo (x, y);
			started = true;
		}
		for (let i = points.length - 1; i >= 0; i--) {
			const p = points[i];
			if (p[2] === null || !isFinite (p[2]))
				continue;
			ctx.lineTo (sx (p[0]), sy (p[2]));
		}
		ctx.closePath ();
		ctx.fill ();
	}

	// the series itself; a gap in the data stays a gap, rather than
	// being bridged by a straight line that was never measured
	ctx.strokeStyle = line;
	ctx.lineWidth = 1.5;
	ctx.beginPath ();
	let pen = false;
	for (const p of points) {
		if (p[1] === null || !isFinite (p[1])) {
			pen = false;
			continue;
		}
		const x = sx (p[0]), y = sy (p[1]);
		pen ? ctx.lineTo (x, y) : ctx.moveTo (x, y);
		pen = true;
	}
	ctx.stroke ();

	plot = { x0, x1, y0, y1, tMin, tMax, sx, sy, withDate };
}

function onMove (event) {
	if (!plot || !series || !series.points.length) {
		tooltipEl.style.display = 'none';
		return;
	}
	const rect = canvas.getBoundingClientRect ();
	const x = event.clientX - rect.left;
	if (x < plot.x0 || x > plot.x1) {
		tooltipEl.style.display = 'none';
		return;
	}

	const t = plot.tMin + (x - plot.x0) / (plot.x1 - plot.x0) * (plot.tMax - plot.tMin);
	let best = null, bestDist = Infinity;
	for (const p of series.points) {
		const d = Math.abs (p[0] - t);
		if (d < bestDist) {
			bestDist = d;
			best = p;
		}
	}
	if (!best || best[1] === null || !isFinite (best[1])) {
		tooltipEl.style.display = 'none';
		return;
	}

	let text = formatTime (best[0], true) + '  ' + formatNumber (best[1]);
	if (best[4] > 1)
		text += '  (' + best[4] + ' samples, ' + formatNumber (best[2]) + ' to ' + formatNumber (best[3]) + ')';
	tooltipEl.textContent = text;
	tooltipEl.style.display = 'block';
	// keep the tooltip inside the canvas rather than letting it hang off
	// the right edge on the last sample
	const tipWidth = tooltipEl.offsetWidth;
	let left = plot.sx (best[0]) + 12;
	if (left + tipWidth > rect.width)
		left = plot.sx (best[0]) - tipWidth - 12;
	tooltipEl.style.left = left + 'px';
	tooltipEl.style.top = (plot.sy (best[1]) - 28) + 'px';
}

formEl.addEventListener ('submit', event => {
	event.preventDefault ();
	load ();
});

for (const button of formEl.querySelectorAll ('button[data-hours]')) {
	button.addEventListener ('click', () => {
		setRangeHours (parseFloat (button.dataset.hours));
		load ();
	});
}

valueEl.addEventListener ('change', () => {
	showExtent ();
	load ();
});

canvas.addEventListener ('mousemove', onMove);
canvas.addEventListener ('mouseleave', () => { tooltipEl.style.display = 'none'; });

let resizeTimer = null;
window.addEventListener ('resize', () => {
	// redraw, don't refetch: the point count only matters when the
	// window changes by a lot, and refetching on every resize event
	// would hammer the database
	clearTimeout (resizeTimer);
	resizeTimer = setTimeout (draw, 150);
});

setRangeHours (24);
loadRecvals ();
