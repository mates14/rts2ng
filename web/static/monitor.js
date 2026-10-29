// RTS2 monitor (monitor.html) - rts2-mon as a web page: the device list on
// the left, the selected device's values on the right (api/monitor: the
// texts rts2-mon prints, with its W, error/warning and FITS marks), the
// messages under them, a command line for the device and a status bar.
// "status" at the end of the list is rts2-mon's status item. Everything
// is polled over plain HTTP, so it works through the sites' Apache proxy.

const devListEl = document.getElementById ('mon-devlist');
const stateEl = document.getElementById ('mon-statestring');
const progressEl = document.getElementById ('mon-progress');
const valuesEl = document.querySelector ('#mon-values tbody');
const valuesTableEl = document.getElementById ('mon-values');
const statusTableEl = document.getElementById ('mon-status');
const descEl = document.getElementById ('mon-desc');
const filtersEl = document.getElementById ('mon-filters');
const messagesEl = document.getElementById ('mon-messages');
const cmdDevEl = document.getElementById ('mon-cmd-dev');
const cmdEl = document.getElementById ('mon-cmd');
const cmdResultEl = document.getElementById ('mon-cmd-result');
const connEl = document.getElementById ('mon-conn');
const masterEl = document.getElementById ('mon-master');
const clockEl = document.getElementById ('mon-clock');

const POLL_MS = 1000;
const POLL_HIDDEN_MS = 10000;
const MESSAGES_MS = 2000;
const MAX_MESSAGES = 300;
const STATUS_ITEM = 'status';		// rts2-mon's last list entry

const RTS2_VALUE_BOOL = 6;
const RTS2_VALUE_SELECTION = 7;
const RTS2_VALUE_MMAX = 0x20;		// value, min, max: only the value is set

let devices = [];					// the last api/status answer
let selected = decodeURIComponent (location.hash.slice (1)) || 'centrald';
let filterMode = 'all';
let showDebug = false;
let search = '';
const descriptions = new Map ();	// "device\tvalue" -> description
const selections = new Map ();		// "device\tvalue" -> option names
let editing = null;					// name of the value being edited
let lastOk = null;

// --- device list --------------------------------------------------------

function renderDevList () {
	const names = devices.map (d => d.name).concat ([STATUS_ITEM]);
	if (!names.includes (selected))
		selected = names[0];
	const html = names.map (name => {
		const dev = devices.find (d => d.name === name);
		const cls = ['mon-dev', dev ? 'level-' + dev.level : 'mon-status-item', name === selected ? 'selected' : ''].join (' ');
		return `<a href="#${encodeURIComponent (name)}" class="${cls}" data-name="${escapeHtml (name)}">${escapeHtml (name)}</a>`;
	}).join ('');
	if (devListEl.innerHTML !== html)
		devListEl.innerHTML = html;
}

function select (name) {
	if (name === selected)
		return;
	selected = name;
	editing = null;
	valuesEl.innerHTML = '';
	valueRows.clear ();
	descEl.innerHTML = '&nbsp;';
	history.replaceState (null, '', '#' + encodeURIComponent (name));
	renderDevList ();
	cmdDevEl.textContent = (name === STATUS_ITEM ? '' : name) + '>';
	cmdEl.disabled = name === STATUS_ITEM;
	pollDevice ();
}

devListEl.addEventListener ('click', ev => {
	const a = ev.target.closest ('a.mon-dev');
	if (!a)
		return;
	ev.preventDefault ();
	select (a.dataset.name);
});

// Up/Down walk the device list, as in rts2-mon, unless typing somewhere.
document.addEventListener ('keydown', ev => {
	if (ev.target.closest ('input, select, textarea') || ev.altKey || ev.ctrlKey || ev.metaKey)
		return;
	if (ev.key !== 'ArrowUp' && ev.key !== 'ArrowDown')
		return;
	const names = devices.map (d => d.name).concat ([STATUS_ITEM]);
	const i = names.indexOf (selected);
	const next = names[Math.max (0, Math.min (names.length - 1, i + (ev.key === 'ArrowDown' ? 1 : -1)))];
	ev.preventDefault ();
	select (next);
});

