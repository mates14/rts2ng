# Re: prefilled "new target" link for the RTS2 Target Editor

For: fnovotny
From: mates, 2026-09-30

Done, with your parameter names unchanged. It goes onto the telescopes
tomorrow (2026-10-01) during the day. Until then, links to it will only
load the empty editor.

## The link

```
https://lascaux.asu.cas.cz/images/target.html?new=1&name=AT%202026adfv&ra=300.068792&dec=3.440528&comment=TNS%2C%20mag%2015.9&site=d50
```

| Parameter  | Meaning |
|------------|---------|
| `new=1`    | start the new-target flow; ignored if `id` is also given |
| `name`     | Name |
| `ra`, `dec`| RA / Dec. Decimal degrees, or sexagesimal like the form (`20:00:16.5`, `+03:26:25.9`) |
| `comment`  | Comment (optional) |
| `info`     | Info / tar_info (optional) |
| `site`     | `d50`, `sbt` or `d50,sbt`; which telescopes get ticked (optional, default none) |
| `priority` | priority for the ticked telescope(s) (optional) |
| `duration` | duration in seconds for the ticked telescope(s) (optional) |

URL-encode the values (`%20` for spaces, `%2C` for commas). Only
equatorial targets are supported, as you asked.

## What happens when the link is opened

1. **Duplicate check.** The editor reads the target lists of both
   telescopes. If a target has the same name (ignoring case and spaces,
   so `AT 2026adfv` = `AT2026adfv`) or lies within 5 arcsec of `ra`/`dec`,
   that target is opened instead, exactly like `?id=N`. The status line
   says which target it is and why it matched. A name match wins over a
   position match. If one telescope's list cannot be read, the check
   still runs on the other and the status line says so.
2. **Otherwise the normal New target flow runs.** It reserves an ID that
   is free on both telescopes, fills in the fields and ticks the
   telescope(s) from `site` with priority and duration.
3. **Nothing is written until you click Save target.**
4. **The parameters are removed from the address bar** (`?id=N` after a
   duplicate hit, nothing otherwise), so a reload doesn't repeat it.
5. **If no ID can be reserved**, the error is shown and the form stays
   hidden.

## The two optional items

- **Visibility before saving.** The nightly and yearly plots are shown
  for the unsaved target from the RA/Dec in the form, including the
  other telescope's horizon. They redraw if you edit RA or Dec. This
  works for any new target, not only one opened from a link.
- **Duration on first save.** When Save target creates the target on a
  telescope, it also writes the duration (and request type, if set)
  there. You don't need a separate Save scheduling for a new target.

## Also fixed: "New target" failing to find an ID

This was the "could not find an id free on both telescopes" error you
reported earlier. The first fix only stepped past one colliding ID at a
time and gave up after 8 tries. The lowest free IDs on one telescope are
mostly IDs of deleted targets that are still in use on the other, so it
still failed nearly every time. The editor now asks the two telescopes in
turn until they agree on an ID that is free on both. That takes a few
requests even across long runs of taken IDs. This is in the same update.

## Things to know

- You need to be logged in on both telescopes. The first request to the
  other telescope may bring up its own login prompt, as it does today.
- Your "Schedule" link can point at either telescope's `target.html`. The
  duplicate check and ID reservation always look at both.
- Your plan to link existing targets (8000-8999) as `target.html?id=N` is
  fine. A `new=1` link for a target that already exists would also end
  up on it, through the duplicate check.

## Testing

I tested in headless Chrome against a mock of both telescope APIs:

- A new target was reserved and filled in with no writes before Save.
- Save created the target and stored the duration.
- A name duplicate and a 2.8" position duplicate each opened the
  existing target.
- `id` together with `new=1` opened the `id`.

I haven't tried it against the real daemons yet. That happens tomorrow
with the update. If something looks off, send me the link you used.

## Your note about GET on the write endpoints

You're right: the write endpoints accept plain GET, so a link to one,
opened by a logged-in person, would perform the write. This update
doesn't change that. It's a separate change I'll do next. Your page
doesn't need to do anything differently either way, because it only
links to `target.html`.
