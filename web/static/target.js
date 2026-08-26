// RTS2 target editor - vanilla JS, no build step, no external
// dependencies. Loaded via a plain relative <script src="target.js">, so
// fetch('api/...') calls below resolve correctly under any mount depth
// (see app.js's basePath() comment / STATUS.md's proxy-subpath note) -
// this page never constructs a WebSocket URL, so that's the only thing
// relative fetch() needs to get right here.
//
// Peer telescope sync (STATUS.md task 10, phase 2) is deliberately
// entirely client-side: neither daemon knows the other exists.  A
// same-origin Apache proxy path (e.g. "/d50" on lascaux's vhost,
// "/sbt" on d50's) makes the peer's own /api/db/* reachable without
// CORS. The synced-field list below (name/comment/priority/
// interruptible/position-or-mpec, plus sinfo's `type=` token only) is
// the user's own call, not a guess - bonus/enabled/next_observable are
// deliberately per-telescope (create/tables.sql's own comment on
// tar_bonus: "site dependent part - depends highly on local object
// visibility"), and the rest of sinfo (duration=/mag=/snr=/filters=/
// count=/pscale=) is deliberately per-telescope too (a 50cm and a 20cm
// scope want different exposure parameters for the same target).

let currentId = null;
let currentTarget = null;			 // last-loaded-or-saved local target JSON
let currentSinfo = '';				 // last-loaded-or-saved local sinfo string
let peerTarget = null;
let peerSinfo = null;				 // null = not fetched/unavailable, "" = fetched, no row yet

const loadStatusEl = document.getElementById ('load-status');
const targetPanelEl = document.getElementById ('target-panel');
const schedulingPanelEl = document.getElementById ('scheduling-panel');
const saveStatusEl = document.getElementById ('save-status');
const schedulingStatusEl = document.getElementById ('scheduling-status');
const peerPanelEl = document.getElementById ('peer-panel');
const peerStatusEl = document.getElementById ('peer-status');
const peerCompareEl = document.getElementById ('peer-compare');
const peerButtonsEl = document.getElementById ('peer-buttons');

const PEER_PREFIX_KEY = 'rts2-target-peer-prefix';

function setStatus (el, ok, text) {
	el.className = 'status-line ' + (ok ? 'ok' : 'err');
	el.textContent = text;
}

function peerPrefix () {
	return (localStorage.getItem (PEER_PREFIX_KEY) || '').trim ().replace (/\/$/, '');
}

// --- sinfo key=value parsing (mirrors sch/database.py's parse_sinfo) -------

function parseSinfo (s) {
	const out = {};
	(s || '').split (/\s+/).filter (Boolean).forEach ((tok) => {
		const i = tok.indexOf ('=');
		if (i > 0)
			out[tok.slice (0, i)] = tok.slice (i + 1);
	});
	return out;
}

function serializeSinfo (obj) {
	return Object.keys (obj).map ((k) => `${k}=${obj[k]}`).join (' ');
}

function escapeHtml (s) {
	return String (s).replace (/&/g, '&amp;').replace (/</g, '&lt;').replace (/>/g, '&gt;');
}

// --- Local target load/save -------------------------------------------------

