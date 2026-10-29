// RTS2 devices page (devices.html) - vanilla JS, no build step, no
// external dependencies. Loads initial state via /api/getall, then applies
// live updates pushed over /ws instead of polling - see web/STATUS.md task
// 8. The night/image browser is on index.html (app.js).

const state = {};				 // { deviceName: { valueName: value } }
const devicesEl = document.getElementById('devices');
const connStatusEl = document.getElementById('conn-status');
const messagesEl = document.getElementById('messages');
const cmdDeviceEl = document.getElementById('cmd-device');
const cmdResultEl = document.getElementById('command-result');
const rawCmdDeviceEl = document.getElementById ('rawcmd-device');
const rawCmdResultEl = document.getElementById ('rawcmd-result');

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
	// a device that turns up re-renders everything: keep what is selected
	fillDeviceSelect (cmdDeviceEl, devices);
	// commands also go to centrald (on, standby, off, ...), as in rts2-mon;
	// getall lists it among the devices, but not before it has answered
	fillDeviceSelect (rawCmdDeviceEl, devices.includes ('centrald') ? devices : ['centrald', ...devices]);
}

function fillDeviceSelect (select, devices) {
	const selected = select.value;
	select.innerHTML = devices.map (d => `<option value="${escapeHtml (d)}">${escapeHtml (d)}</option>`).join ('');
	if (devices.includes (selected))
		select.value = selected;
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
	// oldest first; each goes on top, so the list is newest first, as
	// the messages pushed later are
	const msgs = await res.json ();
	messagesEl.innerHTML = '';
	for (const m of msgs.slice (-MAX_MESSAGES))
		prependMessage (m);
}

const MESSAGE_LEVELS = { 1: 'error', 2: 'warning', 4: 'info', 8: 'debug' };

// Newest first: a message goes on top, the oldest drops off the bottom.
// Each once: it may come pushed and polled both.
const shownMessages = new Set ();

function prependMessage (m) {
	const key = `${m.time}\t${m.device}\t${m.text}`;
	if (shownMessages.has (key))
		return;
	shownMessages.add (key);
	const li = document.createElement ('li');
	const level = MESSAGE_LEVELS[m.type & 0x0f] || 'info';
	li.className = 'level-' + level;
	const time = new Date (m.time * 1000).toLocaleTimeString ();
	li.innerHTML = `<span class="msg-time">${time}</span><span class="msg-device">${escapeHtml (m.device)}</span><span class="msg-text">${escapeHtml (m.text)}</span>`;
	messagesEl.insertBefore (li, messagesEl.firstChild);

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
		stopPolling ();
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
		} else if (msg.event === 'message') {
			// State changes are status.html's; shown here as made-up
			// messages they were gone at the next reload.
			prependMessage (msg);
		}
	};

	// Without the push (the sites' Apache passes no WebSocket unless its
	// ProxyPass has upgrade=websocket), the page polls instead, and tries
	// the push again now and then.
	ws.onclose = () => {
		startPolling ();
		setTimeout (connectWs, reconnectDelay);
		reconnectDelay = Math.min (reconnectDelay * 2, MAX_RECONNECT_DELAY);
	};

	ws.onerror = () => {
		ws.close ();
	};
}

// --- polling, while there is no push ---------------------------------------

const POLL_MS = 2000;
let pollTimer = null;
let polling = false;

function startPolling () {
	if (polling)
		return;
	polling = true;
	poll ();
}

function stopPolling () {
	polling = false;
	clearTimeout (pollTimer);
}

// The values that changed since the last poll (updateValue () flashes
// them, as for a push), and the messages not shown yet.
async function poll () {
	try {
		const [vr, mr] = await Promise.all ([fetch ('api/getall', { cache: 'no-store' }), fetch ('api/messages', { cache: 'no-store' })]);
		if (!vr.ok)
			throw new Error (`api/getall -> ${vr.status}`);
		const data = await vr.json ();
		if (!polling)
			return;
		for (const device of Object.keys (data))
			for (const name of Object.keys (data[device]))
				if (!state[device] || JSON.stringify (state[device][name]) !== JSON.stringify (data[device][name]))
					updateValue (device, name, data[device][name]);
		if (mr.ok)
			for (const m of (await mr.json ()).slice (-MAX_MESSAGES))
				prependMessage (m);
		setConnStatus ('open', 'polled');
	} catch (e) {
		setConnStatus ('closed', 'no answer');
	}
	if (polling)
		pollTimer = setTimeout (poll, document.hidden ? POLL_MS * 5 : POLL_MS);
}

// --- Command form ----------------------------------------------------------

// A raw command line to a device or centrald ("stop", "killall", ...),
// as rts2-mon's command line sends it. api/cmd answers once the device
// has: ok, or its status and error text; after 30 s without an answer,
// rts2-httpd gives up waiting.
document.getElementById ('rawcmd-form').addEventListener ('submit', async (ev) => {
	ev.preventDefault ();
	const device = rawCmdDeviceEl.value;
	const command = document.getElementById ('rawcmd-text').value.trim ();
	if (!device || !command) {
		rawCmdResultEl.className = 'err';
		rawCmdResultEl.textContent = 'device and command are required';
		return;
	}
	rawCmdResultEl.className = '';
	rawCmdResultEl.textContent = `${device}: ${command} - waiting for the answer…`;
	try {
		// A 401 brings up the browser's own login, as for set below.
		const res = await fetch (`api/cmd?d=${encodeURIComponent (device)}&c=${encodeURIComponent (command)}`);
		const text = await res.text ();
		let answer = null;
		try {
			answer = JSON.parse (text);
		} catch (e) {
		}
		if (res.ok && answer && answer.ok) {
			rawCmdResultEl.className = 'ok';
			rawCmdResultEl.textContent = `${device}: ${command} - OK`;
		} else if (res.ok && answer) {
			rawCmdResultEl.className = 'err';
			rawCmdResultEl.textContent = `${device}: ${command} - failed (${answer.status})${answer.error ? ': ' + answer.error : ''}`;
		} else {
			rawCmdResultEl.className = 'err';
			rawCmdResultEl.textContent = `${device}: ${command} - ${res.status}: ${answer && answer.error ? answer.error : text}`;
		}
	} catch (e) {
		rawCmdResultEl.className = 'err';
		rawCmdResultEl.textContent = `${device}: ${command} - ${e}`;
	}
});

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
