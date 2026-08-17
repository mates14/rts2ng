// RTS2 device monitor - vanilla JS, no build step, no external
// dependencies. Loads initial state via /api/getall, then applies live
// updates pushed over /ws instead of polling - see web/STATUS.md task 8.

const state = {};				 // { deviceName: { valueName: value } }
const devicesEl = document.getElementById('devices');
const connStatusEl = document.getElementById('conn-status');
const messagesEl = document.getElementById('messages');
const cmdDeviceEl = document.getElementById('cmd-device');
const cmdResultEl = document.getElementById('command-result');

const MAX_MESSAGES = 50;

// This page is loaded via a plain relative <script src="app.js">, so
// fetch('api/...') calls above already resolve correctly under any
// mount depth (root, or a reverse-proxied subpath like Apache's
// "ProxyPass /images http://localhost:8889" on lascaux - see
// STATUS.md). The one thing fetch()'s relative resolution can't do for
// us is the WebSocket URL, since `new WebSocket()` requires a full
// ws(s):// URL, not something the browser resolves relatively - this
// computes the same directory fetch() would've used.
function basePath () {
	return location.pathname.replace (/[^/]*$/, '');
}

function formatValue (v) {
	if (v === null || v === undefined)
		return '—';			 // em dash
	if (typeof v === 'number')
		return Number.isInteger (v) ? String (v) : v.toFixed (4).replace (/\.?0+$/, '');
	if (typeof v === 'boolean')
		return v ? 'true' : 'false';
	return String (v);
}

function deviceCardId (device) {
	return 'device-' + device.replace (/[^A-Za-z0-9_-]/g, '_');
}

function valueCellId (device, name) {
	return deviceCardId (device) + '-v-' + name.replace (/[^A-Za-z0-9_-]/g, '_');
}

function renderDevice (device, values) {
	const names = Object.keys (values).sort ();
	const rows = names.map (name =>
		`<tr><td class="value-name">${escapeHtml (name)}</td><td class="value-val" id="${valueCellId (device, name)}">${escapeHtml (formatValue (values[name]))}</td></tr>`
	).join ('');

	return `
		<article class="device-card" id="${deviceCardId (device)}">
			<h3>${escapeHtml (device)} <span class="value-count">${names.length} values</span></h3>
			<table><tbody>${rows || '<tr><td class="empty-hint" colspan="2">no values</td></tr>'}</tbody></table>
		</article>
	`;
}

function escapeHtml (s) {
	return String (s).replace (/[&<>"']/g, c => ({
		'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
	}[c]));
}

function renderAll () {
	const devices = Object.keys (state).sort ();
	devicesEl.innerHTML = devices.map (d => renderDevice (d, state[d])).join ('');
	cmdDeviceEl.innerHTML = devices.map (d => `<option value="${escapeHtml (d)}">${escapeHtml (d)}</option>`).join ('');
}

// Patches a single value in place (with a brief flash) rather than
// re-rendering the whole card - keeps live updates cheap and avoids
// losing focus/scroll position while values are streaming in.
function updateValue (device, name, value) {
	if (!state[device])
		state[device] = {};

	const isNewDevice = !(device in state) || !(name in state[device]);
	state[device][name] = value;

	const cell = document.getElementById (valueCellId (device, name));
	if (cell) {
		cell.textContent = formatValue (value);
		cell.classList.remove ('just-changed');
		// Force reflow so the animation restarts on rapid repeated updates.
		void cell.offsetWidth;
		cell.classList.add ('just-changed');
	} else {
		// A value or whole device we haven't rendered yet (e.g. a device
		// that connected after page load, or reconnected) - full
		// re-render is the simplest correct fix and rare enough not to
		// matter for performance.
		renderAll ();
	}
}

async function loadInitialState () {
	const res = await fetch ('api/getall');
	if (!res.ok)
		throw new Error (`GET api/getall -> ${res.status}`);
	const data = await res.json ();
	// /api/getall's shape is {"device": {"valueName": value, ...}, ...} -
	// flat per device, no extra wrapper - use it directly as state.
	for (const device of Object.keys (data))
		state[device] = data[device];
	renderAll ();
}

