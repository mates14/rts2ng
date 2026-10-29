// RTS2 nightreport (nightreport.html) - what `rts2-nightreport -i` prints,
// from api/db/nightreport (the very lines classic prints), refreshed every
// 30 s. Shown whole, as the last N rows, or as many as fit the window
// without scrolling; a cut inside an observation keeps its line on top. An
// image row's acquisition time is a link: the mouse over it shows the
// preview (switched off by a checkbox), a click opens it big
// (lightbox.js), corrected or raw as chosen in the image archive.

const listEl = document.getElementById ('nr-list');
const nightEl = document.getElementById ('nr-night');
const tailEl = document.getElementById ('nr-tail');
const rangeEl = document.getElementById ('nr-range');
const connEl = document.getElementById ('nr-conn');
const hoverEl = document.getElementById ('nr-hover');
const hoverOnEl = document.getElementById ('nr-hover-on');
const camerasEl = document.getElementById ('nr-cameras');

const REFRESH_MS = 30000;
const HOVER_SIZE = 400;				// preview/?ps= under the mouse
const BIG_SIZE = 2048;				// and enlarged

let report = null;					// the last api/db/nightreport answer
let shown = new Set ();				// rows already seen, to mark new ones
let firstLoad = true;
let refreshTimer = null;

function escapeHtml (s) {
	return String (s).replace (/[&<>"']/g, c => ({
		'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
	}[c]));
}

function encPath (p) {
	return p.split ('/').map (encodeURIComponent).join ('/');
}

function calibrated () {
	try {
		return localStorage.getItem ('archiveCalibrated') !== '0';
	} catch (e) {
		return true;
	}
}

function utText (t) {
	return new Date (t * 1000).toISOString ().slice (0, 19).replace ('T', ' ') + ' UT';
}

function showMode () {
	return document.querySelector ('input[name="nr-show"]:checked').value;
}

// --- the night --------------------------------------------------------

function nightParams () {
	const [y, m, d] = nightEl.value.split ('-').map (Number);
	return `year=${y}&month=${m}&day=${d}`;
}

async function currentNight () {
	const res = await fetch ('api/db/current-night');
	const n = await res.json ();
	return `${n.year}-${String (n.month).padStart (2, '0')}-${String (n.day).padStart (2, '0')}`;
}

// --- rows ---------------------------------------------------------------

// every row of the report, as classic prints them: an observation, then
// its images or "--- no images ---"
// The camera of an image: the first word of classic's line.
function cameraOf (im) {
	return im.text.trim ().split (/\s+/)[0];
}

// Cameras switched off; kept in the browser as the unticked ones, so that
// a camera new in a report starts ticked.
let hiddenCameras = new Set ();
try {
	hiddenCameras = new Set (JSON.parse (localStorage.getItem ('nightreportHiddenCameras') || '[]'));
} catch (e) {
}

// a checkbox for each camera with images in the report
function renderCameras () {
	const cams = [...new Set (report.observations.flatMap (o => o.images.map (cameraOf)))].sort ();
	const html = cams.length < 2 ? '' : 'cameras ' + cams.map (c =>
		`<label><input type="checkbox" value="${escapeHtml (c)}"${hiddenCameras.has (c) ? '' : ' checked'}> ${escapeHtml (c)}</label>`).join (' ');
	if (camerasEl.dataset.html !== html) {
		camerasEl.dataset.html = html;
		camerasEl.innerHTML = html;
	}
}

camerasEl.addEventListener ('change', ev => {
	const box = ev.target.closest ('input[type="checkbox"]');
	if (!box)
		return;
	if (box.checked)
		hiddenCameras.delete (box.value);
	else
		hiddenCameras.add (box.value);
	try {
		localStorage.setItem ('nightreportHiddenCameras', JSON.stringify ([...hiddenCameras]));
	} catch (e) {
	}
	camerasEl.dataset.html = '';
	// rows that come back are not new
	firstLoad = true;
	render ();
});

