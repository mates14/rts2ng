// RTS2 target editor - vanilla JS, no build step, no external
// dependencies. Loaded via a plain relative <script src="target.js">, so
// fetch('api/...') calls below resolve correctly under any mount depth
// (see app.js's basePath() comment / STATUS.md's proxy-subpath note) -
// this page never constructs a WebSocket URL, so that's the only thing
// relative fetch() needs to get right here.
//
// D50/SBT peer sync (STATUS.md task 10 phase 2, redesigned in phase 3 to
// "always sync" rather than manual push/pull) is deliberately entirely
// client-side: neither daemon knows the other exists. A same-origin
// Apache proxy path (e.g. "/d50" on lascaux's vhost, "/sbt" on d50's)
// makes the peer's own /api/db/* reachable without CORS.
//
// Sync rule (user's own call, not a guess): for the fields that should
// always be identical (name/comment/priority/interruptible/position-or-
// mpec, plus sinfo's `type=` token only - see SITES/getSyncFieldDescriptors
// below) -
//   - present on one side, missing on the other -> fill the missing side
//     immediately, no confirmation needed (this is what "always syncs"
//     means in practice).
//   - present and different on both sides -> never auto-written; the SBT
//     side's value is shown in the form here (only when this instance
//     isn't SBT itself) so a human Save adopts it, but nothing is written
//     automatically and the non-SBT side's stored value is left alone
//     until that happens.
//   - a normal Save (on either side) also pushes the current form values
//     for these fields to the peer - that's how a deliberate edit here
//     propagates, and how a conflict shown-as-SBT actually gets committed.
// enabled/bonus/bonus_time/next_observable and the rest of sinfo
// (duration=/mag=/snr=/filters=/count=/pscale=) are deliberately never
// synced - see target.html's hint text for why.
//
// Scripts (per-camera observing script overrides) are NOT synced at all
// - D50 and SBT don't share cameras, so there's nothing to reconcile.

// --- Site / camera configuration --------------------------------------------

const SITES = {
	sbt: { label: 'SBT', peerPathDefault: '/d50', cameras: ['C1', 'C2', 'C3'], defaultScriptHints: { C3: 'guider' } },
	d50: { label: 'D50', peerPathDefault: '/sbt', cameras: ['C0', 'C1'], defaultScriptHints: { C1: 'guider' } },
};

const SITE_KEY = 'rts2-target-site';
const PEER_PREFIX_KEY = 'rts2-target-peer-prefix';

function detectSite () {
	const stored = localStorage.getItem (SITE_KEY);
	if (stored && SITES[stored])
		return stored;
	return location.hostname.includes ('d50') ? 'd50' : 'sbt';
}

let currentSite = detectSite ();

function peerPrefix () {
	const stored = localStorage.getItem (PEER_PREFIX_KEY);
	if (stored !== null)
		return stored.trim ().replace (/\/$/, '');
	return SITES[currentSite].peerPathDefault;
}

// --- Shared state ------------------------------------------------------------

let currentId = null;
let currentTarget = null;			 // last-loaded-or-saved local target JSON
let currentSinfo = '';				 // last-loaded-or-saved local sinfo string
let peerTarget = null;
let peerSinfo = null;				 // null = not fetched/unavailable, "" = fetched, no row yet

const loadStatusEl = document.getElementById ('load-status');
const targetPanelEl = document.getElementById ('target-panel');
const scriptsPanelEl = document.getElementById ('scripts-panel');
const schedulingPanelEl = document.getElementById ('scheduling-panel');
const saveStatusEl = document.getElementById ('save-status');
const schedulingStatusEl = document.getElementById ('scheduling-status');
const peerPanelEl = document.getElementById ('peer-panel');
const peerStatusEl = document.getElementById ('peer-status');
const peerLogEl = document.getElementById ('peer-log');
const peerCompareEl = document.getElementById ('peer-compare');

function setStatus (el, ok, text) {
	el.className = 'status-line ' + (ok ? 'ok' : 'err');
	el.textContent = text;
}

function escapeHtml (s) {
	return String (s).replace (/&/g, '&amp;').replace (/</g, '&lt;').replace (/>/g, '&gt;');
}