async function loadRecentMessages () {
	const res = await fetch ('api/messages');
	if (!res.ok)
		return;
	const msgs = await res.json ();
	messagesEl.innerHTML = '';
	for (const m of msgs.slice (-MAX_MESSAGES))
		prependMessage (m, false);
}

const MESSAGE_LEVELS = { 1: 'error', 2: 'warning', 4: 'info', 8: 'debug' };

function prependMessage (m, atTop) {
	const li = document.createElement ('li');
	const level = MESSAGE_LEVELS[m.type & 0x0f] || 'info';
	li.className = 'level-' + level;
	const time = new Date (m.time * 1000).toLocaleTimeString ();
	li.innerHTML = `<span class="msg-time">${time}</span><span class="msg-device">${escapeHtml (m.device)}</span><span class="msg-text">${escapeHtml (m.text)}</span>`;
	if (atTop && messagesEl.firstChild)
		messagesEl.insertBefore (li, messagesEl.firstChild);
	else
		messagesEl.appendChild (li);

	while (messagesEl.children.length > MAX_MESSAGES)
		messagesEl.removeChild (messagesEl.lastChild);
}

// --- WebSocket live push -------------------------------------------------

let ws = null;
let reconnectDelay = 1000;
const MAX_RECONNECT_DELAY = 30000;

function setConnStatus (cls, text) {
	connStatusEl.className = 'conn-status conn-' + cls;
	connStatusEl.textContent = text;
}

function connectWs () {
	const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
	ws = new WebSocket (`${proto}//${location.host}${basePath ()}ws`);

	ws.onopen = () => {
		setConnStatus ('open', 'live');
		reconnectDelay = 1000;
	};

	ws.onmessage = (ev) => {
		let msg;
		try {
			msg = JSON.parse (ev.data);
		} catch (e) {
			console.error ('bad WS message', ev.data);
			return;
		}

		if (msg.event === 'value') {
			const names = Object.keys (msg.v);
			for (const name of names)
				updateValue (msg.device, name, msg.v[name]);
		} else if (msg.event === 'state') {
			// No dedicated state row rendered yet (task 8 first pass) -
			// surface it as a message instead of dropping it silently.
			prependMessage ({ time: Date.now () / 1000, device: msg.device, type: 4, text: `state -> ${msg.statestring} (${msg.value})` }, true);
		} else if (msg.event === 'message') {
			prependMessage (msg, true);
		}
	};

	ws.onclose = () => {
		setConnStatus ('closed', 'reconnecting…');
		setTimeout (connectWs, reconnectDelay);
		reconnectDelay = Math.min (reconnectDelay * 2, MAX_RECONNECT_DELAY);
	};

	ws.onerror = () => {
		ws.close ();
	};
}

// --- Command form ----------------------------------------------------------

document.getElementById ('command-form').addEventListener ('submit', async (ev) => {
	ev.preventDefault ();
	const op = ev.submitter ? ev.submitter.dataset.op : 'set';
	const device = cmdDeviceEl.value;
	const name = document.getElementById ('cmd-name').value.trim ();
	const value = document.getElementById ('cmd-value').value;

	if (!device || !name) {
		cmdResultEl.className = 'err';
		cmdResultEl.textContent = 'device and variable name are required';
		return;
	}

	const url = `api/${op}?d=${encodeURIComponent (device)}&n=${encodeURIComponent (name)}&v=${encodeURIComponent (value)}`;
	try {
		// A 401 here triggers the browser's own native credential
		// prompt (WWW-Authenticate: Basic) - no credential handling
		// needed in this code at all, see web/STATUS.md task 6.
		const res = await fetch (url);
		const text = await res.text ();
		cmdResultEl.className = res.ok ? 'ok' : 'err';
		cmdResultEl.textContent = `${res.status}: ${text}`;
	} catch (e) {
		cmdResultEl.className = 'err';
		cmdResultEl.textContent = String (e);
	}
});

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

(async function init () {
	try {
		await loadInitialState ();
	} catch (e) {
		devicesEl.innerHTML = `<p class="empty-hint">failed to load initial state: ${escapeHtml (String (e))}</p>`;
	}
	await loadRecentMessages ();
	await loadDefaultImagesView ();
	connectWs ();
}) ();
