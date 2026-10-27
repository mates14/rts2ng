// RTS2 status page (status.html) - vanilla JS, no build step, no
// external dependencies. rts2-mon's "status" list: centrald and every
// device with its state and blocking, from api/status, polled every
// second (rts2-mon redraws as often) - plain HTTP, so it works through
// the sites' Apache proxy, which passes no WebSocket.

const tbodyEl = document.querySelector ('#status-table tbody');
const connEl = document.getElementById ('status-conn');

const POLL_MS = 1000;
const POLL_HIDDEN_MS = 10000;

// BOP_FLAGS, OWN_FLAGS, BLOCK_RE, escapeHtml, blockBadges () and stateText () are in
// rts2status.js, shared with monitor.html.

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
		setHtml (tr.cells[1], escapeHtml (stateText (dev)));
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
