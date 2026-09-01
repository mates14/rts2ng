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

function otherSite () {
	return currentSite === 'sbt' ? 'd50' : 'sbt';
}

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

// --- RA/Dec sexagesimal display/parsing --------------------------------
//
// Cosmetic per the user's own request - decimal-degree RA/Dec with a
// locale comma decimal separator (this session already found <input
// type=number> renders that way on a comma-decimal system - see phase 1)
// is hard to read at a glance. Storage/the API stay decimal degrees
// throughout (rts2db::ConstTarget's position is degrees) - only the
// display and the user's typed input go through these.

function decToSexagesimal (deg, isRa, secDecimals) {
	if (deg === null || deg === undefined || typeof deg !== 'number' || isNaN (deg))
		return '';
	const sign = deg < 0 ? '-' : (isRa ? '' : '+');
	let a = Math.abs (deg);
	if (isRa)
		a /= 15; // degrees -> hours
	let h = Math.floor (a);
	let mFull = (a - h) * 60;
	let m = Math.floor (mFull);
	let s = (mFull - m) * 60;

	// Round to secDecimals and carry into m/h if that rounds s up to 60
	// (and m up to 60) - a plain toFixed() alone can print "60.0".
	const factor = Math.pow (10, secDecimals);
	s = Math.round (s * factor) / factor;
	if (s >= 60) { s -= 60; m += 1; }
	if (m >= 60) { m -= 60; h += 1; }

	const pad2 = (n) => String (n).padStart (2, '0');
	const sWidth = secDecimals > 0 ? secDecimals + 3 : 2;
	return `${sign}${pad2 (h)}:${pad2 (m)}:${s.toFixed (secDecimals).padStart (sWidth, '0')}`;
}

/** Parse either sexagesimal ("12:34:56.7", "12 34 56.7", "-12 34 56,7")
 * or plain decimal ("20.9208", "20,9208") - whichever the text looks
 * like. Returns degrees, or null if unparseable/empty. `isRa` scales a
 * sexagesimal reading from hours to degrees (*15); plain decimal input
 * is assumed to already be degrees either way, since nobody types RA as
 * decimal hours by hand. */
function parseCoordinate (str, isRa) {
	str = (str || '').trim ();
	if (str === '')
		return null;

	const sex = str.match (/^([+-]?)\s*(\d+)[:\s]+(\d+)[:\s]+([\d.,]+)$/);
	if (sex) {
		const sign = sex[1] === '-' ? -1 : 1;
		const h = parseFloat (sex[2]);
		const m = parseFloat (sex[3]);
		const s = parseFloat (sex[4].replace (',', '.'));
		if (isNaN (h) || isNaN (m) || isNaN (s))
			return null;
		let val = h + m / 60 + s / 3600;
		if (isRa)
			val *= 15;
		return sign * val;
	}

	const val = parseFloat (str.replace (',', '.'));
	return isNaN (val) ? null : val;
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
	document.getElementById ('sky-panel').hidden = true;
	document.getElementById ('year-panel').hidden = true;
	document.getElementById ('create-type-row').hidden = true;

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
	await loadVisibility ();
	await loadYearVisibility ();
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
	if (d.key === 'ra')
		d.formEl ().value = decToSexagesimal (value, true, 3);
	else if (d.key === 'dec')
		d.formEl ().value = decToSexagesimal (value, false, 1);
	else
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
		const ra = parseCoordinate (document.getElementById ('f-ra').value, true);
		const dec = parseCoordinate (document.getElementById ('f-dec').value, false);
		params.set ('ra', ra ?? '0');
		params.set ('dec', dec ?? '0');
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
	const results = [];

	// A 401 here triggers the browser's own native credential prompt
	// (WWW-Authenticate: Basic) exactly like app.js's command form - see
	// web/STATUS.md task 6, no credential handling needed here. The peer
	// side (if reached through the proxy) prompts separately the first
	// time, same reasoning.
	for (const site of ['d50', 'sbt']) {
		const cb = document.getElementById (`f-enabled-${site}`);

		if (siteData[site].exists) {
			const p = new URLSearchParams (params);
			p.set ('id', currentId);
			try {
				const r = await fetchJson (apiUrl (site, `api/db/target-save?${p.toString ()}`));
				if (r.ok) {
					siteData[site].target = r.body;
					results.push (`${SITES[site].label}: saved`);
				} else {
					results.push (`${SITES[site].label}: ${r.body.error || 'error'}`);
				}
			} catch (e) {
				results.push (`${SITES[site].label}: unreachable (${e})`);
			}
		} else if (cb && cb.checked) {
			// Not present there yet, but checked in the When box - create
			// it there now with the current What box values (see
			// target.html's When-box hint). Type is inferred from the
			// other side if it exists, otherwise from the "new target"
			// type picker.
			const type = document.getElementById ('create-type-row').hidden
				? (document.getElementById ('mpec-label').hidden ? 'equatorial' : 'elliptical')
				: document.getElementById ('f-create-type').value;
			const p = new URLSearchParams (params);
			p.set ('id', currentId);
			p.set ('type', type);
			p.set ('enabled', '1');
			const prInput = document.getElementById (`f-priority-${site}`);
			if (prInput && prInput.value !== '')
				p.set ('priority', prInput.value);
			try {
				const r = await fetchJson (apiUrl (site, `api/db/target-create?${p.toString ()}`));
				if (r.ok) {
					siteData[site] = { exists: true, target: r.body, sinfo: '' };
					results.push (`${SITES[site].label}: created`);
				} else {
					results.push (`${SITES[site].label}: create failed - ${r.body.error || 'error'}`);
				}
			} catch (e) {
				results.push (`${SITES[site].label}: unreachable (${e})`);
			}
		}
	}

	setStatus (saveStatusEl, results.every ((r) => /saved|created/.test (r)), results.join (' / '));
	document.getElementById ('create-type-row').hidden = true;
	if (siteData.d50.exists || siteData.sbt.exists)
		await reconcileWhatBox (); // re-renders the form from confirmed post-save state (e.g. RA/Dec back to sexagesimal display)
	renderWhenBox ();
	await renderScriptsBox ();
});

