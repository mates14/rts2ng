// RTS2 image archive (index.html) - vanilla JS, no build step, no
// external dependencies. The night/image browser; the device values,
// commands and messages are on devices.html (devices.js).

function escapeHtml (s) {
	return String (s).replace (/[&<>"']/g, c => ({
		'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
	}[c]));
}

// --- Images / night browser (STATUS.md task 7's nights/images endpoints) --
//
// Hierarchical drill-down (year -> month -> day) backed by
// /api/db/nights, landing on a specific night's observation list
// (/api/db/night) and thumbnail grid (/api/db/images +
// /preview/<previewPath>). Only present when this build has
// WEB_HAVE_DB (a non-DB build's /api/db/* 404s, handled below by just
// hiding the panel rather than showing a broken one).

const imagesBreadcrumbEl = document.getElementById ('images-breadcrumb');
const imagesContentEl = document.getElementById ('images-content');
const imagesPanelEl = document.getElementById ('images-panel');

const MONTH_NAMES = ['', 'Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];

function formatDuration (secs) {
	if (secs === null || secs === undefined || Number.isNaN (secs))
		return '—';
	const h = Math.floor (secs / 3600);
	const m = Math.round ((secs % 3600) / 60);
	return h > 0 ? `${h}h${m}m` : `${m}m`;
}

function renderBreadcrumb (year, month, day) {
	const parts = [];
	parts.push (`<a data-y="-1" data-m="-1" data-d="-1">all nights</a>`);
	if (year > 0) {
		parts.push ('<span class="sep">/</span>');
		parts.push (`<a data-y="${year}" data-m="-1" data-d="-1">${year}</a>`);
	}
	if (month > 0) {
		parts.push ('<span class="sep">/</span>');
		parts.push (`<a data-y="${year}" data-m="${month}" data-d="-1">${MONTH_NAMES[month]}</a>`);
	}
	if (day > 0) {
		parts.push ('<span class="sep">/</span>');
		parts.push (`<span>${day}</span>`);
	}
	imagesBreadcrumbEl.innerHTML = parts.join (' ');
	for (const a of imagesBreadcrumbEl.querySelectorAll ('a'))
		a.addEventListener ('click', () => loadNightsLevel (Number (a.dataset.y), Number (a.dataset.m), Number (a.dataset.d)));
}

async function loadNightsLevel (year, month, day) {
	renderBreadcrumb (year, month, day);
	imagesContentEl.innerHTML = '<p class="empty-hint">loading&hellip;</p>';
	try {
		const res = await fetch (`api/db/nights?year=${year}&month=${month}&day=${day}`);
		if (res.status === 404) {
			// Non-DB build (WEB_WITH_DB=OFF) - /api/db/* doesn't exist at
			// all. Hide the panel rather than show a permanently-broken one.
			imagesPanelEl.style.display = 'none';
			return;
		}
		if (!res.ok)
			throw new Error (`GET api/db/nights -> ${res.status}`);
		const data = await res.json ();

		if (data.entries.length === 0) {
			imagesContentEl.innerHTML = '<p class="empty-hint">no observations at this level</p>';
			return;
		}

		const cells = data.entries.map (e => {
			const label = data.level === 'month' ? MONTH_NAMES[e.key] : e.key;
			return `
				<div class="drilldown-cell" data-key="${e.key}">
					<div class="cell-key">${label}</div>
					<div class="cell-stats">${e.observations} obs · ${e.images} img · ${formatDuration (e.timeOnSky)}</div>
				</div>
			`;
		}).join ('');
		imagesContentEl.innerHTML = `<div class="drilldown-grid">${cells}</div>`;

		for (const cell of imagesContentEl.querySelectorAll ('.drilldown-cell')) {
			cell.addEventListener ('click', () => {
				const key = Number (cell.dataset.key);
				if (data.level === 'year')
					loadNightsLevel (key, -1, -1);
				else if (data.level === 'month')
					loadNightsLevel (year, key, -1);
				else if (data.level === 'day')
					loadNightDetail (year, month, key);
			});
		}
	} catch (e) {
		imagesContentEl.innerHTML = `<p class="empty-hint">images/nights browser not available: ${escapeHtml (String (e))}</p>`;
	}
}

// --- One night: its observations and a page of its images --------------
//
// The images come a page at a time (api/db/images with limit=), filtered
// by target (a click on a target in the observations), cameras and
// photometric filters (checkboxes, with the counts the other two filters
// leave). A click on a thumbnail opens it big (lightbox.js, with zoom);
// images are selected with their checkboxes - the selection survives
// paging and filtering - and downloaded as one tar (POST api/download), or
// one at a time as FITS (fits/<path>).

const PAGE_SIZES = [24, 48, 96, 192];
const THUMB_SIZE = 200;				// preview/?ps= of a thumbnail
const BIG_SIZE = 2048;				// and of the enlarged image

const archive = {
	year: 0, month: 0, day: 0,
	target: -1,						// -1: all targets
	targetNames: new Map (),
	cameras: null,					// Set of camera names, null: all
	filters: null,					// Set of filter names, null: all
	offset: 0,
	limit: 48,
	total: 0,
	selected: new Set (),			// previewPaths
	seq: 0,
	calibrated: true,				// dark/flat corrected previews (cal=1)
	newestFirst: false,				// order=desc
};

try {
	const saved = Number (localStorage.getItem ('archivePageSize'));
	if (PAGE_SIZES.includes (saved))
		archive.limit = saved;
	archive.calibrated = localStorage.getItem ('archiveCalibrated') !== '0';
	archive.newestFirst = localStorage.getItem ('archiveNewestFirst') === '1';
} catch (e) {
}

function encPath (p) {
	return p.split ('/').map (encodeURIComponent).join ('/');
}

function utTime (t) {
	return new Date (t * 1000).toISOString ().slice (11, 19);
}

// An empty set (none ticked) is left out: the answer then still has its
// facet, to draw the checkboxes from; the page shows no images.
function imagesQuery (offset, limit) {
	let q = `api/db/images?year=${archive.year}&month=${archive.month}&day=${archive.day}&limit=${limit}&offset=${offset}${archive.newestFirst ? '&order=desc' : ''}`;
	if (archive.target >= 0)
		q += `&tar=${archive.target}`;
	if (archive.cameras && archive.cameras.size)
		q += `&cameras=${encodeURIComponent ([...archive.cameras].join (','))}`;
	if (archive.filters && archive.filters.size)
		q += `&filters=${encodeURIComponent ([...archive.filters].join (','))}`;
	return q;
}

function noneTicked () {
	return (archive.cameras && archive.cameras.size === 0) || (archive.filters && archive.filters.size === 0);
}

async function loadNightDetail (year, month, day) {
	renderBreadcrumb (year, month, day);
	Object.assign (archive, { year, month, day, target: -1, cameras: null, filters: null, offset: 0 });
	archive.selected.clear ();
	imagesContentEl.innerHTML = '<p class="empty-hint">loading&hellip;</p>';
	try {
		const obsRes = await fetch (`api/db/night?year=${year}&month=${month}&day=${day}`);
		if (!obsRes.ok)
			throw new Error (`GET api/db/night -> ${obsRes.status}`);
		const obs = await obsRes.json ();
		for (const o of obs)
			archive.targetNames.set (o.targetId, o.targetName);

		const obsRows = obs.map (o => `
			<tr data-tar="${o.targetId}">
				<td>${o.id}</td>
				<td><a class="tar-filter" data-tar="${o.targetId}" title="show only this target's images">${escapeHtml (o.targetName)}</a> (#${o.targetId})</td>
				<td>${new Date (o.start * 1000).toLocaleTimeString ()}</td>
				<td>${o.end ? new Date (o.end * 1000).toLocaleTimeString () : '—'}</td>
				<td>${o.goodImages}/${o.images}</td>
				<td>${formatDuration (o.timeOnSky)}</td>
			</tr>
		`).join ('');
		const obsTable = obs.length === 0 ? '<p class="empty-hint">no observations this night</p>' : `
			<table class="obs-table">
				<thead><tr><th>Obs</th><th>Target</th><th>Start</th><th>End</th><th>Good/all</th><th>On sky</th></tr></thead>
				<tbody>${obsRows}</tbody>
			</table>
		`;
		const pager = `
			<div class="arch-bar">
				<span class="arch-count"></span>
				<span class="arch-pager">
					<button type="button" class="arch-first" title="first page">&laquo;</button>
					<button type="button" class="arch-prev" title="previous page">&lsaquo;</button>
					<label>page <input type="number" class="arch-pageno" min="1" value="1" title="page number - Enter to go there"> of <span class="arch-pages">1</span></label>
					<button type="button" class="arch-next" title="next page">&rsaquo;</button>
					<button type="button" class="arch-last" title="last page">&raquo;</button>
				</span>
				<label>per page <select class="arch-limit">${PAGE_SIZES.map (n => `<option${n === archive.limit ? ' selected' : ''}>${n}</option>`).join ('')}</select></label>
				<label><select class="arch-order">
					<option value="asc"${archive.newestFirst ? '' : ' selected'}>oldest first</option>
					<option value="desc"${archive.newestFirst ? ' selected' : ''}>newest first</option>
				</select></label>
			</div>`;
		imagesContentEl.innerHTML = obsTable + `
			<div class="arch-filters">
				<div class="arch-row"><span class="arch-label">Target</span><span id="arch-target"></span></div>
				<div class="arch-row"><span class="arch-label">Cameras</span><span id="arch-cameras"></span></div>
				<div class="arch-row"><span class="arch-label">Filters</span><span id="arch-filters"></span></div>
				<div class="arch-row"><span class="arch-label">Previews</span><span class="arch-previews">
					<label><input type="radio" name="arch-cal" value="1"${archive.calibrated ? ' checked' : ''}> dark/flat corrected</label>
					<label><input type="radio" name="arch-cal" value="0"${archive.calibrated ? '' : ' checked'}> raw</label>
					<span class="arch-hint">corrected with the site pipeline's master frames, where they are there; the enlarged image says what was applied</span>
				</span></div>
			</div>
			<div class="arch-selbar">
				<span id="arch-selcount">0 selected</span>
				<button type="button" id="arch-sel-page">select this page</button>
				<button type="button" id="arch-sel-all">select all matching</button>
				<button type="button" id="arch-sel-clear">clear the selection</button>
				<button type="button" id="arch-download" class="primary" disabled>download selected (.tar)</button>
				<span class="arch-purge">
					<button type="button" id="arch-purge" title="delete all cached previews; they are made again when shown">purge preview cache</button>
					<span id="arch-purge-result"></span>
				</span>
			</div>
			${pager}
			<div id="arch-grid" class="thumb-grid"></div>
			${pager}
			<form id="arch-download-form" method="post" hidden><input type="hidden" name="paths"></form>`;
		wireArchiveControls ();
		await loadImagesPage ();
	} catch (e) {
		imagesContentEl.innerHTML = `<p class="empty-hint">failed to load night detail: ${escapeHtml (String (e))}</p>`;
	}
}

function wireArchiveControls () {
	imagesContentEl.querySelectorAll ('.tar-filter').forEach (a => a.addEventListener ('click', () => {
		const tar = Number (a.dataset.tar);
		// a second click on the shown target shows all again
		setTarget (archive.target === tar ? -1 : tar);
	}));
	imagesContentEl.querySelectorAll ('.arch-first').forEach (b => b.addEventListener ('click', () => goPage (0)));
	imagesContentEl.querySelectorAll ('.arch-prev').forEach (b => b.addEventListener ('click', () => goPage (archive.offset - archive.limit)));
	imagesContentEl.querySelectorAll ('.arch-next').forEach (b => b.addEventListener ('click', () => goPage (archive.offset + archive.limit)));
	imagesContentEl.querySelectorAll ('.arch-last').forEach (b => b.addEventListener ('click', () => goPage (archive.total - 1)));
	// a page by its number: Enter, or leaving the field
	imagesContentEl.querySelectorAll ('.arch-pageno').forEach (input => {
		const go = () => {
			const n = parseInt (input.value, 10);
			if (Number.isFinite (n))
				goPage ((n - 1) * archive.limit);
		};
		input.addEventListener ('keydown', ev => {
			if (ev.key === 'Enter')
				go ();
		});
		input.addEventListener ('change', go);
	});
	imagesContentEl.querySelectorAll ('.arch-order').forEach (sel => sel.addEventListener ('change', () => {
		archive.newestFirst = sel.value === 'desc';
		try {
			localStorage.setItem ('archiveNewestFirst', archive.newestFirst ? '1' : '0');
		} catch (e) {
		}
		imagesContentEl.querySelectorAll ('.arch-order').forEach (s => { s.value = sel.value; });
		goPage (0);
	}));
	imagesContentEl.querySelectorAll ('.arch-limit').forEach (sel => sel.addEventListener ('change', () => {
		archive.limit = Number (sel.value);
		try {
			localStorage.setItem ('archivePageSize', String (archive.limit));
		} catch (e) {
		}
		imagesContentEl.querySelectorAll ('.arch-limit').forEach (s => { s.value = sel.value; });
		goPage (0);
	}));
	document.getElementById ('arch-sel-page').addEventListener ('click', () => {
		document.querySelectorAll ('#arch-grid .thumb-cell[data-path]').forEach (c => archive.selected.add (c.dataset.path));
		showSelection ();
	});
	document.getElementById ('arch-sel-all').addEventListener ('click', selectAllMatching);
	document.getElementById ('arch-sel-clear').addEventListener ('click', () => {
		archive.selected.clear ();
		showSelection ();
	});
	document.getElementById ('arch-download').addEventListener ('click', downloadSelected);
	document.getElementById ('arch-purge').addEventListener ('click', purgeCache);
	imagesContentEl.querySelectorAll ('input[name="arch-cal"]').forEach (r => r.addEventListener ('change', () => {
		archive.calibrated = r.value === '1';
		try {
			localStorage.setItem ('archiveCalibrated', r.value);
		} catch (e) {
		}
		loadImagesPage ();
	}));
}

function setTarget (tar) {
	archive.target = tar;
	goPage (0);
}

function goPage (offset) {
	archive.offset = Math.max (0, Math.min (offset, Math.max (0, archive.total - 1)));
	archive.offset -= archive.offset % archive.limit;
	loadImagesPage ();
}

async function loadImagesPage () {
	const seq = ++archive.seq;
	const grid = document.getElementById ('arch-grid');
	if (!grid)
		return;
	grid.classList.add ('loading');
	try {
		const res = await fetch (imagesQuery (archive.offset, archive.limit));
		if (!res.ok)
			throw new Error (`GET api/db/images -> ${res.status}`);
		const page = await res.json ();
		if (seq !== archive.seq)
			return;
		renderFacets (page.facets);
		if (noneTicked ()) {
			archive.total = 0;
			grid.innerHTML = '<p class="empty-hint">no camera or no filter selected</p>';
		} else {
			archive.total = page.total;
			renderThumbs (page.images);
		}
		showPager ();
		showSelection ();
	} catch (e) {
		grid.innerHTML = `<p class="empty-hint">failed to load images: ${escapeHtml (String (e))}</p>`;
	} finally {
		grid.classList.remove ('loading');
	}
}

function renderFacets (facets) {
	for (const t of facets.targets)
		archive.targetNames.set (t.id, t.name);
	const tEl = document.getElementById ('arch-target');
	tEl.innerHTML = archive.target < 0
		? '<span class="arch-hint">all targets - click a target in the observations to show only its images</span>'
		: `<span class="chip">${escapeHtml (archive.targetNames.get (archive.target) || '#' + archive.target)} (#${archive.target})
			<button type="button" title="all targets" class="chip-x">&times;</button></span>`;
	const x = tEl.querySelector ('.chip-x');
	if (x)
		x.addEventListener ('click', () => setTarget (-1));
	document.querySelectorAll ('.obs-table tr[data-tar]').forEach (tr => tr.classList.toggle ('active', Number (tr.dataset.tar) === archive.target));

	const boxes = (el, list, chosen, key) => {
		el.innerHTML = list.map (f => `<label class="facet"><input type="checkbox" value="${escapeHtml (f.name)}"${!chosen || chosen.has (f.name) ? ' checked' : ''}>
			${f.name === '' ? '<i>none</i>' : escapeHtml (f.name)} <span class="facet-count">${f.count}</span></label>`).join ('')
			+ (list.length > 1 ? ' <button type="button" class="facet-all">all</button><button type="button" class="facet-none">none</button>' : '');
		el.querySelectorAll ('input').forEach (box => box.addEventListener ('change', () => {
			const ticked = [...el.querySelectorAll ('input')].filter (b => b.checked).map (b => b.value);
			archive[key] = ticked.length === list.length ? null : new Set (ticked);
			goPage (0);
		}));
		const all = el.querySelector ('.facet-all');
		if (all)
			all.addEventListener ('click', () => {
				archive[key] = null;
				goPage (0);
			});
		// none: then tick the ones wanted
		const none = el.querySelector ('.facet-none');
		if (none)
			none.addEventListener ('click', () => {
				archive[key] = new Set ();
				goPage (0);
			});
	};
	boxes (document.getElementById ('arch-cameras'), facets.cameras, archive.cameras, 'cameras');
	boxes (document.getElementById ('arch-filters'), facets.filters, archive.filters, 'filters');
}

function renderThumbs (images) {
	const grid = document.getElementById ('arch-grid');
	if (images.length === 0) {
		grid.innerHTML = '<p class="empty-hint">no images</p>';
		return;
	}
	grid.innerHTML = images.map (im => {
		const target = im.targetName || '#' + im.targetId;
		const info = `${utTime (im.exposureStart)} UT · ${im.exposure} s · ${im.filter || 'no filter'}${im.binning ? ' · ' + im.binning : ''} · ${im.cameraName}`;
		const p = im.previewPath;
		return `
			<div class="thumb-cell${p && archive.selected.has (p) ? ' selected' : ''}"${p ? ` data-path="${escapeHtml (p)}"` : ''}
				data-title="${escapeHtml (im.file)}" data-info="${escapeHtml (info + ' · ' + target)}">
				${p ? `<img src="preview/${encPath (p)}?ps=${THUMB_SIZE}&cal=${archive.calibrated ? 1 : 0}" loading="lazy" alt="">`
					: '<div class="thumb-noimg">not under the images directory</div>'}
				<div class="thumb-bottom">
					<div class="thumb-caption">
						<div class="thumb-file">${escapeHtml (im.file)}</div>
						<div>${escapeHtml (info)}</div>
						<div class="thumb-target">${escapeHtml (target)}</div>
					</div>
					${p ? `<div class="thumb-actions">
						<input type="checkbox" class="thumb-select" title="select for download"${archive.selected.has (p) ? ' checked' : ''}>
						<a class="thumb-dl" href="fits/${encPath (p)}" title="open this FITS file">&#x2197;</a>
					</div>` : ''}
				</div>
			</div>`;
	}).join ('');
	grid.querySelectorAll ('.thumb-cell[data-path]').forEach (cell => {
		cell.querySelector ('img').addEventListener ('click', () =>
			openLightboxFetch (`preview/${encPath (cell.dataset.path)}?ps=${BIG_SIZE}&cal=${archive.calibrated ? 1 : 0}`,
				cell.dataset.title, cell.dataset.info, 'X-RTS2-Calibration'));
		cell.querySelector ('.thumb-select').addEventListener ('change', ev => {
			if (ev.target.checked)
				archive.selected.add (cell.dataset.path);
			else
				archive.selected.delete (cell.dataset.path);
			showSelection ();
		});
	});
}

function showPager () {
	const pages = Math.max (1, Math.ceil (archive.total / archive.limit));
	const page = Math.floor (archive.offset / archive.limit) + 1;
	const first = archive.total ? archive.offset + 1 : 0;
	const last = Math.min (archive.total, archive.offset + archive.limit);
	imagesContentEl.querySelectorAll ('.arch-count').forEach (el => { el.textContent = `images ${first}-${last} of ${archive.total}`; });
	imagesContentEl.querySelectorAll ('.arch-pageno').forEach (el => {
		el.value = page;
		el.max = pages;
	});
	imagesContentEl.querySelectorAll ('.arch-pages').forEach (el => { el.textContent = pages; });
	imagesContentEl.querySelectorAll ('.arch-first, .arch-prev').forEach (b => { b.disabled = page <= 1; });
	imagesContentEl.querySelectorAll ('.arch-next, .arch-last').forEach (b => { b.disabled = page >= pages; });
}

function showSelection () {
	const n = archive.selected.size;
	document.getElementById ('arch-selcount').textContent = `${n} selected`;
	document.getElementById ('arch-download').disabled = n === 0;
	document.querySelectorAll ('#arch-grid .thumb-cell[data-path]').forEach (cell => {
		const on = archive.selected.has (cell.dataset.path);
		cell.classList.toggle ('selected', on);
		cell.querySelector ('.thumb-select').checked = on;
	});
}

async function selectAllMatching () {
	if (noneTicked ())
		return;
	const b = document.getElementById ('arch-sel-all');
	b.disabled = true;
	try {
		const res = await fetch (imagesQuery (0, 10000));
		if (res.ok)
			for (const im of (await res.json ()).images)
				if (im.previewPath)
					archive.selected.add (im.previewPath);
	} finally {
		b.disabled = false;
		showSelection ();
	}
}

// The whole preview cache (raw and corrected, every night) is deleted;
// the previews are made again when shown. A 401 brings up the browser's
// own login, as for the commands.
async function purgeCache () {
	if (!confirm ('Delete all cached previews (raw and corrected, all nights)?\nThey are made again when shown, which takes a while the first time.'))
		return;
	const b = document.getElementById ('arch-purge');
	const out = document.getElementById ('arch-purge-result');
	b.disabled = true;
	out.className = '';
	out.textContent = 'purging…';
	try {
		const res = await fetch ('api/purge-cache', { cache: 'no-store' });
		const answer = await res.json ().catch (() => ({}));
		if (!res.ok)
			throw new Error (answer.error || `${res.status}`);
		out.className = 'ok';
		out.textContent = `${answer.files} files, ${(answer.bytes / 1048576).toFixed (1)} MB deleted`;
		loadImagesPage ();
	} catch (e) {
		out.className = 'err';
		out.textContent = `not purged: ${e.message}`;
	} finally {
		b.disabled = false;
	}
}

// A form POST, so that the browser saves the tar as it comes, however
// large, instead of holding it in memory first.
function downloadSelected () {
	const form = document.getElementById ('arch-download-form');
	const night = `${archive.year}${String (archive.month).padStart (2, '0')}${String (archive.day).padStart (2, '0')}`;
	form.action = `api/download?name=rts2-${night}-${archive.selected.size}`;
	form.elements.paths.value = [...archive.selected].join ('\n');
	form.submit ();
}

// Lands on tonight's detail directly rather than the top-level nights
// drill-down (dbNightsSummary() with no year/month/day is an unbounded
// aggregate over every observation/image ever recorded - found live
// against lascaux's real archive to be what made the dashboard's
// initial load slow. api/db/current-night is a separate, DB-free
// endpoint (just today's date run through the same night-boundary
// math), so this costs nothing extra before jumping straight to the
// one night an operator actually wants to see on open. The "all nights"
// breadcrumb link still reaches the full drill-down for anyone who
// wants to browse history - that's an explicit action, not the default.
async function loadDefaultImagesView () {
	try {
		const res = await fetch ('api/db/current-night');
		if (res.status === 404) {
			imagesPanelEl.style.display = 'none';
			return;
		}
		if (!res.ok)
			throw new Error (`GET api/db/current-night -> ${res.status}`);
		const { year, month, day } = await res.json ();
		await loadNightDetail (year, month, day);
	} catch (e) {
		// Fall back to the drill-down rather than showing nothing.
		await loadNightsLevel (-1, -1, -1);
	}
}

// --- Init --------------------------------------------------------------

loadDefaultImagesView ();
