// RTS2 target editor - vanilla JS, no build step, no external
// dependencies. Loaded via a plain relative <script src="target.js">, so
// fetch('api/...') calls below resolve correctly under any mount depth
// (see app.js's basePath() comment / STATUS.md's proxy-subpath note) -
// this page never constructs a WebSocket URL, so that's the only thing
// relative fetch() needs to get right here.
//
// This page presents D50+SBT as one unified facility with one target
// database (STATUS.md task 10 phase 4, per the user's own framing) even
// though it's actually backed by two independent Postgres databases that
// have to be kept in sync - "This is not SBT, this is SBT+D50 unified".
// Neither daemon knows the other exists; this page reaches whichever
// telescope it isn't physically served by through a same-origin Apache
// proxy path (e.g. "/d50" on lascaux's vhost, "/sbt" on d50's).
//
// Three boxes, deliberately not full-width (they lay out side by side on
// a wide screen, wrapping on a narrower one - see .target-boxes in
// style.css):
//   - What: fields genuinely shared between both telescopes (name,
//     comment, position-or-MPC-line, info). Reconciled the same way
//     phase 2/3 did: a value missing on one side is filled from the
//     other immediately; a value present-and-different on both sides is
//     never auto-written - SBT's value is shown here for review, and a
//     Save writes the resolved value to *both* sides (there's no "local"
//     side any more - this box represents the one target, not one
//     telescope's copy of it).
//   - How: per-camera scripts. Never synced - D50 and SBT don't share
//     cameras (D50: C0/C1, SBT: C1/C2/C3 - "C1" is two unrelated cameras
//     that happen to share a name).
//   - When: genuinely per-telescope scheduling - which telescope(s) are
//     enabled, each one's priority and reservation duration (both
//     deliberately never synced/reconciled - a bigger and a smaller
//     telescope legitimately want different priority/duration for the
//     same target), plus one shared "request type" (scheduling.sinfo's
//     `type=` key - oneof/and/sim, written identically to both sides).
//
// Dropped from earlier phases per explicit direction: bonus (internal,
// not user-facing), interruptible (never used in practice, everything
// is interruptible), and every sinfo key except duration=/type= (the
// rest - mag=/snr=/filters=/count=/pscale= - is unused in production).

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

/** Build a same-origin-relative or peer-prefixed URL for `path`
 * ("api/db/...", no leading slash) depending on whether `site` is the
 * telescope this page is physically served by. */
function apiUrl (site, path) {
	if (site === currentSite)
		return path;
	const prefix = peerPrefix ();
	return prefix ? `${prefix}/${path}` : path;
}

// --- Shared state ------------------------------------------------------------

let currentId = null;
// siteData.d50 / .sbt: { exists, target, sinfo, error }. Populated fresh
// on every loadTarget() - see loadSiteData().
let siteData = { d50: { exists: false }, sbt: { exists: false } };

const loadStatusEl = document.getElementById ('load-status');
const tarIdLabelEl = document.getElementById ('tar-id-label');
const targetBoxesEl = document.getElementById ('target-boxes');
const saveStatusEl = document.getElementById ('save-status');
const whenStatusEl = document.getElementById ('when-status');
const whatLogEl = document.getElementById ('what-log');

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