function nearlyEqual (a, b) {
	if (a === null || a === undefined || b === null || b === undefined)
		return a === b;
	return Math.abs (a - b) < 1e-6;
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

// --- Local target load/save -------------------------------------------------

function populateFormFromTarget (tar) {
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
	// worse, get silently rejected (dbUpdateTarget() now rejects `info`
	// outright for an elliptical target - see dbendpoints.cpp).
	document.getElementById ('mpec-label').hidden = !tar.isElliptical;
	document.getElementById ('info-label').hidden = tar.isElliptical;
	if (tar.isElliptical) {
		document.getElementById ('f-mpec').value = tar.info || '';
	} else {
		document.getElementById ('f-info').value = tar.info || '';
	}
}

async function loadTarget (id) {
	setStatus (loadStatusEl, true, 'loading…');
	targetPanelEl.hidden = true;
	scriptsPanelEl.hidden = true;
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

	populateFormFromTarget (tar);
	targetPanelEl.hidden = false;

	document.getElementById ('peer-site').value = currentSite;
	document.getElementById ('peer-prefix').value = peerPrefix ();
	document.getElementById ('peer-site-label').textContent = `(this is ${SITES[currentSite].label})`;

	await loadScripts (tar.id);

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

	peerPanelEl.hidden = false;
	await syncWithPeer (tar.id);
}

document.getElementById ('load-form').addEventListener ('submit', (ev) => {
	ev.preventDefault ();
	const id = document.getElementById ('load-id').value;
	if (id)
		loadTarget (id);
});

function readTargetFormParams () {
	const params = new URLSearchParams ();
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
	return params;
}

document.getElementById ('target-form').addEventListener ('submit', async (ev) => {
	ev.preventDefault ();
	if (currentId === null)
		return;

	const params = readTargetFormParams ();
	params.set ('id', currentId);

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
		await pushSyncedFieldsToPeer ();
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
		await pushSinfoTypeToPeer ();
		renderPeerCompare ();
	} catch (e) {
		setStatus (schedulingStatusEl, false, String (e));
	}
});

// --- Scripts (per-camera, never synced) -------------------------------------

async function loadScripts (id) {
	const listEl = document.getElementById ('scripts-list');
	listEl.textContent = 'loading…';
	try {
		const res = await fetch (`api/db/scripts?id=${encodeURIComponent (id)}`);
		const body = await res.json ();
		if (!res.ok) {
			scriptsPanelEl.hidden = true;
			return;
		}
		renderScripts (body.scripts || {});
		scriptsPanelEl.hidden = false;
	} catch (e) {
		// WEB_HAVE_DB=OFF builds (or a very old daemon predating this
		// endpoint) - hide the panel rather than show a permanently
		// broken one, same convention as scheduling above.
		scriptsPanelEl.hidden = true;
	}
}

function renderScripts (overrides) {
	const site = SITES[currentSite];
	const listEl = document.getElementById ('scripts-list');
	listEl.textContent = '';

	for (const cam of site.cameras) {
		const hasOverride = Object.prototype.hasOwnProperty.call (overrides, cam);
		const hint = site.defaultScriptHints[cam];

		const row = document.createElement ('div');
		row.className = 'script-row';

		const nameEl = document.createElement ('div');
		nameEl.className = 'script-camera-name';
		nameEl.textContent = cam;

		const input = document.createElement ('input');
		input.type = 'text';
		input.className = 'script-input';
		input.spellcheck = false;
		input.value = hasOverride ? overrides[cam] : '';
		input.placeholder = 'using default' + (hint ? ` (${hint})` : '');

		const defaultBtn = document.createElement ('button');
		defaultBtn.type = 'button';
		defaultBtn.className = 'script-default-btn' + (hasOverride ? '' : ' active');
		defaultBtn.textContent = '[*] default' + (hint ? ` (${hint})` : '');

		const saveBtn = document.createElement ('button');
		saveBtn.type = 'button';
		saveBtn.textContent = 'Save';

		const statusEl = document.createElement ('span');
		statusEl.className = 'status-line script-row-status';

		saveBtn.addEventListener ('click', () => saveScript (cam, input, statusEl));
		defaultBtn.addEventListener ('click', () => deleteScript (cam, input, statusEl));

		row.append (nameEl, input, defaultBtn, saveBtn, statusEl);
		listEl.appendChild (row);
	}
}

async function saveScript (camera, input, statusEl) {
	try {
		const res = await fetch (`api/db/script-save?id=${encodeURIComponent (currentId)}&camera=${encodeURIComponent (camera)}&script=${encodeURIComponent (input.value)}`);
		const body = await res.json ();
		if (!res.ok) {
			setStatus (statusEl, false, body.error || `${res.status}`);
			return;
		}
		setStatus (statusEl, true, 'saved');
		loadScripts (currentId);
	} catch (e) {
		setStatus (statusEl, false, String (e));
	}
}