async function loadTarget (id) {
	setStatus (loadStatusEl, true, 'loading…');
	targetPanelEl.hidden = true;
	schedulingPanelEl.hidden = true;
	peerPanelEl.hidden = true;

	let tar;
	try {
		const res = await fetch (`api/db/target?id=${encodeURIComponent (id)}`);
		const body = await res.json ();
		if (!res.ok) {
			setStatus (loadStatusEl, false, body.error || `${res.status}`);
			return;
		}
		tar = body;
	} catch (e) {
		setStatus (loadStatusEl, false, String (e));
		return;
	}

	currentId = tar.id;
	currentTarget = tar;
	setStatus (loadStatusEl, true, `loaded target ${tar.id}`);

	document.getElementById ('tar-id-label').textContent = `#${tar.id}`;
	document.getElementById ('f-name').value = tar.name || '';
	document.getElementById ('f-type').value = tar.type;
	document.getElementById ('f-priority').value = tar.priority ?? '';
	document.getElementById ('f-bonus').value = tar.bonus ?? '';
	document.getElementById ('f-enabled').checked = !!tar.enabled;
	document.getElementById ('f-interruptible').checked = !!tar.interruptible;
	document.getElementById ('f-comment').value = tar.comment || '';

	document.getElementById ('position-fields').hidden = !tar.hasPosition;
	if (tar.hasPosition) {
		document.getElementById ('f-ra').value = tar.ra ?? '';
		document.getElementById ('f-dec').value = tar.dec ?? '';
		document.getElementById ('f-pmra').value = tar.pmRa ?? '';
		document.getElementById ('f-pmdec').value = tar.pmDec ?? '';
	}

	// The two textareas are mutually exclusive - an elliptical target's
	// tar_info *is* its MPC orbital line (parsed server-side by
	// EllTarget::orbitFromMPC(), which also derives name/type from it),
	// so editing it as freeform "info" would either be redundant or,
	// worse, get silently ignored (dbUpdateTarget() only honours `info`
	// when `mpec` isn't also present - see dbendpoints.h).
	document.getElementById ('mpec-label').hidden = !tar.isElliptical;
	document.getElementById ('info-label').hidden = tar.isElliptical;
	if (tar.isElliptical) {
		document.getElementById ('f-mpec').value = tar.info || '';
	} else {
		document.getElementById ('f-info').value = tar.info || '';
	}

	targetPanelEl.hidden = false;
	peerPanelEl.hidden = false;
	document.getElementById ('peer-prefix').value = peerPrefix ();

	// Scheduling has its own existence check server-side (dbGetScheduling
	// throws if the target itself doesn't exist) but we already know it
	// does at this point, so this is just "does it have a row yet".
	try {
		const res = await fetch (`api/db/scheduling?id=${encodeURIComponent (tar.id)}`);
		const body = await res.json ();
		if (res.ok) {
			currentSinfo = body.sinfo || '';
			document.getElementById ('f-sinfo').value = currentSinfo;
			schedulingStatusEl.textContent = '';
			schedulingPanelEl.hidden = false;
		}
	} catch (e) {
		// Non-fatal - the target itself loaded fine, scheduling is a
		// secondary panel. WEB_WITH_DB=OFF builds also 404 here, which
		// this catch/no-op treats the same as "leave it hidden".
	}

	await loadPeer (tar.id);
}

document.getElementById ('load-form').addEventListener ('submit', (ev) => {
	ev.preventDefault ();
	const id = document.getElementById ('load-id').value;
	if (id)
		loadTarget (id);
});

document.getElementById ('target-form').addEventListener ('submit', async (ev) => {
	ev.preventDefault ();
	if (currentId === null)
		return;

	const params = new URLSearchParams ();
	params.set ('id', currentId);
	params.set ('name', document.getElementById ('f-name').value);
	params.set ('comment', document.getElementById ('f-comment').value);
	params.set ('priority', document.getElementById ('f-priority').value || '0');
	params.set ('bonus', document.getElementById ('f-bonus').value || '0');
	params.set ('enabled', document.getElementById ('f-enabled').checked ? '1' : '0');
	params.set ('interruptible', document.getElementById ('f-interruptible').checked ? '1' : '0');

	if (!document.getElementById ('position-fields').hidden) {
		params.set ('ra', document.getElementById ('f-ra').value || '0');
		params.set ('dec', document.getElementById ('f-dec').value || '0');
		params.set ('pm_ra', document.getElementById ('f-pmra').value || '0');
		params.set ('pm_dec', document.getElementById ('f-pmdec').value || '0');
	}

	if (!document.getElementById ('mpec-label').hidden) {
		params.set ('mpec', document.getElementById ('f-mpec').value);
	} else {
		params.set ('info', document.getElementById ('f-info').value);
	}

	try {
		// A 401 here triggers the browser's own native credential prompt
		// (WWW-Authenticate: Basic) exactly like app.js's command form -
		// see web/STATUS.md task 6, no credential handling needed here.
		const res = await fetch (`api/db/target-save?${params.toString ()}`);
		const body = await res.json ();
		if (!res.ok) {
			setStatus (saveStatusEl, false, body.error || `${res.status}`);
			return;
		}
		setStatus (saveStatusEl, true, 'saved');
		currentTarget = body;
		renderPeerCompare ();
	} catch (e) {
		setStatus (saveStatusEl, false, String (e));
	}
});

document.getElementById ('scheduling-form').addEventListener ('submit', async (ev) => {
	ev.preventDefault ();
	if (currentId === null)
		return;

	const sinfo = document.getElementById ('f-sinfo').value;
	const url = `api/db/scheduling-save?id=${encodeURIComponent (currentId)}&sinfo=${encodeURIComponent (sinfo)}`;
	try {
		const res = await fetch (url);
		const body = await res.json ();
		if (!res.ok) {
			setStatus (schedulingStatusEl, false, body.error || `${res.status}`);
			return;
		}
		setStatus (schedulingStatusEl, true, 'saved');
		currentSinfo = body.sinfo;
		renderPeerCompare ();
	} catch (e) {
		setStatus (schedulingStatusEl, false, String (e));
	}
});