// --- status item (rts2-mon's "status"), shared with status.html -----------

const statusRows = new Map ();

function renderStatusTable () {
	const tbody = statusTableEl.tBodies[0];
	const seen = new Set ();
	let prev = null;
	for (const dev of devices) {
		seen.add (dev.name);
		let tr = statusRows.get (dev.name);
		if (!tr) {
			tr = document.createElement ('tr');
			tr.innerHTML = '<td class="dev-name"></td><td class="dev-state"></td><td class="dev-block"></td><td class="state-hex"></td>';
			tr.cells[0].textContent = dev.name;
			statusRows.set (dev.name, tr);
		}
		tr.className = 'level-' + dev.level;
		setHtml (tr.cells[1], escapeHtml (stateText (dev)));
		setHtml (tr.cells[2], blockBadges (dev));
		setHtml (tr.cells[3], (dev.state >>> 0).toString (16));
		const want = prev ? prev.nextSibling : tbody.firstChild;
		if (want !== tr)
			tbody.insertBefore (tr, want);
		prev = tr;
	}
	for (const [name, tr] of statusRows)
		if (!seen.has (name)) {
			tr.remove ();
			statusRows.delete (name);
		}
}

function setHtml (el, html) {
	if (el.innerHTML !== html)
		el.innerHTML = html;
}

// --- the selected device's values ----------------------------------------

const valueRows = new Map ();		// value name -> <tr>

// A raw value as an edit starts from: the wire's %.20e doubles shortened
// (1.79313741664366197586e+09 -> 1793137416.643662), pairs (RA/Dec) too.
function editText (raw) {
	return raw.split (' ').map (p => {
		if (!/^[-+]?\d/.test (p) || !/[.e]/.test (p))
			return p;
		const n = Number (p);
		return Number.isFinite (n) ? String (Number (n.toPrecision (15))) : p;
	}).join (' ');
}

function valueVisible (v) {
	if (!showDebug && v.dbg)
		return false;
	if (filterMode === 'fits' && !v.f)
		return false;
	if (filterMode === 'writable' && !v.w)
		return false;
	return !search || v.n.toLowerCase ().includes (search);
}

// SCRIPT with the element it runs now marked, as rts2-mon does
function valueHtml (v) {
	if (v.sp === undefined || !v.v)
		return escapeHtml (v.v);
	const end = v.sp + v.sl;
	return escapeHtml (v.v.slice (0, v.sp)) + `<span class="script-now">${escapeHtml (v.v.slice (v.sp, end))}</span>` + escapeHtml (v.v.slice (end));
}

function renderValues (dev) {
	stateEl.className = 'mon-statestring level-' + dev.level;
	stateEl.textContent = `${dev.name} ${dev.statestring}`;
	if (dev.progress === null) {
		progressEl.hidden = true;
	} else {
		progressEl.hidden = false;
		progressEl.firstElementChild.style.width = `${Math.min (100, Math.max (0, dev.progress))}%`;
		progressEl.lastElementChild.textContent = `${dev.progress.toFixed (1)} %`;
	}

	const seen = new Set ();
	let prev = null;
	for (const v of dev.values) {
		if (v.desc !== undefined)
			descriptions.set (dev.name + '\t' + v.n, v.desc);
		if (!valueVisible (v))
			continue;
		seen.add (v.n);
		let tr = valueRows.get (v.n);
		if (!tr) {
			tr = document.createElement ('tr');
			tr.innerHTML = '<td class="mon-w"></td><td class="mon-name"></td><td class="mon-val"></td>';
			tr.cells[1].textContent = v.n;
			tr.dataset.name = v.n;
			valueRows.set (v.n, tr);
		} else if (tr.dataset.v !== v.v) {
			// changed since the last poll: flash it
			tr.classList.remove ('changed');
			void tr.offsetWidth;
			tr.classList.add ('changed');
		}
		tr.dataset.v = v.v;
		tr.dataset.raw = v.raw;
		tr.dataset.t = v.t;
		tr.className = [tr.classList.contains ('changed') ? 'changed' : '', v.w ? 'writable' : '', v.f ? 'fits' : '',
			v.lvl ? 'lvl-' + v.lvl : '', v.n === editing ? 'editing' : ''].join (' ').trim ();
		setHtml (tr.cells[0], v.w ? 'W' : '');
		setHtml (tr.cells[2], valueHtml (v));
		const want = prev ? prev.nextSibling : valuesEl.firstChild;
		if (want !== tr)
			valuesEl.insertBefore (tr, want);
		prev = tr;
		// the editor stays under its value
		if (v.n === editing && editorRow) {
			if (prev.nextSibling !== editorRow)
				valuesEl.insertBefore (editorRow, prev.nextSibling);
			prev = editorRow;
		}
	}
	for (const [name, tr] of valueRows)
		if (!seen.has (name)) {
			tr.remove ();
			valueRows.delete (name);
		}
	if (editing && !seen.has (editing))
		closeEditor ();
}

