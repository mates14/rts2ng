// RTS2 target editor - vanilla JS, no build step, no external
// dependencies. Loaded via a plain relative <script src="target.js">, so
// fetch('api/...') calls below resolve correctly under any mount depth
// (see app.js's basePath() comment / STATUS.md's proxy-subpath note) -
// this page never constructs a WebSocket URL, so that's the only thing
// relative fetch() needs to get right here.

let currentId = null;

const loadStatusEl = document.getElementById ('load-status');
const targetPanelEl = document.getElementById ('target-panel');
const schedulingPanelEl = document.getElementById ('scheduling-panel');
const saveStatusEl = document.getElementById ('save-status');
const schedulingStatusEl = document.getElementById ('scheduling-status');

function setStatus (el, ok, text) {
	el.className = 'status-line ' + (ok ? 'ok' : 'err');
	el.textContent = text;
}

async function loadTarget (id) {
	setStatus (loadStatusEl, true, 'loading…');
	targetPanelEl.hidden = true;
	schedulingPanelEl.hidden = true;

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

	// Scheduling has its own existence check server-side (dbGetScheduling
	// throws if the target itself doesn't exist) but we already know it
	// does at this point, so this is just "does it have a row yet".
	try {
		const res = await fetch (`api/db/scheduling?id=${encodeURIComponent (tar.id)}`);
		const body = await res.json ();
		if (res.ok) {
			document.getElementById ('f-sinfo').value = body.sinfo || '';
			schedulingStatusEl.textContent = '';
			schedulingPanelEl.hidden = false;
		}
	} catch (e) {
		// Non-fatal - the target itself loaded fine, scheduling is a
		// secondary panel. WEB_WITH_DB=OFF builds also 404 here, which
		// this catch/no-op treats the same as "leave it hidden".
	}
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
		loadTarget (currentId);		 // re-render from the confirmed post-save state
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
	} catch (e) {
		setStatus (schedulingStatusEl, false, String (e));
	}
});

// Support ?id=N in the URL so a link (e.g. from a future target-list
// view) can jump straight to a target instead of requiring the id to be
// typed in by hand.
const preselectId = new URLSearchParams (location.search).get ('id');
if (preselectId) {
	document.getElementById ('load-id').value = preselectId;
	loadTarget (preselectId);
}