// --- New target -------------------------------------------------------------

document.getElementById ('new-target-btn').addEventListener ('click', async () => {
	setStatus (loadStatusEl, true, 'reserving a new target id…');

	let id = null;
	for (let attempt = 0; attempt < 8 && id === null; attempt++) {
		let candidate;
		try {
			const r = await fetchJson (apiUrl (currentSite, 'api/db/new-target-id'));
			if (!r.ok) {
				setStatus (loadStatusEl, false, `cannot reserve an id: ${r.body.error || 'error'}`);
				return;
			}
			candidate = r.body.id;
		} catch (e) {
			setStatus (loadStatusEl, false, `cannot reserve an id: ${e}`);
			return;
		}

		// tar_id sequences are independent per database - a fresh id from
		// this site's sequence could already be a real, unrelated target
		// on the other site. Check, and draw again if so (see
		// STATUS.md task 10 phase 5).
		try {
			const r = await fetchJson (apiUrl (otherSite (), `api/db/target?id=${candidate}`));
			if (!r.ok)
				id = candidate; // not found there either - safe on both sides
		} catch (e) {
			id = candidate; // other site unreachable - can't check, proceed with what we have
		}
	}

	if (id === null) {
		setStatus (loadStatusEl, false, 'could not find an id free on both telescopes after several tries - try again');
		return;
	}

	document.getElementById ('load-id').value = id;
	currentId = id;
	siteData = { d50: { exists: false }, sbt: { exists: false } };

	setStatus (loadStatusEl, true, `new target - id ${id} reserved (not created until you Save)`);
	tarIdLabelEl.hidden = false;
	tarIdLabelEl.textContent = `Target #${id} (new)`;
	targetBoxesEl.hidden = false;

	document.getElementById ('f-name').value = '';
	document.getElementById ('f-type').value = '';
	document.getElementById ('f-comment').value = '';
	document.getElementById ('f-info').value = '';
	document.getElementById ('f-mpec').value = '';
	document.getElementById ('f-ra').value = '';
	document.getElementById ('f-dec').value = '';
	document.getElementById ('f-pmra').value = '';
	document.getElementById ('f-pmdec').value = '';

	document.getElementById ('create-type-row').hidden = false;
	document.getElementById ('f-create-type').value = 'equatorial';
	document.getElementById ('position-fields').hidden = false;
	document.getElementById ('mpec-label').hidden = true;
	document.getElementById ('info-label').hidden = false;

	whatLogEl.innerHTML = '';
	renderWhenBox ();
	await renderScriptsBox ();
});