async function deleteScript (camera, input, statusEl) {
	try {
		const res = await fetch (`api/db/script-delete?id=${encodeURIComponent (currentId)}&camera=${encodeURIComponent (camera)}`);
		const body = await res.json ();
		if (!res.ok) {
			setStatus (statusEl, false, body.error || `${res.status}`);
			return;
		}
		setStatus (statusEl, true, 'cleared - using default');
		loadScripts (currentId);
	} catch (e) {
		setStatus (statusEl, false, String (e));
	}
}

// --- Peer telescope: always-sync (see file header) --------------------------

document.getElementById ('peer-config-form').addEventListener ('submit', (ev) => {
	ev.preventDefault ();
	currentSite = document.getElementById ('peer-site').value;
	localStorage.setItem (SITE_KEY, currentSite);
	const val = document.getElementById ('peer-prefix').value.trim ().replace (/\/$/, '');
	localStorage.setItem (PEER_PREFIX_KEY, val);
	document.getElementById ('peer-prefix').value = peerPrefix ();
	document.getElementById ('peer-site-label').textContent = `(this is ${SITES[currentSite].label})`;
	if (currentId !== null) {
		loadScripts (currentId);
		syncWithPeer (currentId);
	}
});

document.getElementById ('peer-refresh').addEventListener ('click', () => {
	if (currentId !== null)
		syncWithPeer (currentId);
});

/**
 * Fields expected to be identical on both telescopes for this target,
 * shaped for the currently-loaded local target (position-or-mpec/info
 * depends on its type). `get` reads a value out of a dbGetTarget()-shaped
 * JSON object (local or peer); `isEmpty` decides whether a value counts
 * as "not set yet" (auto-fill eligible) as opposed to "a real value that
 * might conflict"; `formEl`/`bool`/`numeric` let the same descriptor
 * drive reading/writing the local form and building target-save params.
 */
function getSyncFieldDescriptors (tar) {
	const list = [
		{ key: 'name', label: 'Name', param: 'name', get: (t) => t.name || '', isEmpty: (v) => v === '', formEl: () => document.getElementById ('f-name') },
		{ key: 'comment', label: 'Comment', param: 'comment', get: (t) => t.comment || '', isEmpty: (v) => v === '', formEl: () => document.getElementById ('f-comment') },
		{ key: 'priority', label: 'Priority', param: 'priority', get: (t) => t.priority, isEmpty: () => false, numeric: true, formEl: () => document.getElementById ('f-priority') },
		{ key: 'interruptible', label: 'Interruptible', param: 'interruptible', get: (t) => !!t.interruptible, isEmpty: () => false, bool: true, formEl: () => document.getElementById ('f-interruptible') },
	];

	if (tar.isElliptical) {
		list.push ({ key: 'info', label: 'MPC line', param: 'mpec', get: (t) => t.info || '', isEmpty: (v) => v === '', formEl: () => document.getElementById ('f-mpec') });
	} else {
		list.push ({ key: 'info', label: 'Info', param: 'info', get: (t) => t.info || '', isEmpty: (v) => v === '', formEl: () => document.getElementById ('f-info') });
	}

	if (tar.hasPosition) {
		list.push (
			{ key: 'ra', label: 'RA', param: 'ra', get: (t) => t.ra, isEmpty: (v) => v === null || v === undefined, numeric: true, formEl: () => document.getElementById ('f-ra') },
			{ key: 'dec', label: 'Dec', param: 'dec', get: (t) => t.dec, isEmpty: (v) => v === null || v === undefined, numeric: true, formEl: () => document.getElementById ('f-dec') },
			{ key: 'pmRa', label: 'PM RA', param: 'pm_ra', get: (t) => t.pmRa, isEmpty: (v) => v === null || v === undefined, numeric: true, formEl: () => document.getElementById ('f-pmra') },
			{ key: 'pmDec', label: 'PM Dec', param: 'pm_dec', get: (t) => t.pmDec, isEmpty: (v) => v === null || v === undefined, numeric: true, formEl: () => document.getElementById ('f-pmdec') }
		);
	}

	return list;
}

function readFormValue (d) {
	const el = d.formEl ();
	if (d.bool)
		return el.checked;
	if (d.numeric)
		return el.value === '' ? null : parseFloat (el.value);
	return el.value;
}