// every row of the report, as classic prints them: an observation, then
// its images (those of the ticked cameras) or "--- no images ---"
function allRows () {
	const rows = [];
	for (const o of report.observations) {
		const head = { kind: 'obs', text: o.text, key: 'o' + o.id };
		rows.push (head);
		const images = o.images.filter (im => !hiddenCameras.has (cameraOf (im)));
		if (images.length === 0)
			rows.push ({ kind: 'none', text: '--- no images ---', key: 'n' + o.id, head });
		for (const im of images)
			rows.push ({ kind: 'img', text: im.text, key: 'i' + o.id + '/' + im.file, head, im });
	}
	return rows;
}

// the exposure start in classic's image line (Timestamp: date, T, time,
// zone), which the preview and the enlarged image hang on
const TIME_RE = /\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)? \S+/;

function rowHtml (r, fresh) {
	const cls = `nr-row nr-${r.kind}${fresh ? ' nr-new' : ''}`;
	const m = r.kind === 'img' && r.im.previewPath ? TIME_RE.exec (r.text) : null;
	const text = m
		? escapeHtml (r.text.slice (0, m.index)) + `<span class="nr-name" title="${escapeHtml (r.im.file)} - click: enlarged">${escapeHtml (m[0])}</span>` + escapeHtml (r.text.slice (m.index + m[0].length))
		: escapeHtml (r.text);
	return `<div class="${cls}" data-key="${escapeHtml (r.key)}">${text}</div>`;
}

// the rows that fit between the list's top and the window's bottom
function fittingRows () {
	const probe = document.createElement ('div');
	probe.className = 'nr-row nr-img';
	probe.textContent = 'x';
	listEl.appendChild (probe);
	const rowH = probe.getBoundingClientRect ().height || 16;
	probe.remove ();
	return Math.max (3, Math.floor ((listEl.clientHeight || window.innerHeight - listEl.getBoundingClientRect ().top) / rowH));
}

let rendered = [];

function render () {
	if (!report)
		return;
	renderCameras ();
	const rows = allRows ();
	const mode = showMode ();
	// fit: first the list's height, ending at the window's bottom;
	// then the rows it holds.
	document.body.classList.toggle ('nr-fit', mode === 'fit');
	listEl.style.height = '';
	if (mode === 'fit') {
		listEl.style.height = `${Math.max (60, window.innerHeight - listEl.getBoundingClientRect ().top)}px`;
		// less what the page then still overflows by (the panel's
		// padding and margin, the body's below the list)
		const over = document.documentElement.scrollHeight - window.innerHeight;
		if (over > 0)
			listEl.style.height = `${Math.max (60, listEl.clientHeight - over)}px`;
	}
	let view;
	if (mode === 'all')
		view = rows;
	else {
		const n = mode === 'tail' ? Math.max (1, parseInt (tailEl.value, 10) || 20) : fittingRows ();
		view = rows.slice (Math.max (0, rows.length - n));
		// a cut inside an observation: its line on top, in place of the
		// first row
		if (view.length && view[0].kind !== 'obs' && view[0].head && view.length > 1)
			view = [view[0].head].concat (view.slice (1));
	}
	rendered = view;
	listEl.innerHTML = view.length
		? view.map (r => rowHtml (r, !firstLoad && !shown.has (r.key))).join ('')
		: '<p class="empty-hint">no observations this night</p>';
	for (const r of rows)
		shown.add (r.key);
	firstLoad = false;
	if (mode !== 'fit')
		return;
	// the list must not scroll: one row less while it still does
	while (rendered.length > 1 && listEl.scrollHeight > listEl.clientHeight + 1) {
		rendered = rendered.length > 2 && rendered[0].kind === 'obs' && rendered[1].head === rendered[0]
			? [rendered[0]].concat (rendered.slice (2))
			: rendered.slice (1);
		listEl.innerHTML = rendered.map (r => rowHtml (r, false)).join ('');
	}
}

async function load () {
	clearTimeout (refreshTimer);
	try {
		const res = await fetch (`api/db/nightreport?${nightParams ()}`, { cache: 'no-store' });
		if (!res.ok)
			throw new Error (`api/db/nightreport -> ${res.status}`);
		report = await res.json ();
		rangeEl.textContent = `${utText (report.from)} - ${utText (report.to)}`;
		connEl.className = 'conn-status conn-open';
		connEl.textContent = `updated ${new Date ().toISOString ().slice (11, 19)} UT`;
		render ();
	} catch (e) {
		connEl.className = 'conn-status conn-closed';
		connEl.textContent = 'no answer';
	}
	refreshTimer = setTimeout (load, document.hidden ? REFRESH_MS * 4 : REFRESH_MS);
}