// the description of the value under the mouse, rts2-mon's "D:" line
valuesEl.addEventListener ('mouseover', ev => {
	const tr = ev.target.closest ('tr[data-name]');
	if (!tr)
		return;
	const desc = descriptions.get (selected + '\t' + tr.dataset.name);
	descEl.textContent = desc ? `${tr.dataset.name}: ${desc}` : tr.dataset.name;
});

// --- editing a writable value ---------------------------------------------

let editorRow = null;

function closeEditor () {
	if (editorRow)
		editorRow.remove ();
	editorRow = null;
	if (editing && valueRows.get (editing))
		valueRows.get (editing).classList.remove ('editing');
	editing = null;
}

async function selectionOptions (device, name) {
	const key = device + '\t' + name;
	if (!selections.has (key)) {
		const res = await fetch (`api/selval?d=${encodeURIComponent (device)}&n=${encodeURIComponent (name)}`);
		selections.set (key, res.ok ? await res.json () : []);
	}
	return selections.get (key);
}

async function openEditor (tr) {
	closeEditor ();
	const name = tr.dataset.name;
	const type = Number (tr.dataset.t);
	const device = selected;
	editing = name;
	tr.classList.add ('editing');
	editorRow = document.createElement ('tr');
	editorRow.className = 'mon-editor';
	let input;
	if (type === RTS2_VALUE_SELECTION) {
		const opts = await selectionOptions (device, name);
		input = `<select>${opts.map ((o, i) => `<option value="${i}"${String (i) === tr.dataset.raw ? ' selected' : ''}>${i} ${escapeHtml (o)}</option>`).join ('')}</select>`;
	} else if (type === RTS2_VALUE_BOOL) {
		const on = /^(true|on|1)$/i.test (tr.dataset.raw);
		input = `<select><option value="true"${on ? ' selected' : ''}>true</option><option value="false"${on ? '' : ' selected'}>false</option></select>`;
	} else {
		const raw = type & RTS2_VALUE_MMAX ? tr.dataset.raw.split (' ')[0] : tr.dataset.raw;
		input = `<input type="text" value="${escapeHtml (editText (raw))}">`;
	}
	// the device may have changed or the editor closed while the
	// selection's options loaded
	if (editing !== name || selected !== device)
		return;
	editorRow.innerHTML = `<td></td><td colspan="2"><form class="mon-edit-form">${input}
		<button type="submit">Set</button><button type="button" class="mon-edit-cancel">Cancel</button>
		<span class="mon-edit-result"></span></form></td>`;
	tr.after (editorRow);
	const field = editorRow.querySelector ('input, select');
	field.focus ();
	if (field.select)
		field.select ();
	editorRow.querySelector ('.mon-edit-cancel').addEventListener ('click', closeEditor);
	editorRow.querySelector ('form').addEventListener ('submit', async ev => {
		ev.preventDefault ();
		const result = editorRow.querySelector ('.mon-edit-result');
		result.className = 'mon-edit-result';
		result.textContent = 'setting…';
		try {
			// a 401 brings up the browser's own login
			const res = await fetch (`api/set?d=${encodeURIComponent (device)}&n=${encodeURIComponent (name)}&v=${encodeURIComponent (field.value)}`);
			if (!res.ok) {
				const text = await res.text ();
				let err = text;
				try { err = JSON.parse (text).error || text; } catch (e) {}
				result.className = 'mon-edit-result err';
				result.textContent = `${res.status}: ${err}`;
				return;
			}
			closeEditor ();
			pollDevice ();
		} catch (e) {
			result.className = 'mon-edit-result err';
			result.textContent = String (e);
		}
	});
	editorRow.addEventListener ('keydown', ev => {
		if (ev.key === 'Escape') {
			ev.stopPropagation ();
			closeEditor ();
		}
	});
}

