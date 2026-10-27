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

async function loadNightDetail (year, month, day) {
	renderBreadcrumb (year, month, day);
	imagesContentEl.innerHTML = '<p class="empty-hint">loading&hellip;</p>';
	try {
		const [obsRes, imgRes] = await Promise.all ([
			fetch (`api/db/night?year=${year}&month=${month}&day=${day}`),
			fetch (`api/db/images?year=${year}&month=${month}&day=${day}`)
		]);
		if (!obsRes.ok || !imgRes.ok)
			throw new Error (`GET api/db/night|images -> ${obsRes.status}/${imgRes.status}`);
		const obs = await obsRes.json ();
		const imgs = await imgRes.json ();

		const obsRows = obs.map (o => `
			<tr>
				<td>${o.id}</td>
				<td>${escapeHtml (o.targetName)} (#${o.targetId})</td>
				<td>${new Date (o.start * 1000).toLocaleTimeString ()}</td>
				<td>${o.end ? new Date (o.end * 1000).toLocaleTimeString () : '—'}</td>
				<td>${o.goodImages}/${o.images}</td>
				<td>${formatDuration (o.timeOnSky)}</td>
			</tr>
		`).join ('');
		const obsTable = obs.length === 0 ? '<p class="empty-hint">no observations this night</p>' : `
			<table>
				<thead><tr><th>Obs</th><th>Target</th><th>Start</th><th>End</th><th>Good/all</th><th>On sky</th></tr></thead>
				<tbody>${obsRows}</tbody>
			</table>
		`;

		const thumbs = imgs.map (im => {
			const inner = im.previewPath
				? `<img src="preview/${im.previewPath.split ('/').map (encodeURIComponent).join ('/')}?ps=140" loading="lazy" alt="">`
				: `<div class="thumb-noimg">no preview</div>`;
			return `
				<div class="thumb-cell">
					${inner}
					<div class="thumb-caption">${escapeHtml (im.targetName || ('#' + im.targetId))} · ${escapeHtml (im.cameraName || '')}</div>
				</div>
			`;
		}).join ('');
		const thumbGrid = imgs.length === 0 ? '<p class="empty-hint">no images this night</p>' : `<div class="thumb-grid">${thumbs}</div>`;

		imagesContentEl.innerHTML = obsTable + thumbGrid;
	} catch (e) {
		imagesContentEl.innerHTML = `<p class="empty-hint">failed to load night detail: ${escapeHtml (String (e))}</p>`;
	}
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