function writeFormValue (d, value) {
	const el = d.formEl ();
	if (d.bool)
		el.checked = !!value;
	else
		el.value = value ?? '';
}

async function fetchJson (url) {
	const res = await fetch (url);
	const body = await res.json ();
	return { ok: res.ok, body };
}

async function syncWithPeer (id) {
	peerTarget = null;
	peerSinfo = null;
	peerLogEl.innerHTML = '';
	peerCompareEl.innerHTML = '';

	const prefix = peerPrefix ();
	if (!prefix) {
		setStatus (peerStatusEl, true, 'peer sync disabled - set a peer proxy path above');
		return;
	}

	setStatus (peerStatusEl, true, 'loading peer…');
	let r;
	try {
		r = await fetchJson (`${prefix}/api/db/target?id=${encodeURIComponent (id)}`);
	} catch (e) {
		setStatus (peerStatusEl, false, `peer unreachable: ${e}`);
		return;
	}
	if (!r.ok) {
		setStatus (peerStatusEl, false, `peer: ${r.body.error || 'error'} (not present there, or the proxy path is wrong)`);
		return;
	}
	peerTarget = r.body;

	try {
		const sr = await fetchJson (`${prefix}/api/db/scheduling?id=${encodeURIComponent (id)}`);
		if (sr.ok)
			peerSinfo = sr.body.sinfo || '';
	} catch (e) {
		// non-fatal - comparison just shows sinfo type= as unknown
	}

	if (currentTarget.hasPosition !== peerTarget.hasPosition || currentTarget.isElliptical !== peerTarget.isElliptical) {
		setStatus (peerStatusEl, false, `peer loaded, but target type shape differs (local hasPosition=${currentTarget.hasPosition}/isElliptical=${currentTarget.isElliptical} vs peer ${peerTarget.hasPosition}/${peerTarget.isElliptical}) - skipping position/info sync for now`);
	} else {
		setStatus (peerStatusEl, true, `peer loaded (id ${peerTarget.id})`);
	}

	await applySyncPlan ();
}