// --- Peer telescope (optional, see file header) -----------------------------

document.getElementById ('peer-config-form').addEventListener ('submit', (ev) => {
	ev.preventDefault ();
	const val = document.getElementById ('peer-prefix').value.trim ().replace (/\/$/, '');
	localStorage.setItem (PEER_PREFIX_KEY, val);
	document.getElementById ('peer-prefix').value = val;
	if (currentId !== null)
		loadPeer (currentId);
});

async function loadPeer (id) {
	peerTarget = null;
	peerSinfo = null;
	peerButtonsEl.hidden = true;
	peerCompareEl.innerHTML = '';

	const prefix = peerPrefix ();
	if (!prefix) {
		setStatus (peerStatusEl, true, 'not configured - set a peer proxy path above to compare with the other telescope');
		return;
	}

	setStatus (peerStatusEl, true, 'loading peer…');
	try {
		const res = await fetch (`${prefix}/api/db/target?id=${encodeURIComponent (id)}`);
		const body = await res.json ();
		if (!res.ok) {
			setStatus (peerStatusEl, false, `peer: ${body.error || res.status} (not present there, or the proxy path is wrong)`);
			return;
		}
		peerTarget = body;

		try {
			const sres = await fetch (`${prefix}/api/db/scheduling?id=${encodeURIComponent (id)}`);
			const sbody = await sres.json ();
			if (sres.ok)
				peerSinfo = sbody.sinfo || '';
		} catch (e) {
			// non-fatal - comparison just shows sinfo type= as unknown
		}

		setStatus (peerStatusEl, true, `peer loaded (id ${peerTarget.id})`);
		peerButtonsEl.hidden = false;
		renderPeerCompare ();
	} catch (e) {
		setStatus (peerStatusEl, false, `peer unreachable: ${e}`);
	}
}

function nearlyEqual (a, b) {
	if (a === null || a === undefined || b === null || b === undefined)
		return a === b;
	return Math.abs (a - b) < 1e-6;
}

function renderPeerCompare () {
	if (!currentTarget || !peerTarget) {
		peerCompareEl.innerHTML = '';
		return;
	}

	const rows = [];
	const add = (label, localVal, peerVal, numeric) => {
		const differs = numeric ? !nearlyEqual (localVal, peerVal) : String (localVal ?? '') !== String (peerVal ?? '');
		rows.push ({ label, localVal, peerVal, differs });
	};

	add ('Name', currentTarget.name || '', peerTarget.name || '');
	add ('Comment', currentTarget.comment || '', peerTarget.comment || '');
	add ('Priority', currentTarget.priority, peerTarget.priority, true);
	add ('Interruptible', currentTarget.interruptible, peerTarget.interruptible);
	add (currentTarget.isElliptical ? 'MPC line' : 'Info', currentTarget.info || '', peerTarget.info || '');
	if (currentTarget.hasPosition && peerTarget.hasPosition) {
		add ('RA', currentTarget.ra, peerTarget.ra, true);
		add ('Dec', currentTarget.dec, peerTarget.dec, true);
		add ('PM RA', currentTarget.pmRa, peerTarget.pmRa, true);
		add ('PM Dec', currentTarget.pmDec, peerTarget.pmDec, true);
	}
	if (currentTarget.type !== peerTarget.type)
		rows.push ({ label: 'Type', localVal: currentTarget.type, peerVal: peerTarget.type, differs: true });

	const localType = parseSinfo (currentSinfo).type;
	const peerType = peerSinfo !== null ? parseSinfo (peerSinfo).type : undefined;
	rows.push ({
		label: 'sinfo type=',
		localVal: localType || '(none)',
		peerVal: peerSinfo === null ? '?' : (peerType || '(none)'),
		differs: peerSinfo !== null && (localType || '') !== (peerType || ''),
	});

	// Shown for context only - deliberately not compared/synced (see
	// target.html's hint text for why).
	rows.push ({ label: 'Enabled (not synced)', localVal: currentTarget.enabled, peerVal: peerTarget.enabled, differs: false, info: true });
	rows.push ({ label: 'Bonus (not synced)', localVal: currentTarget.bonus, peerVal: peerTarget.bonus, differs: false, info: true });

	let html = '<table class="peer-compare-table"><thead><tr><th>Field</th><th>Local</th><th>Peer</th></tr></thead><tbody>';
	for (const r of rows) {
		const cls = r.differs ? 'diff' : (r.info ? 'info-row' : '');
		html += `<tr class="${cls}"><td>${escapeHtml (r.label)}</td><td>${escapeHtml (r.localVal)}</td><td>${escapeHtml (r.peerVal)}</td></tr>`;
	}
	html += '</tbody></table>';
	peerCompareEl.innerHTML = html;
}