document.getElementById ('f-create-type').addEventListener ('change', (ev) => {
	const elliptical = ev.target.value === 'elliptical';
	document.getElementById ('position-fields').hidden = elliptical;
	document.getElementById ('mpec-label').hidden = !elliptical;
	document.getElementById ('info-label').hidden = elliptical;
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

			const input = document.createElement ('textarea');
			input.rows = 2;
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

			row.append (nameEl, defaultBtn, saveBtn, input, statusEl);
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
		const unreachable = !d.exists && d.error && d.error.startsWith ('unreachable');
		const row = document.createElement ('div');
		row.className = 'telescope-row' + (unreachable ? ' unavailable' : '');

		const nameWrap = document.createElement ('div');
		nameWrap.className = 'telescope-name';
		const cb = document.createElement ('input');
		cb.type = 'checkbox';
		cb.id = `f-enabled-${site}`;
		cb.disabled = unreachable;
		if (d.exists)
			cb.checked = !!d.target.enabled;
		nameWrap.append (cb, document.createTextNode (' ' + SITES[site].label));

		const prLabel = document.createElement ('label');
		prLabel.textContent = 'Priority';
		const prInput = document.createElement ('input');
		prInput.type = 'number';
		prInput.step = 'any';
		prInput.id = `f-priority-${site}`;
		prInput.disabled = unreachable;
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
		durInput.disabled = unreachable;
		if (d.exists)
			durInput.value = parseSinfo (d.sinfo).duration ?? '';
		durLabel.appendChild (durInput);

		row.append (nameWrap, prLabel, durLabel);

		if (!d.exists) {
			const note = document.createElement ('span');
			note.className = 'hint';
			note.textContent = unreachable ? `unreachable (${d.error})` : 'not present - check the box and Save target to create it here';
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
		if (!siteData[site].exists) {
			if (document.getElementById (`f-enabled-${site}`).checked)
				results.push (`${SITES[site].label}: not created yet - click Save target first`);
			continue;
		}

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

// --- Visibility plot ---------------------------------------------------------
//
// The staralt-style night plot classic produced with `rts2-targetinfo -g`
// (which printed gnuplot code to pipe into gnuplot). Everything
// astronomical is computed by the daemon - /api/db/target-altitude walks
// centrald's own next_event() state machine for the night boundaries and
// rts2db::Target::getAltAz() for the trace, so a moving target traces its
// real path and nothing here re-derives an ephemeris the backend already
// knows. This function only draws.
//
// The horizon curve is the horizon at the target's azimuth at each
// moment, which is the whole reason the plot is worth drawing per target:
// where the trace meets it is where this telescope loses the object.

const skyPanelEl = document.getElementById ('sky-panel');
const skyCanvas = document.getElementById ('sky-chart');
const skyTooltipEl = document.getElementById ('sky-tooltip');
const skyStatusEl = document.getElementById ('sky-status');
const skySummaryEl = document.getElementById ('sky-summary');
const skyLegendEl = document.getElementById ('sky-legend');
const skyDateEl = document.getElementById ('sky-date');

let skyData = null;
let skyPlot = null;

const SKY_PAD = { left: 44, right: 12, top: 12, bottom: 28 };

/** YYYY-MM-DD in local time - what <input type="date"> speaks, and what
 * the endpoint's date= parameter takes. */
function skyDateString (t) {
	const d = new Date (t * 1000);
	return d.getFullYear () + '-' + String (d.getMonth () + 1).padStart (2, '0')
		+ '-' + String (d.getDate ()).padStart (2, '0');
}

function skyTime (t) {
	const d = new Date (t * 1000);
	return String (d.getHours ()).padStart (2, '0') + ':' + String (d.getMinutes ()).padStart (2, '0');
}

/** libnova counts azimuth from south; a person reading a chart wants it
 * from north, same conversion the site monitors' sky charts make. */
function skyCompassAz (azLibnova) {
	return (azLibnova + 180) % 360;
}

async function loadVisibility () {
	if (currentId === null)
		return;
	skyPanelEl.hidden = false;
	setStatus (skyStatusEl, true, 'loading…');

	// One point per canvas pixel is all the plot can resolve, and each
	// one costs the daemon a target/moon/sun position.
	const points = Math.min (600, Math.max (120, Math.round (skyCanvas.clientWidth || 800)));
	const date = skyDateEl.value;
	const url = apiUrl (currentSite, `api/db/target-altitude?id=${encodeURIComponent (currentId)}`
		+ (date ? `&date=${encodeURIComponent (date)}` : '') + `&points=${points}`);

	try {
		// fetchJson() here returns { ok, body } - an API-level error comes
		// back as a normal JSON body with an HTTP status to match, same
		// as everywhere else on this page
		const r = await fetchJson (url);
		if (!r.ok)
			throw new Error (r.body.error || 'request failed');
		const data = r.body;
		skyData = data;
		if (!date)
			skyDateEl.value = skyDateString (data.sunset);
		describeVisibility ();
		setStatus (skyStatusEl, true, `sunset ${skyTime (data.sunset)}, sunrise ${skyTime (data.sunrise)}`
			+ (data.nightStart ? `, RTS2 night ${skyTime (data.nightStart)}–${skyTime (data.nightEnd)} (sun below ${data.nightHorizon}°)` : ''));
	} catch (er) {
		skyData = null;
		skySummaryEl.textContent = '';
		skyLegendEl.innerHTML = '';
		setStatus (skyStatusEl, false, `cannot compute visibility: ${er.message}`);
	}
	drawVisibility ();
}

/** Heading summary and legend: the two questions a plan actually asks -
 * how high does it get, and for how long is it observable at all. */
function describeVisibility () {
	if (!skyData || !skyData.points.length) {
		skySummaryEl.textContent = '';
		skyLegendEl.innerHTML = '';
		return;
	}

	const pts = skyData.points;
	const nightFrom = skyData.nightStart || skyData.sunset;
	const nightTo = skyData.nightEnd || skyData.sunrise;
	const step = pts.length > 1 ? (pts[pts.length - 1][0] - pts[0][0]) / (pts.length - 1) : 0;

	let best = null;
	let visibleSeconds = 0;
	let moonMin = Infinity;
	for (const p of pts) {
		const inNight = p[0] >= nightFrom && p[0] <= nightTo;
		if (!inNight)
			continue;
		if (p[1] > p[3]) {
			visibleSeconds += step;
			if (!best || p[1] > best[1])
				best = p;
		}
		if (p[5] < moonMin)
			moonMin = p[5];
	}

	if (!best) {
		skySummaryEl.textContent = 'never rises above the horizon during the night';
	} else {
		const hours = visibleSeconds / 3600;
		// everything here is measured inside the RTS2 night, not between
		// sunset and sunrise: that is the window the scheduler can
		// actually use, and a peak altitude reached during twilight
		// would be a number nobody can observe at
		skySummaryEl.textContent = `up to ${best[1].toFixed (0)}° at ${skyTime (best[0])} (RTS2 night), `
			+ `observable ${hours.toFixed (1)} h of it, `
			+ `Moon ${(skyData.moonDisk * 100).toFixed (0)}% at ${moonMin.toFixed (0)}°`;
	}

	skyLegendEl.innerHTML = '<span class="k-target">target</span>'
		+ '<span class="k-moon">Moon</span>'
		+ '<span class="k-horizon">horizon at target azimuth</span>'
		+ (skyData.nightStart ? '<span class="k-night">RTS2 night start/end</span>' : '');
}

function drawVisibility () {
	if (!skyCanvas)
		return;
	const ctx = skyCanvas.getContext ('2d');
	const ratio = window.devicePixelRatio || 1;
	const width = skyCanvas.clientWidth;
	const height = skyCanvas.clientHeight;
	skyCanvas.width = Math.round (width * ratio);
	skyCanvas.height = Math.round (height * ratio);
	ctx.setTransform (ratio, 0, 0, ratio, 0, 0);
	ctx.clearRect (0, 0, width, height);
	skyPlot = null;

	const style = getComputedStyle (document.body);
	const fg = style.getPropertyValue ('--text-muted').trim () || '#6b7280';
	const grid = style.getPropertyValue ('--border').trim () || '#dcdfe4';
	const cTarget = style.getPropertyValue ('--sky-target').trim () || '#2563eb';
	const cMoon = style.getPropertyValue ('--sky-moon').trim () || '#b45309';
	const cHorizon = style.getPropertyValue ('--sky-horizon').trim () || '#6b7280';
	const cHorizonFill = style.getPropertyValue ('--sky-horizon-fill').trim () || 'rgba(107,114,128,0.25)';
	const cNight = style.getPropertyValue ('--sky-night-line').trim () || '#15803d';
	const cTwilight = style.getPropertyValue ('--sky-twilight').trim () || 'rgba(37,99,235,0.07)';

	if (!skyData || !skyData.points.length) {
		ctx.fillStyle = fg;
		ctx.font = '13px sans-serif';
		ctx.textAlign = 'center';
		ctx.fillText ('no visibility data', width / 2, height / 2);
		return;
	}

	const pts = skyData.points;
	const x0 = SKY_PAD.left, x1 = width - SKY_PAD.right;
	const y0 = SKY_PAD.top, y1 = height - SKY_PAD.bottom;
	if (x1 <= x0 || y1 <= y0)
		return;

	const tMin = skyData.sunset, tMax = skyData.sunrise;
	// Fixed 0..90: an altitude plot that rescales itself per target
	// invites comparing two nights that are not on the same scale.
	const vMin = 0, vMax = 90;
	const sx = t => x0 + (t - tMin) / (tMax - tMin) * (x1 - x0);
	const sy = v => y1 - (Math.max (vMin, Math.min (vMax, v)) - vMin) / (vMax - vMin) * (y1 - y0);

	// twilight: before RTS2 night starts and after it ends
	if (skyData.nightStart && skyData.nightEnd) {
		ctx.fillStyle = cTwilight;
		ctx.fillRect (x0, y0, sx (skyData.nightStart) - x0, y1 - y0);
		ctx.fillRect (sx (skyData.nightEnd), y0, x1 - sx (skyData.nightEnd), y1 - y0);
	}

	ctx.strokeStyle = grid;
	ctx.fillStyle = fg;
	ctx.lineWidth = 1;
	ctx.font = '11px sans-serif';
	ctx.textAlign = 'right';
	ctx.textBaseline = 'middle';
	for (let v = 0; v <= 90; v += 15) {
		const y = Math.round (sy (v)) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (x0, y);
		ctx.lineTo (x1, y);
		ctx.stroke ();
		ctx.fillText (v + '°', x0 - 6, y);
	}

	// one tick per hour, on the hour
	ctx.textAlign = 'center';
	ctx.textBaseline = 'top';
	const firstHour = Math.ceil (tMin / 3600) * 3600;
	for (let t = firstHour; t <= tMax; t += 3600) {
		const x = Math.round (sx (t)) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (x, y0);
		ctx.lineTo (x, y1);
		ctx.stroke ();
		const label = skyTime (t);
		const half = ctx.measureText (label).width / 2;
		ctx.textAlign = x + half > width ? 'right' : (x - half < 0 ? 'left' : 'center');
		ctx.fillText (label, x, y1 + 4);
	}
	ctx.textAlign = 'center';

	// horizon, filled from the bottom - the ground, effectively
	ctx.fillStyle = cHorizonFill;
	ctx.strokeStyle = cHorizon;
	ctx.lineWidth = 1;
	ctx.beginPath ();
	ctx.moveTo (sx (pts[0][0]), y1);
	for (const p of pts)
		ctx.lineTo (sx (p[0]), sy (p[3]));
	ctx.lineTo (sx (pts[pts.length - 1][0]), y1);
	ctx.closePath ();
	ctx.fill ();
	ctx.beginPath ();
	for (let i = 0; i < pts.length; i++) {
		const x = sx (pts[i][0]), y = sy (pts[i][3]);
		i ? ctx.lineTo (x, y) : ctx.moveTo (x, y);
	}
	ctx.stroke ();

	// Moon, dashed - it is context for the target trace, not a second
	// thing of equal weight
	ctx.strokeStyle = cMoon;
	ctx.setLineDash ([4, 3]);
	ctx.lineWidth = 1.2;
	ctx.beginPath ();
	let pen = false;
	for (const p of pts) {
		if (p[4] < 0) { pen = false; continue; }	 // below the horizon: not drawn, not clamped to 0
		const x = sx (p[0]), y = sy (p[4]);
		pen ? ctx.lineTo (x, y) : ctx.moveTo (x, y);
		pen = true;
	}
	ctx.stroke ();
	ctx.setLineDash ([]);

	// RTS2 night boundaries
	if (skyData.nightStart && skyData.nightEnd) {
		ctx.strokeStyle = cNight;
		ctx.setLineDash ([5, 4]);
		ctx.lineWidth = 1.2;
		for (const t of [skyData.nightStart, skyData.nightEnd]) {
			const x = Math.round (sx (t)) + 0.5;
			ctx.beginPath ();
			ctx.moveTo (x, y0);
			ctx.lineTo (x, y1);
			ctx.stroke ();
		}
		ctx.setLineDash ([]);
	}

	// the target itself, on top of everything
	ctx.strokeStyle = cTarget;
	ctx.lineWidth = 2;
	ctx.beginPath ();
	pen = false;
	for (const p of pts) {
		if (p[1] < 0) { pen = false; continue; }
		const x = sx (p[0]), y = sy (p[1]);
		pen ? ctx.lineTo (x, y) : ctx.moveTo (x, y);
		pen = true;
	}
	ctx.stroke ();

	skyPlot = { x0, x1, y0, y1, tMin, tMax, sx, sy };
}

function onSkyMove (event) {
	if (!skyPlot || !skyData || !skyData.points.length) {
		skyTooltipEl.style.display = 'none';
		return;
	}
	const rect = skyCanvas.getBoundingClientRect ();
	const x = event.clientX - rect.left;
	if (x < skyPlot.x0 || x > skyPlot.x1) {
		skyTooltipEl.style.display = 'none';
		return;
	}

	const t = skyPlot.tMin + (x - skyPlot.x0) / (skyPlot.x1 - skyPlot.x0) * (skyPlot.tMax - skyPlot.tMin);
	let best = null, bestDist = Infinity;
	for (const p of skyData.points) {
		const d = Math.abs (p[0] - t);
		if (d < bestDist) { bestDist = d; best = p; }
	}
	if (!best) {
		skyTooltipEl.style.display = 'none';
		return;
	}

	skyTooltipEl.textContent = `${skyTime (best[0])}  alt ${best[1].toFixed (1)}°  az ${skyCompassAz (best[2]).toFixed (0)}°`
		+ `  horizon ${best[3].toFixed (1)}°  Moon ${best[5].toFixed (0)}° away`;
	skyTooltipEl.style.display = 'block';
	const tipWidth = skyTooltipEl.offsetWidth;
	let left = skyPlot.sx (best[0]) + 12;
	if (left + tipWidth > rect.width)
		left = skyPlot.sx (best[0]) - tipWidth - 12;
	skyTooltipEl.style.left = left + 'px';
	skyTooltipEl.style.top = (skyPlot.sy (Math.max (best[1], best[3])) - 30) + 'px';
}

function shiftSkyDate (days) {
	const base = skyDateEl.value ? new Date (skyDateEl.value + 'T12:00:00') : new Date ();
	base.setDate (base.getDate () + days);
	skyDateEl.value = skyDateString (base.getTime () / 1000);
	loadVisibility ();
}

document.getElementById ('sky-prev').addEventListener ('click', () => shiftSkyDate (-1));
document.getElementById ('sky-next').addEventListener ('click', () => shiftSkyDate (1));
document.getElementById ('sky-today').addEventListener ('click', () => {
	skyDateEl.value = '';
	loadVisibility ();
});
skyDateEl.addEventListener ('change', loadVisibility);
document.getElementById ('sky-form').addEventListener ('submit', (ev) => ev.preventDefault ());

skyCanvas.addEventListener ('mousemove', onSkyMove);
skyCanvas.addEventListener ('mouseleave', () => { skyTooltipEl.style.display = 'none'; });

let skyResizeTimer = null;
window.addEventListener ('resize', () => {
	// redraw only: the sample count follows the width, but refetching on
	// every resize event would make the daemon recompute a few hundred
	// ephemeris positions for nothing
	clearTimeout (skyResizeTimer);
	skyResizeTimer = setTimeout (drawVisibility, 200);
});

// --- Yearly visibility ("butterfly") plot ------------------------------
//
// One point per night of the year, from /api/db/target-visibility-year:
// sunset/twilight/sunrise trace an hourglass that pinches and widens with
// the seasons, and a filled band shows when this target actually clears
// this telescope's real horizon that night - the wing that sweeps across
// the plot over the year as sidereal time drifts against the calendar.

const yearPanelEl = document.getElementById ('year-panel');
const yearCanvas = document.getElementById ('year-chart');
const yearTooltipEl = document.getElementById ('year-tooltip');
const yearStatusEl = document.getElementById ('year-status');
const yearSummaryEl = document.getElementById ('year-summary');
const yearLegendEl = document.getElementById ('year-legend');
const yearYearEl = document.getElementById ('year-year');

let yearData = null;
let yearPlot = null;

const YEAR_PAD = { left: 44, right: 12, top: 12, bottom: 24 };

/** Seconds-after-local-noon -> "HH:MM", without going through Date (and
 * so without that day's own DST offset getting in the way) - a day
 * entry's non-noon fields are already stored as plain epoch-seconds
 * differences from that day's noon, which is exactly hours-after-noon in
 * real elapsed time. */
function yearTimeLabel (offsetSec) {
	const totalMin = (Math.round (offsetSec / 60) + 12 * 60 + 1440) % 1440;
	const h = Math.floor (totalMin / 60), m = totalMin % 60;
	return String (h).padStart (2, '0') + ':' + String (m).padStart (2, '0');
}

function yearDateString (noon) {
	const d = new Date (noon * 1000);
	return d.getFullYear () + '-' + String (d.getMonth () + 1).padStart (2, '0')
		+ '-' + String (d.getDate ()).padStart (2, '0');
}

async function loadYearVisibility () {
	if (currentId === null)
		return;
	yearPanelEl.hidden = false;
	setStatus (yearStatusEl, true, 'loading…');

	const year = yearYearEl.value ? parseInt (yearYearEl.value, 10) : new Date ().getFullYear ();
	const url = apiUrl (currentSite, `api/db/target-visibility-year?id=${encodeURIComponent (currentId)}&year=${year}`);

	try {
		const r = await fetchJson (url);
		if (!r.ok)
			throw new Error (r.body.error || 'request failed');
		yearData = r.body;
		yearYearEl.value = yearData.year;
		describeYearVisibility ();
		setStatus (yearStatusEl, true, `${yearData.days.length} nights computed for ${yearData.year}`);
	} catch (er) {
		yearData = null;
		yearSummaryEl.textContent = '';
		yearLegendEl.innerHTML = '';
		setStatus (yearStatusEl, false, `cannot compute yearly visibility: ${er.message}`);
	}
	drawYearVisibility ();
}

function describeYearVisibility () {
	if (!yearData || !yearData.days.length) {
		yearSummaryEl.textContent = '';
		yearLegendEl.innerHTML = '';
		return;
	}

	let best = null, visibleNights = 0;
	for (const d of yearData.days) {
		if (d[7].length)
			visibleNights++;
		if (!best || d[5] > best[5])
			best = d;
	}

	yearSummaryEl.textContent = best
		? `best around ${yearDateString (best[0])}: up to ${best[5].toFixed (0)}°, `
			+ `visible on ${visibleNights} of ${yearData.days.length} nights`
		: 'never rises above the horizon this year';

	yearLegendEl.innerHTML = '<span class="k-visible">above horizon at night</span>'
		+ '<span class="k-twilight">twilight</span>'
		+ '<span class="k-night">RTS2 night start/end</span>';
}

/** Runs the callback over each maximal run of days where getter(day) is
 * not null, as [{x, y}, ...] point lists - the shared shape for drawing
 * both the night-boundary curves and the target-visibility band without
 * a straight line bridging across a gap (polar-ish nights with no true
 * RTS2 night, or the target never clearing the horizon that night). */
function yearRuns (days, sx, sy, getter) {
	const runs = [];
	let run = null;
	for (const d of days) {
		const v = getter (d);
		if (v === null) {
			if (run) { runs.push (run); run = null; }
			continue;
		}
		if (!run) { run = []; runs.push (run); }
		run.push ({ x: sx (d[0]), y: sy (v - d[0]) });
	}
	return runs;
}

function drawYearVisibility () {
	if (!yearCanvas)
		return;
	const ratio = window.devicePixelRatio || 1;
	const width = yearCanvas.clientWidth;
	const height = yearCanvas.clientHeight;
	yearCanvas.width = Math.round (width * ratio);
	yearCanvas.height = Math.round (height * ratio);
	const ctx = yearCanvas.getContext ('2d');
	ctx.setTransform (ratio, 0, 0, ratio, 0, 0);
	ctx.clearRect (0, 0, width, height);
	yearPlot = null;

	const style = getComputedStyle (document.body);
	const fg = style.getPropertyValue ('--text-muted').trim () || '#6b7280';
	const grid = style.getPropertyValue ('--border').trim () || '#dcdfe4';
	const cHorizon = style.getPropertyValue ('--sky-horizon').trim () || '#6b7280';
	const cNight = style.getPropertyValue ('--sky-night-line').trim () || '#15803d';
	const cTwilight = style.getPropertyValue ('--sky-twilight').trim () || 'rgba(37,99,235,0.07)';
	const cTargetFill = style.getPropertyValue ('--sky-target-fill').trim () || 'rgba(37,99,235,0.30)';

	if (!yearData || !yearData.days.length) {
		ctx.fillStyle = fg;
		ctx.font = '13px sans-serif';
		ctx.textAlign = 'center';
		ctx.fillText ('no visibility data', width / 2, height / 2);
		return;
	}

	const days = yearData.days;
	const x0 = YEAR_PAD.left, x1 = width - YEAR_PAD.right;
	const y0 = YEAR_PAD.top, y1 = height - YEAR_PAD.bottom;
	if (x1 <= x0 || y1 <= y0)
		return;

	const tMinX = days[0][0], tMaxX = days[days.length - 1][0];
	// Y axis: seconds after that day's own local noon, auto-scaled to
	// this year's actual sunset/sunrise extremes rather than a fixed
	// window that would need re-tuning per latitude.
	let yMin = Infinity, yMax = -Infinity;
	for (const d of days) {
		yMin = Math.min (yMin, d[1] - d[0]);
		yMax = Math.max (yMax, d[4] - d[0]);
	}
	const yMargin = (yMax - yMin) * 0.04;
	yMin -= yMargin;
	yMax += yMargin;

	const sx = t => x0 + (tMaxX > tMinX ? (t - tMinX) / (tMaxX - tMinX) : 0) * (x1 - x0);
	const sy = off => y1 - (off - yMin) / (yMax - yMin) * (y1 - y0);

	// Twilight/visibility bands, filled between two per-day getters (e.g.
	// sunset and nightStart) - a plain area fill rather than yearRuns()'s
	// line-run shape, since top and bottom share the same day set here
	// and a run just needs to skip days where either side is null.
	function fillArea (topGetter, bottomGetter, color) {
		ctx.fillStyle = color;
		let i = 0;
		while (i < days.length) {
			if (topGetter (days[i]) === null || bottomGetter (days[i]) === null) { i++; continue; }
			const pts = [];
			while (i < days.length && topGetter (days[i]) !== null && bottomGetter (days[i]) !== null) {
				pts.push (days[i]);
				i++;
			}
			ctx.beginPath ();
			pts.forEach ((d, k) => {
				const x = sx (d[0]), y = sy (topGetter (d) - d[0]);
				k ? ctx.lineTo (x, y) : ctx.moveTo (x, y);
			});
			for (let k = pts.length - 1; k >= 0; k--) {
				const d = pts[k];
				ctx.lineTo (sx (d[0]), sy (bottomGetter (d) - d[0]));
			}
			ctx.closePath ();
			ctx.fill ();
		}
	}

	// A day with no true RTS2 night (nightStart/nightEnd null - the sun
	// never reaches night_horizon, a summer "white night" at high
	// latitude) is twilight the whole way from sunset to sunrise, not
	// unshaded: the first band's bottom falls back to sunrise itself,
	// and the second band is skipped entirely for that day (its top is
	// null) so the two never double-paint the same night.
	fillArea (d => d[1], d => d[2] !== null ? d[2] : d[4], cTwilight);	// sunset -> nightStart (or sunrise)
	fillArea (d => d[3], d => d[4], cTwilight);						// nightEnd -> sunrise

	// Target visibility: a set of intervals per night (d[7], [[start,
	// end], ...]), not one span - a circumpolar target sweeps through
	// every azimuth over a night and can duck behind a real horizon
	// obstruction and reappear, so this is drawn as one filled rectangle
	// per interval per day rather than a single ribbon connected across
	// days, which would paint over any such dip as still visible.
	ctx.fillStyle = cTargetFill;
	for (let i = 0; i < days.length; i++) {
		const d = days[i];
		if (!d[7].length)
			continue;
		const cx = sx (d[0]);
		const xL = i > 0 ? (cx + sx (days[i - 1][0])) / 2 : cx - (days.length > 1 ? (sx (days[1][0]) - cx) / 2 : 0);
		const xR = i < days.length - 1 ? (cx + sx (days[i + 1][0])) / 2 : cx + (days.length > 1 ? (cx - sx (days[i - 1][0])) / 2 : 0);
		for (const [ws, we] of d[7]) {
			const yTop = sy (we - d[0]), yBottom = sy (ws - d[0]);
			ctx.fillRect (xL, yTop, xR - xL, yBottom - yTop);
		}
	}

	// y grid + labels, on a nice round number of hours
	ctx.strokeStyle = grid;
	ctx.fillStyle = fg;
	ctx.lineWidth = 1;
	ctx.font = '11px sans-serif';
	ctx.textAlign = 'right';
	ctx.textBaseline = 'middle';
	const hourStep = 3600 * Math.max (1, Math.round ((yMax - yMin) / 3600 / 6));
	for (let off = Math.ceil (yMin / hourStep) * hourStep; off <= yMax; off += hourStep) {
		const y = Math.round (sy (off)) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (x0, y);
		ctx.lineTo (x1, y);
		ctx.stroke ();
		ctx.fillText (yearTimeLabel (off), x0 - 6, y);
	}

	// x grid + labels, one per month (the 1st of each month in range)
	ctx.textAlign = 'center';
	ctx.textBaseline = 'top';
	const monthNames = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
	for (const d of days) {
		const dt = new Date (d[0] * 1000);
		if (dt.getDate () !== 1)
			continue;
		const x = Math.round (sx (d[0])) + 0.5;
		ctx.beginPath ();
		ctx.moveTo (x, y0);
		ctx.lineTo (x, y1);
		ctx.stroke ();
		const label = monthNames[dt.getMonth ()];
		const half = ctx.measureText (label).width / 2;
		ctx.textAlign = x + half > width ? 'right' : (x - half < 0 ? 'left' : 'center');
		ctx.fillText (label, x, y1 + 4);
		ctx.textAlign = 'center';
	}

	// sunset/sunrise outline
	ctx.strokeStyle = cHorizon;
	ctx.lineWidth = 1;
	for (const getter of [d => d[1], d => d[4]]) {
		ctx.beginPath ();
		let pen = false;
		for (const d of days) {
			const x = sx (d[0]), y = sy (getter (d) - d[0]);
			pen ? ctx.lineTo (x, y) : ctx.moveTo (x, y);
			pen = true;
		}
		ctx.stroke ();
	}

	// RTS2 night boundary curves, dashed - gaps where a day has no true
	// night state (only twilight, at high latitude midsummer)
	ctx.strokeStyle = cNight;
	ctx.setLineDash ([5, 4]);
	ctx.lineWidth = 1.2;
	for (const getter of [d => d[2], d => d[3]]) {
		for (const run of yearRuns (days, sx, sy, getter)) {
			ctx.beginPath ();
			run.forEach ((p, k) => k ? ctx.lineTo (p.x, p.y) : ctx.moveTo (p.x, p.y));
			ctx.stroke ();
		}
	}
	ctx.setLineDash ([]);

	yearPlot = { x0, x1, y0, y1, tMinX, tMaxX, sx, sy, yMin, yMax };
}

function onYearMove (event) {
	if (!yearPlot || !yearData || !yearData.days.length) {
		yearTooltipEl.style.display = 'none';
		return;
	}
	const rect = yearCanvas.getBoundingClientRect ();
	const x = event.clientX - rect.left;
	if (x < yearPlot.x0 || x > yearPlot.x1) {
		yearTooltipEl.style.display = 'none';
		return;
	}

	const t = yearPlot.tMinX + (x - yearPlot.x0) / (yearPlot.x1 - yearPlot.x0) * (yearPlot.tMaxX - yearPlot.tMinX);
	let best = null, bestDist = Infinity;
	for (const d of yearData.days) {
		const dist = Math.abs (d[0] - t);
		if (dist < bestDist) { bestDist = dist; best = d; }
	}
	if (!best) {
		yearTooltipEl.style.display = 'none';
		return;
	}

	let text = `${yearDateString (best[0])}  sunset ${yearTimeLabel (best[1] - best[0])}  sunrise ${yearTimeLabel (best[4] - best[0])}`;
	text += best[7].length
		? '  up ' + best[7].map (([ws, we]) => `${yearTimeLabel (ws - best[0])}–${yearTimeLabel (we - best[0])}`).join (', ')
			+ `, peak ${best[5].toFixed (0)}°`
		: '  never above the horizon that night';
	yearTooltipEl.textContent = text;
	yearTooltipEl.style.display = 'block';
	const tipWidth = yearTooltipEl.offsetWidth;
	let left = yearPlot.sx (best[0]) + 12;
	if (left + tipWidth > rect.width)
		left = yearPlot.sx (best[0]) - tipWidth - 12;
	yearTooltipEl.style.left = left + 'px';
	yearTooltipEl.style.top = '8px';
}

function shiftYear (delta) {
	const year = (yearYearEl.value ? parseInt (yearYearEl.value, 10) : new Date ().getFullYear ()) + delta;
	yearYearEl.value = year;
	loadYearVisibility ();
}

document.getElementById ('year-prev').addEventListener ('click', () => shiftYear (-1));
document.getElementById ('year-next').addEventListener ('click', () => shiftYear (1));
document.getElementById ('year-this').addEventListener ('click', () => {
	yearYearEl.value = new Date ().getFullYear ();
	loadYearVisibility ();
});
yearYearEl.addEventListener ('change', loadYearVisibility);
document.getElementById ('year-form').addEventListener ('submit', (ev) => ev.preventDefault ());

yearCanvas.addEventListener ('mousemove', onYearMove);
yearCanvas.addEventListener ('mouseleave', () => { yearTooltipEl.style.display = 'none'; });

let yearResizeTimer = null;
window.addEventListener ('resize', () => {
	clearTimeout (yearResizeTimer);
	yearResizeTimer = setTimeout (drawYearVisibility, 200);
});

// Support ?id=N in the URL so a link (e.g. from a future target-list
// view) can jump straight to a target instead of requiring the id to be
// typed in by hand.
const preselectId = new URLSearchParams (location.search).get ('id');
if (preselectId) {
	document.getElementById ('load-id').value = preselectId;
	loadTarget (preselectId);
}