async function applySyncPlan () {
	const iAmSbt = currentSite === 'sbt';
	const typeMatches = currentTarget.hasPosition === peerTarget.hasPosition && currentTarget.isElliptical === peerTarget.isElliptical;
	const descriptors = typeMatches ? getSyncFieldDescriptors (currentTarget) : getSyncFieldDescriptors (currentTarget).filter ((d) => d.key !== 'info' && d.key !== 'ra' && d.key !== 'dec' && d.key !== 'pmRa' && d.key !== 'pmDec');

	const localFill = new URLSearchParams ();
	const peerFill = new URLSearchParams ();
	const log = [];
	const conflicts = [];

	for (const d of descriptors) {
		const lv = d.get (currentTarget);
		const pv = d.get (peerTarget);
		const lEmpty = d.isEmpty (lv);
		const pEmpty = d.isEmpty (pv);

		if (lEmpty && !pEmpty) {
			localFill.set (d.param, d.bool ? (pv ? '1' : '0') : pv);
			log.push (`${d.label}: was empty locally, filled from peer`);
		} else if (pEmpty && !lEmpty) {
			peerFill.set (d.param, d.bool ? (lv ? '1' : '0') : lv);
			log.push (`${d.label}: was empty on peer, filled from local`);
		} else if (!lEmpty && !pEmpty) {
			const differs = d.numeric ? !nearlyEqual (lv, pv) : String (lv) !== String (pv);
			if (differs)
				conflicts.push ({ d, lv, pv });
		}
	}

	const localType = parseSinfo (currentSinfo).type;
	const peerType = peerSinfo !== null ? parseSinfo (peerSinfo).type : undefined;
	let sinfoLocalFill = null, sinfoPeerFill = null, sinfoConflict = null;
	if (peerSinfo !== null) {
		if (!localType && peerType) {
			sinfoLocalFill = peerType;
			log.push (`sinfo type=: was empty locally, filled from peer (${peerType})`);
		} else if (!peerType && localType) {
			sinfoPeerFill = localType;
			log.push (`sinfo type=: was empty on peer, filled from local (${localType})`);
		} else if (localType && peerType && localType !== peerType) {
			sinfoConflict = { localType, peerType };
		}
	}

	// Apply unambiguous fills - writes, no confirmation needed (see file header).
	if ([...localFill.keys ()].length) {
		localFill.set ('id', currentId);
		const r = await fetchJson (`api/db/target-save?${localFill.toString ()}`);
		if (r.ok) {
			currentTarget = r.body;
			populateFormFromTarget (currentTarget);
		} else {
			log.push (`local auto-fill FAILED: ${r.body.error || 'error'}`);
		}
	}
	if (sinfoLocalFill !== null) {
		const merged = parseSinfo (currentSinfo);
		merged.type = sinfoLocalFill;
		const newSinfo = serializeSinfo (merged);
		const r = await fetchJson (`api/db/scheduling-save?id=${encodeURIComponent (currentId)}&sinfo=${encodeURIComponent (newSinfo)}`);
		if (r.ok) {
			currentSinfo = r.body.sinfo;
			document.getElementById ('f-sinfo').value = currentSinfo;
		} else {
			log.push (`local sinfo auto-fill FAILED: ${r.body.error || 'error'}`);
		}
	}
	if ([...peerFill.keys ()].length) {
		const prefix = peerPrefix ();
		peerFill.set ('id', currentId);
		try {
			const r = await fetchJson (`${prefix}/api/db/target-save?${peerFill.toString ()}`);
			if (r.ok)
				peerTarget = r.body;
			else
				log.push (`peer auto-fill FAILED: ${r.body.error || 'error'}`);
		} catch (e) {
			log.push (`peer auto-fill FAILED: ${e}`);
		}
	}
	if (sinfoPeerFill !== null) {
		const prefix = peerPrefix ();
		try {
			const pr = await fetchJson (`${prefix}/api/db/scheduling?id=${encodeURIComponent (currentId)}`);
			const merged = pr.ok ? parseSinfo (pr.body.sinfo) : {};
			merged.type = sinfoPeerFill;
			const r = await fetchJson (`${prefix}/api/db/scheduling-save?id=${encodeURIComponent (currentId)}&sinfo=${encodeURIComponent (serializeSinfo (merged))}`);
			if (r.ok)
				peerSinfo = r.body.sinfo;
			else
				log.push (`peer sinfo auto-fill FAILED: ${r.body.error || 'error'}`);
		} catch (e) {
			log.push (`peer sinfo auto-fill FAILED: ${e}`);
		}
	}

	// Conflicts: never auto-written. Only override the *display* here, and
	// only on the non-SBT side - see file header for the full rule.
	for (const c of conflicts) {
		const sbtValue = iAmSbt ? c.lv : c.pv;
		if (!iAmSbt) {
			writeFormValue (c.d, sbtValue);
			log.push (`${c.d.label}: CONFLICT (local=${c.lv}, SBT=${c.pv}) - form now shows the SBT value; Save to apply it here`);
		} else {
			log.push (`${c.d.label}: CONFLICT (local/SBT=${c.lv}, D50=${c.pv}) - D50 not touched`);
		}
	}
	if (sinfoConflict) {
		const sbtType = iAmSbt ? sinfoConflict.localType : sinfoConflict.peerType;
		if (!iAmSbt) {
			const cur = parseSinfo (document.getElementById ('f-sinfo').value);
			cur.type = sbtType;
			document.getElementById ('f-sinfo').value = serializeSinfo (cur);
			log.push (`sinfo type=: CONFLICT (local=${sinfoConflict.localType}, SBT=${sinfoConflict.peerType}) - form now shows the SBT value; Save scheduling to apply it here`);
		} else {
			log.push (`sinfo type=: CONFLICT (local/SBT=${sinfoConflict.localType}, D50=${sinfoConflict.peerType}) - D50 not touched`);
		}
	}

	peerLogEl.innerHTML = log.length ? ('<ul>' + log.map ((l) => `<li>${escapeHtml (l)}</li>`).join ('') + '</ul>') : '<p class="hint">Everything checked is already in sync.</p>';
	renderPeerCompare ();
}