function buildSyncParams (tar) {
	const params = new URLSearchParams ();
	params.set ('name', tar.name || '');
	params.set ('comment', tar.comment || '');
	if (tar.priority !== null && tar.priority !== undefined)
		params.set ('priority', tar.priority);
	params.set ('interruptible', tar.interruptible ? '1' : '0');
	if (tar.hasPosition) {
		params.set ('ra', tar.ra ?? '0');
		params.set ('dec', tar.dec ?? '0');
		params.set ('pm_ra', tar.pmRa ?? '0');
		params.set ('pm_dec', tar.pmDec ?? '0');
	}
	if (tar.isElliptical)
		params.set ('mpec', tar.info || '');
	else
		params.set ('info', tar.info || '');
	return params;
}

document.getElementById ('peer-push').addEventListener ('click', async () => {
	const prefix = peerPrefix ();
	if (!prefix || currentId === null || !currentTarget)
		return;

	setStatus (peerStatusEl, true, 'pushing…');
	try {
		const params = buildSyncParams (currentTarget);
		params.set ('id', currentId);
		// Same native Basic-Auth-prompt behaviour as the local save form -
		// the peer daemon gates its own writes independently.
		const res = await fetch (`${prefix}/api/db/target-save?${params.toString ()}`);
		const body = await res.json ();
		if (!res.ok) {
			setStatus (peerStatusEl, false, `peer target-save: ${body.error || res.status}`);
			return;
		}

		// Only sinfo's `type=` token is synced - see file header.
		const sres = await fetch (`${prefix}/api/db/scheduling?id=${encodeURIComponent (currentId)}`);
		const sbody = await sres.json ();
		if (sres.ok) {
			const peerParsed = parseSinfo (sbody.sinfo);
			const localParsed = parseSinfo (currentSinfo);
			if ('type' in localParsed)
				peerParsed.type = localParsed.type;
			else
				delete peerParsed.type;
			await fetch (`${prefix}/api/db/scheduling-save?id=${encodeURIComponent (currentId)}&sinfo=${encodeURIComponent (serializeSinfo (peerParsed))}`);
		}

		setStatus (peerStatusEl, true, 'pushed to peer');
		await loadPeer (currentId);
	} catch (e) {
		setStatus (peerStatusEl, false, String (e));
	}
});

document.getElementById ('peer-pull').addEventListener ('click', () => {
	if (!peerTarget)
		return;

	document.getElementById ('f-name').value = peerTarget.name || '';
	document.getElementById ('f-comment').value = peerTarget.comment || '';
	document.getElementById ('f-priority').value = peerTarget.priority ?? '';
	document.getElementById ('f-interruptible').checked = !!peerTarget.interruptible;

	if (peerTarget.hasPosition && !document.getElementById ('position-fields').hidden) {
		document.getElementById ('f-ra').value = peerTarget.ra ?? '';
		document.getElementById ('f-dec').value = peerTarget.dec ?? '';
		document.getElementById ('f-pmra').value = peerTarget.pmRa ?? '';
		document.getElementById ('f-pmdec').value = peerTarget.pmDec ?? '';
	}
	if (!document.getElementById ('mpec-label').hidden) {
		document.getElementById ('f-mpec').value = peerTarget.info || '';
	} else {
		document.getElementById ('f-info').value = peerTarget.info || '';
	}

	if (peerSinfo !== null) {
		const localParsed = parseSinfo (document.getElementById ('f-sinfo').value);
		const peerParsed = parseSinfo (peerSinfo);
		if ('type' in peerParsed)
			localParsed.type = peerParsed.type;
		else
			delete localParsed.type;
		document.getElementById ('f-sinfo').value = serializeSinfo (localParsed);
	}

	setStatus (peerStatusEl, true, 'pulled into the form - review, then click Save target / Save scheduling to commit locally');
});

// Support ?id=N in the URL so a link (e.g. from a future target-list
// view) can jump straight to a target instead of requiring the id to be
// typed in by hand.
document.getElementById ('peer-prefix').value = peerPrefix ();
const preselectId = new URLSearchParams (location.search).get ('id');
if (preselectId) {
	document.getElementById ('load-id').value = preselectId;
	loadTarget (preselectId);
}