valuesEl.addEventListener ('click', ev => {
	const tr = ev.target.closest ('tr[data-name]');
	if (!tr || !tr.classList.contains ('writable') || tr.dataset.name === editing)
		return;
	openEditor (tr);
});

// --- filters ---------------------------------------------------------------

document.querySelectorAll ('#mon-filters button[data-filter]').forEach (b => b.addEventListener ('click', () => {
	filterMode = b.dataset.filter;
	document.querySelectorAll ('#mon-filters button[data-filter]').forEach (x => x.classList.toggle ('active', x === b));
	pollDevice ();
}));
document.getElementById ('mon-debug').addEventListener ('change', ev => {
	showDebug = ev.target.checked;
	pollDevice ();
});
document.getElementById ('mon-search').addEventListener ('input', ev => {
	search = ev.target.value.trim ().toLowerCase ();
	pollDevice ();
});

// --- polling ---------------------------------------------------------------

let deviceTimer = null;
let deviceSeq = 0;

// The selected device: api/monitor, with the descriptions the first time.
async function pollDevice () {
	clearTimeout (deviceTimer);
	const seq = ++deviceSeq;
	const name = selected;
	// the filters and the description line are for a device's values
	filtersEl.hidden = descEl.hidden = name === STATUS_ITEM;
	if (name === STATUS_ITEM) {
		valuesTableEl.hidden = true;
		statusTableEl.hidden = false;
		stateEl.className = 'mon-statestring';
		stateEl.textContent = 'status';
		progressEl.hidden = true;
		renderStatusTable ();
	} else {
		valuesTableEl.hidden = false;
		statusTableEl.hidden = true;
		const needDesc = ![...descriptions.keys ()].some (k => k.startsWith (name + '\t'));
		try {
			const res = await fetch (`api/monitor?d=${encodeURIComponent (name)}${needDesc ? '&desc=1' : ''}`, { cache: 'no-store' });
			if (res.ok && seq === deviceSeq && name === selected)
				renderValues (await res.json ());
		} catch (e) {
		}
	}
	if (seq === deviceSeq)
		deviceTimer = setTimeout (pollDevice, document.hidden ? POLL_HIDDEN_MS : POLL_MS);
}

// The list, the status item and the status bar: api/status.
async function pollStatus () {
	try {
		const res = await fetch ('api/status', { cache: 'no-store' });
		if (!res.ok)
			throw new Error (`api/status -> ${res.status}`);
		devices = sortLikeMon (await res.json ());
		lastOk = Date.now ();
		connEl.className = 'conn-status conn-open';
		connEl.textContent = 'live';
		document.body.classList.remove ('status-stale');
		renderDevList ();
		if (selected === STATUS_ITEM)
			renderStatusTable ();
		const c = devices.find (d => d.name === 'centrald');
		masterEl.textContent = c ? `${stateText (c)} (${(c.state >>> 0).toString (16)})` : '';
	} catch (e) {
		connEl.className = 'conn-status conn-closed';
		connEl.textContent = lastOk === null ? 'no answer' : `no answer since ${new Date (lastOk).toLocaleTimeString ()}`;
		document.body.classList.add ('status-stale');
	}
	setTimeout (pollStatus, document.hidden ? POLL_HIDDEN_MS : POLL_MS);
}