function renderPeerCompare () {
	if (!currentTarget || !peerTarget) {
		peerCompareEl.innerHTML = '';
		return;
	}

	const typeMatches = currentTarget.hasPosition === peerTarget.hasPosition && currentTarget.isElliptical === peerTarget.isElliptical;
	const descriptors = typeMatches ? getSyncFieldDescriptors (currentTarget) : getSyncFieldDescriptors (currentTarget).filter ((d) => d.key !== 'info' && d.key !== 'ra' && d.key !== 'dec' && d.key !== 'pmRa' && d.key !== 'pmDec');

	const rows = [];
	for (const d of descriptors) {
		// Read the *form's* current value for local (reflects any
		// conflict override just applied), the peer object's for peer.
		const lv = readFormValue (d);
		const pv = d.get (peerTarget);
		const differs = d.numeric ? !nearlyEqual (lv, pv) : String (lv ?? '') !== String (pv ?? '');
		rows.push ({ label: d.label, localVal: lv, peerVal: pv, differs });
	}

	const localType = parseSinfo (document.getElementById ('f-sinfo').value).type;
	const peerType = peerSinfo !== null ? parseSinfo (peerSinfo).type : undefined;
	rows.push ({
		label: 'sinfo type=',
		localVal: localType || '(none)',
		peerVal: peerSinfo === null ? '?' : (peerType || '(none)'),
		differs: peerSinfo !== null && (localType || '') !== (peerType || ''),
	});

	// Shown for context only - deliberately not compared/synced.
	rows.push ({ label: 'Enabled (not synced)', localVal: currentTarget.enabled, peerVal: peerTarget.enabled, differs: false, info: true });
	rows.push ({ label: 'Bonus (not synced)', localVal: currentTarget.bonus, peerVal: peerTarget.bonus, differs: false, info: true });

	const cols = currentSite === 'sbt' ? ['Local (SBT)', 'Peer (D50)'] : ['Local (D50)', 'Peer (SBT)'];
	let html = `<table class="peer-compare-table"><thead><tr><th>Field</th><th>${cols[0]}</th><th>${cols[1]}</th></tr></thead><tbody>`;
	for (const r of rows) {
		const cls = r.differs ? 'diff' : (r.info ? 'info-row' : '');
		html += `<tr class="${cls}"><td>${escapeHtml (r.label)}</td><td>${escapeHtml (r.localVal)}</td><td>${escapeHtml (r.peerVal)}</td></tr>`;
	}
	html += '</tbody></table>';
	peerCompareEl.innerHTML = html;
}

async function pushSyncedFieldsToPeer () {
	const prefix = peerPrefix ();
	if (!prefix || !currentTarget)
		return;

	const descriptors = getSyncFieldDescriptors (currentTarget);
	const params = new URLSearchParams ();
	params.set ('id', currentId);
	for (const d of descriptors) {
		const v = d.get (currentTarget);
		if (!d.isEmpty (v))
			params.set (d.param, d.bool ? (v ? '1' : '0') : v);
	}

	try {
		const r = await fetchJson (`${prefix}/api/db/target-save?${params.toString ()}`);
		if (r.ok) {
			peerTarget = r.body;
			setStatus (peerStatusEl, true, 'saved locally and pushed to peer');
		} else {
			setStatus (peerStatusEl, false, `saved locally, but peer push failed: ${r.body.error || 'error'}`);
		}
	} catch (e) {
		setStatus (peerStatusEl, false, `saved locally, but peer is unreachable: ${e}`);
	}
}

async function pushSinfoTypeToPeer () {
	const prefix = peerPrefix ();
	if (!prefix)
		return;

	const localType = parseSinfo (currentSinfo).type;
	try {
		const pr = await fetchJson (`${prefix}/api/db/scheduling?id=${encodeURIComponent (currentId)}`);
		const merged = pr.ok ? parseSinfo (pr.body.sinfo) : {};
		if (localType)
			merged.type = localType;
		else
			delete merged.type;
		const r = await fetchJson (`${prefix}/api/db/scheduling-save?id=${encodeURIComponent (currentId)}&sinfo=${encodeURIComponent (serializeSinfo (merged))}`);
		if (r.ok) {
			peerSinfo = r.body.sinfo;
			setStatus (schedulingStatusEl, true, 'saved locally and pushed type= to peer');
		} else {
			setStatus (schedulingStatusEl, false, `saved locally, but peer push failed: ${r.body.error || 'error'}`);
		}
	} catch (e) {
		setStatus (schedulingStatusEl, false, `saved locally, but peer is unreachable: ${e}`);
	}
}

// Support ?id=N in the URL so a link (e.g. from a future target-list
// view) can jump straight to a target instead of requiring the id to be
// typed in by hand.
document.getElementById ('peer-site').value = currentSite;
document.getElementById ('peer-prefix').value = peerPrefix ();
const preselectId = new URLSearchParams (location.search).get ('id');
if (preselectId) {
	document.getElementById ('load-id').value = preselectId;
	loadTarget (preselectId);
}