async function fetchJson (url) {
	const res = await fetch (url);
	const body = await res.json ();
	return { ok: res.ok, body };
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

// --- Load: fetch both telescopes, unconditionally ---------------------------

async function loadSiteData (site, id) {
	try {
		const r = await fetchJson (apiUrl (site, `api/db/target?id=${encodeURIComponent (id)}`));
		if (!r.ok)
			return { exists: false, error: r.body.error || 'not found' };
		let sinfo = '';
		try {
			const sr = await fetchJson (apiUrl (site, `api/db/scheduling?id=${encodeURIComponent (id)}`));
			if (sr.ok)
				sinfo = sr.body.sinfo || '';
		} catch (e) {
			// non-fatal - target data is what matters most
		}
		return { exists: true, target: r.body, sinfo };
	} catch (e) {
		return { exists: false, error: `unreachable: ${e}` };
	}
}

async function loadTarget (id) {
	setStatus (loadStatusEl, true, 'loading…');
	targetBoxesEl.hidden = true;
	tarIdLabelEl.hidden = true;

	const [d50, sbt] = await Promise.all ([loadSiteData ('d50', id), loadSiteData ('sbt', id)]);
	siteData = { d50, sbt };
	currentId = id;

	if (!d50.exists && !sbt.exists) {
		setStatus (loadStatusEl, false, `target ${id} not found at D50 (${d50.error}) or SBT (${sbt.error})`);
		return;
	}

	setStatus (loadStatusEl, true, `loaded target ${id}`);
	tarIdLabelEl.hidden = false;
	tarIdLabelEl.textContent = `Target #${id}`;
	targetBoxesEl.hidden = false;

	document.getElementById ('peer-site').value = currentSite;
	document.getElementById ('peer-prefix').value = peerPrefix ();

	await reconcileWhatBox ();
	renderWhenBox ();
	await renderScriptsBox ();
}

document.getElementById ('load-form').addEventListener ('submit', (ev) => {
	ev.preventDefault ();
	const id = document.getElementById ('load-id').value;
	if (id)
		loadTarget (id);
});

// --- What box: fields shared between both telescopes ------------------------

/** Descriptors shaped for whichever target JSON is passed - RA/Dec/PM
 * and MPC-vs-freeform info depend on the target's actual type, so this
 * is built fresh from a real dbGetTarget()-shaped object each time,
 * not a static table. */
function getWhatFieldDescriptors (tar) {
	const list = [
		{ key: 'name', label: 'Name', param: 'name', get: (t) => t.name || '', isEmpty: (v) => v === '', formEl: () => document.getElementById ('f-name') },
		{ key: 'comment', label: 'Comment', param: 'comment', get: (t) => t.comment || '', isEmpty: (v) => v === '', formEl: () => document.getElementById ('f-comment') },
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

function writeFormValue (d, value) {
	d.formEl ().value = value ?? '';
}

async function reconcileWhatBox () {
	const d50 = siteData.d50, sbt = siteData.sbt;
	const base = sbt.exists ? sbt.target : d50.target;
	const log = [];

	document.getElementById ('f-type').value = base.type;
	document.getElementById ('position-fields').hidden = !base.hasPosition;
	document.getElementById ('mpec-label').hidden = !base.isElliptical;
	document.getElementById ('info-label').hidden = base.isElliptical;

	const bothExist = d50.exists && sbt.exists;
	const shapesMatch = bothExist && d50.target.hasPosition === sbt.target.hasPosition && d50.target.isElliptical === sbt.target.isElliptical;

	if (shapesMatch) {
		const descriptors = getWhatFieldDescriptors (base);
		const d50Fill = new URLSearchParams ();
		const sbtFill = new URLSearchParams ();

		for (const d of descriptors) {
			const dv = d.get (d50.target);
			const sv = d.get (sbt.target);
			const dEmpty = d.isEmpty (dv);
			const sEmpty = d.isEmpty (sv);

			if (dEmpty && !sEmpty) {
				d50Fill.set (d.param, sv);
				writeFormValue (d, sv);
				log.push (`${d.label}: missing at D50, filled from SBT`);
			} else if (sEmpty && !dEmpty) {
				sbtFill.set (d.param, dv);
				writeFormValue (d, dv);
				log.push (`${d.label}: missing at SBT, filled from D50`);
			} else if (!dEmpty && !sEmpty) {
				const differs = d.numeric ? !nearlyEqual (dv, sv) : String (dv) !== String (sv);
				writeFormValue (d, sv); // SBT is authoritative for display on a genuine conflict
				if (differs)
					log.push (`${d.label}: differs (D50=${dv}, SBT=${sv}) - showing SBT value here, Save to apply it to D50 too`);
			} else {
				writeFormValue (d, '');
			}
		}

		if ([...d50Fill.keys ()].length) {
			d50Fill.set ('id', currentId);
			const r = await fetchJson (apiUrl ('d50', `api/db/target-save?${d50Fill.toString ()}`));
			if (r.ok)
				d50.target = r.body;
			else
				log.push (`D50 auto-fill FAILED: ${r.body.error || 'error'}`);
		}
		if ([...sbtFill.keys ()].length) {
			sbtFill.set ('id', currentId);
			const r = await fetchJson (apiUrl ('sbt', `api/db/target-save?${sbtFill.toString ()}`));
			if (r.ok)
				sbt.target = r.body;
			else
				log.push (`SBT auto-fill FAILED: ${r.body.error || 'error'}`);
		}
	} else {
		for (const d of getWhatFieldDescriptors (base))
			writeFormValue (d, d.get (base));
		if (!d50.exists)
			log.push (`Not present at D50 (${d50.error}).`);
		if (!sbt.exists)
			log.push (`Not present at SBT (${sbt.error}).`);
		if (bothExist && !shapesMatch)
			log.push ('Target type/position shape differs between D50 and SBT - not attempting field-level comparison.');
	}

	whatLogEl.innerHTML = log.length ? ('<ul>' + log.map ((l) => `<li>${escapeHtml (l)}</li>`).join ('') + '</ul>') : '';
}

function readWhatFormParams () {
	const params = new URLSearchParams ();
	params.set ('name', document.getElementById ('f-name').value);
	params.set ('comment', document.getElementById ('f-comment').value);

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

	const params = readWhatFormParams ();
	params.set ('id', currentId);
	const results = [];

	// A 401 here triggers the browser's own native credential prompt
	// (WWW-Authenticate: Basic) exactly like app.js's command form - see
	// web/STATUS.md task 6, no credential handling needed here. The peer
	// side (if reached through the proxy) prompts separately the first
	// time, same reasoning.
	for (const site of ['d50', 'sbt']) {
		if (!siteData[site].exists)
			continue;
		try {
			const r = await fetchJson (apiUrl (site, `api/db/target-save?${params.toString ()}`));
			if (r.ok) {
				siteData[site].target = r.body;
				results.push (`${SITES[site].label}: saved`);
			} else {
				results.push (`${SITES[site].label}: ${r.body.error || 'error'}`);
			}
		} catch (e) {
			results.push (`${SITES[site].label}: unreachable (${e})`);
		}
	}

	setStatus (saveStatusEl, results.every ((r) => r.includes ('saved')), results.join (' / '));
	whatLogEl.innerHTML = '';
});

// --- How box: per-camera scripts, per telescope, never synced ---------------

async function renderScriptsBox () {
	const listEl = document.getElementById ('scripts-list');
	listEl.textContent = '';

	for (const site of ['d50', 'sbt']) {
		const groupLabel = document.createElement ('div');
		groupLabel.className = 'script-group-label';
		groupLabel.textContent = SITES[site].label;
		listEl.appendChild (groupLabel);

		if (!siteData[site].exists) {
			const note = document.createElement ('p');
			note.className = 'hint';
			note.textContent = `not present at ${SITES[site].label} (${siteData[site].error})`;
			listEl.appendChild (note);
			continue;
		}

		let overrides = {};
		try {
			const r = await fetchJson (apiUrl (site, `api/db/scripts?id=${encodeURIComponent (currentId)}`));
			if (r.ok)
				overrides = r.body.scripts || {};
		} catch (e) {
			// leave overrides empty - rows still render, just can't show what's saved
		}

		for (const cam of SITES[site].cameras) {
			const hasOverride = Object.prototype.hasOwnProperty.call (overrides, cam);
			const hint = SITES[site].defaultScriptHints[cam];

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

			saveBtn.addEventListener ('click', () => saveScript (site, cam, input, statusEl));
			defaultBtn.addEventListener ('click', () => deleteScript (site, cam, input, defaultBtn, hint, statusEl));

			row.append (nameEl, input, defaultBtn, saveBtn, statusEl);
			listEl.appendChild (row);
		}
	}
}

async function saveScript (site, camera, input, statusEl) {
	try {
		const r = await fetchJson (apiUrl (site, `api/db/script-save?id=${encodeURIComponent (currentId)}&camera=${encodeURIComponent (camera)}&script=${encodeURIComponent (input.value)}`));
		if (!r.ok) {
			setStatus (statusEl, false, r.body.error || `${r.status}`);
			return;
		}
		setStatus (statusEl, true, 'saved');
	} catch (e) {
		setStatus (statusEl, false, String (e));
	}
}

async function deleteScript (site, camera, input, defaultBtn, hint, statusEl) {
	try {
		const r = await fetchJson (apiUrl (site, `api/db/script-delete?id=${encodeURIComponent (currentId)}&camera=${encodeURIComponent (camera)}`));
		if (!r.ok) {
			setStatus (statusEl, false, r.body.error || `${r.status}`);
			return;
		}
		input.value = '';
		defaultBtn.className = 'script-default-btn active';
		setStatus (statusEl, true, 'cleared - using default');
	} catch (e) {
		setStatus (statusEl, false, String (e));
	}
}

// --- When box: per-telescope enabled/priority/duration, shared type= -------

function renderWhenBox () {
	const container = document.getElementById ('telescope-rows');
	container.textContent = '';

	for (const site of ['d50', 'sbt']) {
		const d = siteData[site];
		const row = document.createElement ('div');
		row.className = 'telescope-row' + (d.exists ? '' : ' unavailable');

		const nameWrap = document.createElement ('div');
		nameWrap.className = 'telescope-name';
		const cb = document.createElement ('input');
		cb.type = 'checkbox';
		cb.id = `f-enabled-${site}`;
		cb.disabled = !d.exists;
		if (d.exists)
			cb.checked = !!d.target.enabled;
		nameWrap.append (cb, document.createTextNode (' ' + SITES[site].label));

		const prLabel = document.createElement ('label');
		prLabel.textContent = 'Priority';
		const prInput = document.createElement ('input');
		prInput.type = 'number';
		prInput.step = 'any';
		prInput.id = `f-priority-${site}`;
		prInput.disabled = !d.exists;
		if (d.exists)
			prInput.value = d.target.priority ?? '';
		prLabel.appendChild (prInput);

		const durLabel = document.createElement ('label');
		durLabel.textContent = 'Duration (s)';
		const durInput = document.createElement ('input');
		durInput.type = 'number';
		durInput.step = '1';
		durInput.min = '0';
		durInput.id = `f-duration-${site}`;
		durInput.disabled = !d.exists;
		if (d.exists)
			durInput.value = parseSinfo (d.sinfo).duration ?? '';
		durLabel.appendChild (durInput);

		row.append (nameWrap, prLabel, durLabel);

		if (!d.exists) {
			const note = document.createElement ('span');
			note.className = 'hint';
			note.textContent = `not present (${d.error})`;
			row.appendChild (note);
		}

		container.appendChild (row);
	}

	// Request type: whichever side already has it (SBT preferred if both
	// somehow disagree - same authority rule as the What box).
	const sbtType = siteData.sbt.exists ? parseSinfo (siteData.sbt.sinfo).type : undefined;
	const d50Type = siteData.d50.exists ? parseSinfo (siteData.d50.sinfo).type : undefined;
	const currentType = sbtType || d50Type || '';
	const select = document.getElementById ('f-request-type');
	select.value = ['', 'and', 'sim'].includes (currentType) ? currentType : '';
}

document.getElementById ('save-when').addEventListener ('click', async () => {
	if (currentId === null)
		return;

	const requestType = document.getElementById ('f-request-type').value;
	const results = [];

	for (const site of ['d50', 'sbt']) {
		if (!siteData[site].exists)
			continue;

		const enabled = document.getElementById (`f-enabled-${site}`).checked;
		const priority = document.getElementById (`f-priority-${site}`).value || '0';
		const params = new URLSearchParams ();
		params.set ('id', currentId);
		params.set ('enabled', enabled ? '1' : '0');
		params.set ('priority', priority);
		try {
			const r = await fetchJson (apiUrl (site, `api/db/target-save?${params.toString ()}`));
			if (r.ok)
				siteData[site].target = r.body;
			else
				results.push (`${SITES[site].label}: ${r.body.error || 'error'}`);
		} catch (e) {
			results.push (`${SITES[site].label}: unreachable (${e})`);
		}

		const duration = document.getElementById (`f-duration-${site}`).value;
		const merged = parseSinfo (siteData[site].sinfo);
		if (duration)
			merged.duration = duration;
		else
			delete merged.duration;
		if (requestType)
			merged.type = requestType;
		else
			delete merged.type;

		try {
			const r = await fetchJson (apiUrl (site, `api/db/scheduling-save?id=${encodeURIComponent (currentId)}&sinfo=${encodeURIComponent (serializeSinfo (merged))}`));
			if (r.ok)
				siteData[site].sinfo = r.body.sinfo;
			else
				results.push (`${SITES[site].label} scheduling: ${r.body.error || 'error'}`);
		} catch (e) {
			results.push (`${SITES[site].label} scheduling: unreachable (${e})`);
		}
	}

	setStatus (whenStatusEl, results.length === 0, results.length ? results.join (' / ') : 'saved to both telescopes');
});

// --- Advanced settings (telescope identity / peer path) ---------------------

document.getElementById ('peer-config-form').addEventListener ('submit', (ev) => {
	ev.preventDefault ();
	currentSite = document.getElementById ('peer-site').value;
	localStorage.setItem (SITE_KEY, currentSite);
	const val = document.getElementById ('peer-prefix').value.trim ().replace (/\/$/, '');
	localStorage.setItem (PEER_PREFIX_KEY, val);
	document.getElementById ('peer-prefix').value = peerPrefix ();
	if (currentId !== null)
		loadTarget (currentId);
});

document.getElementById ('peer-site').value = currentSite;
document.getElementById ('peer-prefix').value = peerPrefix ();

// Support ?id=N in the URL so a link (e.g. from a future target-list
// view) can jump straight to a target instead of requiring the id to be
// typed in by hand.
const preselectId = new URLSearchParams (location.search).get ('id');
if (preselectId) {
	document.getElementById ('load-id').value = preselectId;
	loadTarget (preselectId);
}