// Messages, oldest at the top and the newest at the bottom, as in
// rts2-mon. api/messages is the daemon's recent ring; new ones are those
// not shown yet.
const shownMessages = new Set ();
const MESSAGE_LEVELS = { 1: 'error', 2: 'warning', 4: 'info', 8: 'debug' };

async function pollMessages () {
	try {
		const res = await fetch ('api/messages', { cache: 'no-store' });
		if (res.ok) {
			const atBottom = messagesEl.parentElement.scrollTop + messagesEl.parentElement.clientHeight >= messagesEl.parentElement.scrollHeight - 4;
			for (const m of await res.json ()) {
				const key = `${m.time}\t${m.device}\t${m.text}`;
				if (shownMessages.has (key))
					continue;
				shownMessages.add (key);
				const li = document.createElement ('li');
				li.className = 'level-' + (MESSAGE_LEVELS[m.type & 0x0f] || 'info');
				li.innerHTML = `<span class="msg-time">${new Date (m.time * 1000).toLocaleTimeString ()}</span><span class="msg-device">${escapeHtml (m.device)}</span><span class="msg-text">${escapeHtml (m.text)}</span>`;
				messagesEl.appendChild (li);
			}
			while (messagesEl.children.length > MAX_MESSAGES)
				messagesEl.firstChild.remove ();
			if (atBottom)
				messagesEl.parentElement.scrollTop = messagesEl.parentElement.scrollHeight;
		}
	} catch (e) {
	}
	setTimeout (pollMessages, document.hidden ? POLL_HIDDEN_MS : MESSAGES_MS);
}

// --- command line ------------------------------------------------------------

document.getElementById ('mon-cmdline').addEventListener ('submit', async ev => {
	ev.preventDefault ();
	const device = selected;
	const command = cmdEl.value.trim ();
	if (!command || device === STATUS_ITEM)
		return;
	cmdResultEl.className = '';
	cmdResultEl.textContent = `${command}: waiting…`;
	try {
		const res = await fetch (`api/cmd?d=${encodeURIComponent (device)}&c=${encodeURIComponent (command)}`);
		const text = await res.text ();
		let answer = null;
		try { answer = JSON.parse (text); } catch (e) {}
		if (res.ok && answer && answer.ok) {
			cmdResultEl.className = 'ok';
			cmdResultEl.textContent = `${device} ${command}: OK`;
			cmdEl.value = '';
		} else if (res.ok && answer) {
			cmdResultEl.className = 'err';
			cmdResultEl.textContent = `${device} ${command}: failed (${answer.status})${answer.error ? ' ' + answer.error : ''}`;
		} else {
			cmdResultEl.className = 'err';
			cmdResultEl.textContent = `${device} ${command}: ${res.status} ${answer && answer.error ? answer.error : text}`;
		}
	} catch (e) {
		cmdResultEl.className = 'err';
		cmdResultEl.textContent = String (e);
	}
});

// --- status bar clock: UTC and local sidereal time ------------------------

let longitude = null;				// deg east, from api/horizon

function hms (deg) {
	const h = ((deg % 360) + 360) % 360 / 15;
	const s = Math.floor (h * 3600);
	return [Math.floor (s / 3600), Math.floor (s / 60) % 60, s % 60].map (x => String (x).padStart (2, '0')).join (':');
}

function tick () {
	const now = new Date ();
	let text = 'UTC ' + now.toISOString ().slice (0, 19).replace ('T', ' ');
	if (longitude !== null) {
		const jd = now.getTime () / 86400000 + 2440587.5;
		const gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0);
		text = `LST ${hms (gmst + longitude)} | ` + text;
	}
	clockEl.textContent = text;
}

// --- start -------------------------------------------------------------------

(async function init () {
	cmdDevEl.textContent = (selected === STATUS_ITEM ? '' : selected) + '>';
	cmdEl.disabled = selected === STATUS_ITEM;
	try {
		const res = await fetch ('api/horizon');
		if (res.ok)
			longitude = (await res.json ()).lon;
	} catch (e) {
	}
	tick ();
	setInterval (tick, 1000);
	pollStatus ();
	pollDevice ();
	pollMessages ();
}) ();