// --- preview under the mouse, enlarged on a click ------------------------

// the image whose time the event is on, or null
function imageAt (ev) {
	const name = ev.target.closest ('.nr-name');
	const row = name && name.closest ('.nr-row');
	const r = row && rendered.find (x => x.key === row.dataset.key);
	return r && r.kind === 'img' && r.im.previewPath ? r.im : null;
}

listEl.addEventListener ('mouseover', ev => {
	const im = hoverOnEl.checked ? imageAt (ev) : null;
	if (!im) {
		hoverEl.hidden = true;
		return;
	}
	const src = `preview/${encPath (im.previewPath)}?ps=${HOVER_SIZE}&cal=${calibrated () ? 1 : 0}`;
	const img = hoverEl.querySelector ('img');
	if (img.getAttribute ('src') !== src)
		img.src = src;
	hoverEl.querySelector ('.nr-hover-text').textContent = im.file;
	hoverEl.hidden = false;
});

listEl.addEventListener ('mousemove', ev => {
	if (hoverEl.hidden)
		return;
	// beside the mouse, inside the window
	const w = hoverEl.offsetWidth, h = hoverEl.offsetHeight;
	let x = ev.clientX + 18, y = ev.clientY + 18;
	if (x + w > window.innerWidth - 8)
		x = ev.clientX - w - 18;
	if (y + h > window.innerHeight - 8)
		y = Math.max (8, window.innerHeight - h - 8);
	hoverEl.style.left = x + 'px';
	hoverEl.style.top = y + 'px';
});

listEl.addEventListener ('mouseleave', () => {
	hoverEl.hidden = true;
});

listEl.addEventListener ('click', ev => {
	const im = imageAt (ev);
	if (!im)
		return;
	hoverEl.hidden = true;
	openLightboxFetch (`preview/${encPath (im.previewPath)}?ps=${BIG_SIZE}&cal=${calibrated () ? 1 : 0}`,
		im.file, utText (im.time), 'X-RTS2-Calibration');
});

// --- controls -------------------------------------------------------------

function saveShow () {
	try {
		localStorage.setItem ('nightreportShow', showMode ());
		localStorage.setItem ('nightreportTail', tailEl.value);
	} catch (e) {
	}
}

hoverOnEl.addEventListener ('change', () => {
	hoverEl.hidden = true;
	try {
		localStorage.setItem ('nightreportHover', hoverOnEl.checked ? '1' : '0');
	} catch (e) {
	}
});

document.querySelectorAll ('input[name="nr-show"]').forEach (r => r.addEventListener ('change', () => {
	saveShow ();
	render ();
}));
tailEl.addEventListener ('input', () => {
	document.querySelector ('input[name="nr-show"][value="tail"]').checked = true;
	saveShow ();
	render ();
});
nightEl.addEventListener ('change', () => {
	firstLoad = true;
	shown.clear ();
	load ();
});
document.getElementById ('nr-tonight').addEventListener ('click', async () => {
	nightEl.value = await currentNight ();
	firstLoad = true;
	shown.clear ();
	load ();
});
window.addEventListener ('resize', () => {
	if (showMode () === 'fit')
		render ();
});

(async function init () {
	try {
		const mode = localStorage.getItem ('nightreportShow');
		const tail = localStorage.getItem ('nightreportTail');
		hoverOnEl.checked = localStorage.getItem ('nightreportHover') !== '0';
		if (tail)
			tailEl.value = tail;
		const radio = mode && document.querySelector (`input[name="nr-show"][value="${mode}"]`);
		if (radio)
			radio.checked = true;
	} catch (e) {
	}
	try {
		nightEl.value = await currentNight ();
	} catch (e) {
		nightEl.value = new Date ().toISOString ().slice (0, 10);
	}
	load ();
}) ();
