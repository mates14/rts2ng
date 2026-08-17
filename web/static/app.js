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
	const res = await fetch ('/api/getall');
	if (!res.ok)
		throw new Error (`GET /api/getall -> ${res.status}`);
	const data = await res.json ();
	// /api/getall's shape is {"device": {"valueName": value, ...}, ...} -
	// flat per device, no extra wrapper - use it directly as state.
	for (const device of Object.keys (data))
		state[device] = data[device];
	renderAll ();
}

async function loadRecentMessages () {
	const res = await fetch ('/api/messages');
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
	ws = new WebSocket (`${proto}//${location.host}/ws`);

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

	const url = `/api/${op}?d=${encodeURIComponent (device)}&n=${encodeURIComponent (name)}&v=${encodeURIComponent (value)}`;
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

// --- Init --------------------------------------------------------------

(async function init () {
	try {
		await loadInitialState ();
	} catch (e) {
		devicesEl.innerHTML = `<p class="empty-hint">failed to load initial state: ${escapeHtml (String (e))}</p>`;
	}
	await loadRecentMessages ();
	connectWs ();
}) ();
