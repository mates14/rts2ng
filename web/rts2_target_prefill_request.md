# Feature request: prefilled "new target" link for the RTS2 Target Editor

For: mates (rts2ng `web/static/target.js`)
From: fnovotny, 2026-09-29

## What I would like

A way to open `target.html` with a new target already filled in from the URL,
so that another page can link to it:

```
https://lascaux.asu.cas.cz/images/target.html?new=1&name=AT%202026adfv&ra=300.068792&dec=3.440528&comment=TNS%2C%20mag%2015.9&site=d50
```

Opening the link should behave as if I had clicked **New target...**, typed the
values in and ticked the telescope. Nothing is written until I click
**Save target** myself.

## Why

My transient list (`https://zeus.asu.cas.cz/~fnovotny/d50_targets/output/`)
ranks new transients for D50. I want a "Schedule" link next to each one. Today
I have to open the editor, click New target and retype name, RA and Dec by
hand. The editor already supports `?id=N` for existing targets; this is the
same idea for new ones.

I do not want my page to call the API directly. It is served from zeus
(a different origin), and I prefer that a logged-in person confirms every
target in the editor.

## Proposed URL parameters

| Parameter  | Meaning | Notes |
|------------|---------|-------|
| `new=1`    | start the new-target flow | ignored when `id` is also present |
| `name`     | goes to Name | |
| `ra`, `dec`| go to RA / Dec | same formats the form accepts; I will send decimal degrees |
| `comment`  | goes to Comment | optional |
| `info`     | goes to Info (tar_info) | optional |
| `site`     | telescopes to tick in the When box: `d50`, `sbt` or `d50,sbt` | optional, default none ticked |
| `priority` | priority for the ticked telescope(s) | optional |
| `duration` | duration (s) for the ticked telescope(s) | optional |

Only equatorial targets are needed. Parameter names are a suggestion, I will
use whatever you choose.

## Expected behaviour

1. **Duplicate check first.** Before reserving an id, look in `api/db/targets`
   on both telescopes for a target with the same name (ignoring case and
   spaces) or within about 5 arcsec of `ra`/`dec`. If one exists, load it as
   `?id=N` would and say so in the status line, instead of creating a second
   one.
2. **Otherwise run the existing New target flow** (reserve an id, check it is
   free on the peer), then fill the What box from the parameters and tick the
   telescope(s) from `site`.
3. **No write until Save target.** The reserved id is not created, as today.
4. **Remove the parameters from the address bar** once the form is filled
   (`history.replaceState`), so a reload does not reserve another id and
   refill the form.
5. **If id reservation fails**, show the existing error and leave the form
   hidden, as today.

## Optional, if it is easy

- **Visibility before saving.** `target-altitude` and
  `target-visibility-year` accept `ra=&dec=` for an unsaved target. Showing
  the two plots for the prefilled coordinates would let me see whether the
  transient is observable tonight before I create it.
- **Duration on first save.** Duration is written only by Save scheduling,
  which needs the target to exist. For a prefilled new target it would help if
  Save target also wrote the scheduling row when a duration is filled in.

## Sketch

Untested, only to show the size of the change. The click handler of
`#new-target-btn` becomes a named function so it can be awaited:

```js
async function startNewTarget () {
	// ... current body of the new-target-btn click handler ...
}
document.getElementById ('new-target-btn').addEventListener ('click', startNewTarget);

async function prefillNewTarget (qs) {
	await startNewTarget ();
	if (currentId === null)
		return; // id reservation failed, status line already says why

	const fill = (elId, key) => {
		if (qs.has (key))
			document.getElementById (elId).value = qs.get (key);
	};
	fill ('f-name', 'name');
	fill ('f-ra', 'ra');
	fill ('f-dec', 'dec');
	fill ('f-comment', 'comment');
	fill ('f-info', 'info');

	const sites = (qs.get ('site') || '').split (',');
	for (const site of ['d50', 'sbt']) {
		const cb = document.getElementById (`f-enabled-${site}`);
		if (!cb || cb.disabled || !sites.includes (site))
			continue;
		cb.checked = true;
		if (qs.has ('priority'))
			document.getElementById (`f-priority-${site}`).value = qs.get ('priority');
		if (qs.has ('duration'))
			document.getElementById (`f-duration-${site}`).value = qs.get ('duration');
	}

	history.replaceState (null, '', location.pathname);
}

const qs = new URLSearchParams (location.search);
const preselectId = qs.get ('id');
if (preselectId) {
	document.getElementById ('load-id').value = preselectId;
	loadTarget (preselectId);
} else if (qs.get ('new') === '1') {
	prefillNewTarget (qs);
}
```

The duplicate check (point 1) is not in the sketch.

## One thing I noticed while reading the code

The write endpoints (`target-create`, `target-save`, `scheduling-save`,
`script-save`, `script-delete`) are called with plain GET and the daemon does
not check the method. A link to one of them, clicked in a browser that is
already logged in, would therefore execute the write. Accepting only POST for
writes would close that. This is independent of the request above.

## What I will do on my side

- Add a "Schedule" link per transient that uses the parameters above.
- For transients that already are RTS2 targets (my pipeline reads ids
  8000-8999 hourly), link to `target.html?id=N` instead. That works today.
