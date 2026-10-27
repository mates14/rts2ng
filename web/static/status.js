// RTS2 status page (status.html) - vanilla JS, no build step, no
// external dependencies. rts2-mon's "status" list: centrald and every
// device with its state and blocking, from api/status, polled every
// second (rts2-mon redraws as often) - plain HTTP, so it works through
// the sites' Apache proxy, which passes no WebSocket.

const tbodyEl = document.querySelector ('#status-table tbody');
const connEl = document.getElementById ('status-conn');

const POLL_MS = 1000;
const POLL_HIDDEN_MS = 10000;

// The blocking bits (status.h), with the texts Connection::getStateString
// gives them: "own" when the device itself blocks (rts2-mon's "|"),
// "other" when the block comes from another device, in the full BOP
// state (rts2-mon's "#").
const BOP_FLAGS = [
	{ bit: 0x01000000, own: 'BLOCK EXPOSURE', other: 'BLOCK EXPOSING' },
	{ bit: 0x02000000, own: 'BLOCK READOUT', other: 'BLOCK READOUT' },
	{ bit: 0x04000000, own: 'BLOCK TELESCOPE MOVEMENT', other: 'BLOCK TELESCOPE MOVEMENT' },
	{ bit: 0x08000000, own: 'WILL EXPOSE', other: 'WILL EXPOSE' },
	{ bit: 0x10000000, own: 'WAIT TRIG EXPOSE', other: 'WAIT TRIG EXPOSE' },
];
// device blocks of its own, with no "#" form
const OWN_FLAGS = [
	{ bit: 0x00004000, text: 'BLOCK_OPEN' },
	{ bit: 0x00002000, text: 'BLOCK_CLOSE' },
	{ bit: 0x40000000, text: 'STOP' },
];

// The blocking parts of the state string, shown as badges instead: every
// " | TEXT" or " # TEXT" above, up to the next separator or the end (so
// that " | STOP" goes but a dome's " | STOPPED" stays).
const BLOCK_TEXTS = [...new Set ([...BOP_FLAGS.flatMap (f => [f.own, f.other]), ...OWN_FLAGS.map (f => f.text)])];
const BLOCK_RE = new RegExp (` [|#] (?:${BLOCK_TEXTS.join ('|')})(?= [|#]|$)`, 'g');

function escapeHtml (s) {
	return String (s).replace (/[&<>"']/g, c => ({
		'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
	}[c]));
}

function blockBadges (dev) {
	const badges = [];
	for (const f of OWN_FLAGS)
		if (dev.state & f.bit)
			badges.push (`<span class="block-badge own">${f.text}</span>`);
	for (const f of BOP_FLAGS) {
		if (dev.state & f.bit)
			badges.push (`<span class="block-badge own">${f.own}</span>`);
		else if (dev.bop & f.bit)
			badges.push (`<span class="block-badge other">${f.other}</span>`);
	}
	return badges.join (' ');
}

// One row per device, kept and updated in place, so that a selection or
// the scroll position survives the polls.
const rows = new Map ();

function rowFor (name) {
	let tr = rows.get (name);
	if (!tr) {
		tr = document.createElement ('tr');
		tr.innerHTML = '<td class="dev-name"></td><td class="dev-state"></td><td class="dev-block"></td><td class="state-hex"></td>';
		tr.cells[0].textContent = name;
		rows.set (name, tr);
	}
	return tr;
}

function setHtml (el, html) {
	if (el.innerHTML !== html)
		el.innerHTML = html;
}

function render (devices) {
	const seen = new Set ();
	let prev = null;
	for (const dev of devices) {
		seen.add (dev.name);
		const tr = rowFor (dev.name);
		tr.className = 'level-' + dev.level;
		const text = dev.statestring.replace (BLOCK_RE, '').replace (/^ \| /, '');
		setHtml (tr.cells[1], escapeHtml (text));
		setHtml (tr.cells[2], blockBadges (dev));
		setHtml (tr.cells[3], (dev.state >>> 0).toString (16));
		// in api/status's order
		const want = prev ? prev.nextSibling : tbodyEl.firstChild;
		if (want !== tr)
			tbodyEl.insertBefore (tr, want);
		prev = tr;
	}
	for (const [name, tr] of rows)
		if (!seen.has (name)) {
			tr.remove ();
			rows.delete (name);
		}
}

let lastOk = null;

async function poll () {
	if (!document.hidden || lastOk === null) {
		try {
			const res = await fetch ('api/status', { cache: 'no-store' });
			if (!res.ok)
				throw new Error (`api/status -> ${res.status}`);
			render (await res.json ());
			lastOk = Date.now ();
			connEl.className = 'conn-status conn-open';
			connEl.textContent = 'live';
			document.body.classList.remove ('status-stale');
		} catch (e) {
			connEl.className = 'conn-status conn-closed';
			connEl.textContent = lastOk === null ? 'no answer' : `no answer since ${new Date (lastOk).toLocaleTimeString ()}`;
			// the rows are the last known state, not the current one
			document.body.classList.add ('status-stale');
		}
	}
	setTimeout (poll, document.hidden ? POLL_HIDDEN_MS : POLL_MS);
}

poll ();
