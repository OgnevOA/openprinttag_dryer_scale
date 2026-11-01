# Spoolman

[Spoolman](https://github.com/Donkie/Spoolman) is a self-hosted inventory of
filament spools. Printers report to it what they think they consumed, and it
keeps a running figure for what is left on every spool in the house.

"What it thinks it consumed" is the weak part. Nothing in that chain ever weighs
anything: a slicer's estimate becomes Moonraker's report becomes Spoolman's
remaining weight, and the error accumulates over a spool's life with no way to
notice. A purge, a failed print, a bit of filament pulled off by hand, a spool
swapped between printers without the active spool being changed — all of it
drifts the number and none of it is visible.

This device has a load cell under the dryer. Any spool that spends a night in it
can have its Spoolman figure replaced with a measurement, which is the one thing
the rest of the setup cannot produce.

## What it does

While a tag is in the dryer's reader field:

1. The tag's UID is looked up in Spoolman, once per tag.
2. When the weight has settled, the spool's gross weight — filament plus the
   empty spool, with the dryer's own weight subtracted — is sent to Spoolman's
   `measure` endpoint, which turns it into a used weight and a remaining weight.
3. Nothing further is sent until the weight has moved by 5 g, and never more
   often than every five minutes. Both are in `config.h` as
   `SPOOLMAN_MIN_DELTA_G` and `SPOOLMAN_MIN_INTERVAL_MS`.

The same number goes into the tag's auxiliary region as `consumed_weight`, on the
tag's own schedule. The two agree because they come from the same measurement:
OpenPrintTag's `consumed_weight` and Spoolman's `used_weight` are the same
quantity in the same units.

## Running Spoolman on TrueNAS SCALE

Spoolman is not in the TrueNAS apps catalog, so it goes in as a custom app.
[`deploy/spoolman-truenas.yaml`](../deploy/spoolman-truenas.yaml) is the whole
manifest; what follows is the order to do things in and the two places where a
TrueNAS install differs from the upstream instructions.

1. **Make a dataset for the database.** Datasets → your pool → *Add Dataset*,
   name it `spoolman` (under an `apps` parent if you keep one), and use the
   **Apps** preset so it comes out owned by `apps:apps` with POSIX permissions.
   A dataset rather than a plain directory is worth it for one reason: snapshots.

2. **Find your apps uid.** In the TrueNAS shell, `id apps`. It has been 568 for
   years, which is what the manifest assumes, but it is one command to be sure.

3. **Edit two things in the YAML**: `source:` to your dataset's real path, and
   `PUID`/`PGID` if step 2 disagreed with 568.

4. **Install it.** Apps → Discover → the three-dot menu beside **Custom App** →
   **Install via YAML**. Name it `spoolman` and paste the manifest into *Custom
   Config*. TrueNAS validates the YAML's syntax and nothing else, so a wrong path
   shows up as a container that will not start rather than as an error on save.

5. **Check the database actually landed.** This is the one check worth not
   skipping:

   ```
   ls -l /mnt/POOL/apps/spoolman/
   ```

   There must be a `spoolman.db`. If the directory is empty while the web UI
   works, Spoolman is writing inside the container and every update will wipe
   your inventory. See the permissions note below.

6. **Open it** at `http://<truenas>:7912` and add the spool that is in the dryer,
   or let SpoolmanDB's catalogue fill in the filament for you.

7. **Snapshot it.** Data Protection → Periodic Snapshot Tasks, daily on that
   dataset, two weeks of retention. Spoolman takes its own SQLite backups into the
   same directory, which does not help if the directory is what you lose.

### Permissions, which is where this usually goes wrong

The Spoolman image builds its own `app` user at uid 1000, and the upstream
instructions tell you to `chown 1000:1000` the data directory. `PUID`/`PGID`
change that uid: the entrypoint starts as root, moves `app` to what you asked
for, then drops privileges with `gosu`. So `PUID: "568"` lets the dataset stay
owned by `apps:apps` like every other app's storage.

What it does **not** do is grant access. It changes *which* uid needs to be able
to write to the dataset, and the dataset still has to actually allow it. Get this
wrong and the app dies during deploy rather than starting degraded, because
Spoolman checks the data directory on startup, tries to chown it, and cannot —
it dropped root several lines earlier. The log says so plainly:

```
spoolman.env  WARNING  Data directory is not writable, trying to fix it...
chown: cannot read directory '/home/app/.local/share/spoolman': Permission denied
spoolman.env  ERROR    Data directory is not writable. Please run
                       "sudo chown -R 568:568 /path/to/spoolman/datadir" on the host OS.
uvicorn.error ERROR    Application startup failed. Exiting.
```

The uid in that message is the one the container is really running as, so it
doubles as confirmation that `PUID` took effect. The fix is to match it: either
Datasets → your dataset → *Edit Permissions* with owner and group `apps`, or in
the shell,

```
chown -R 568:568 /mnt/POOL/apps/spoolman
```

A dataset created with the **Apps** preset already comes out this way, which is
why that preset is worth using in step 1; a dataset created with any other preset
comes out `root:root` and produces exactly the failure above.

Two other refusals look similar but are not permissions: a non-numeric `PUID` is
rejected outright, and so is `PUID: "0"`, because Spoolman drops root and cannot
run as it.

### Which image tag

The manifest pins `ghcr.io/donkie/spoolman:0.26`, so patch releases arrive when
you pull and a minor-version schema change does not arrive on its own. The
alternatives, in case you want one:

| Tag | What you get |
| --- | --- |
| `0.26` | The current release line. Tags are identified by `extra.nfc_uid`. |
| `latest` | Whatever the newest release is, including minor-version jumps. |
| `edge` | The `master` branch, which is where the tag table lives. The dryer will use it automatically. Unfinished features, and a schema you may not be able to downgrade from. |

There is no wrong answer here as far as this device is concerned: it works out
which identification scheme the server supports on the first scan, and says which
one it found on the Tag page.

### Reaching it from the dryer

Whatever address you use in the dryer's settings has to keep working after a
reboot, so give the TrueNAS box a DHCP reservation or a static address first. The
ESP32 does plain HTTP and no DNS tricks: an IP and a port, `192.168.1.x:7912`.
A reverse proxy in front of Spoolman is fine as long as the dryer is pointed at
the plain-HTTP side of it.

## Setting up the dryer

1. In the portal, **Settings** → **Spoolman**, enter the address. Plain `http`,
   and Spoolman's default port is 7912, so `192.168.1.10:7912` is enough — the
   scheme is filled in. Leave it empty to switch the whole integration off.
2. Leave **Send measurements automatically** on unless you want to press a
   button every time.
3. Put a tagged spool in the dryer, open **Tag**, and find the **Spoolman**
   card. The first time a given tag is seen it will say *not known to Spoolman*
   and ask for a spool number. That number is in the URL when you open the spool
   in Spoolman's own UI, and on its label if you print those. Enter it and press
   **Link this tag**.

That is the only manual step, and it is once per spool. Every measurement after
it finds the spool on its own.

HTTPS is deliberately not supported. Spoolman ships without authentication and
its own advice is to put a reverse proxy in front of it if that matters; a
certificate store is not something this device can maintain, so it refuses an
`https://` URL rather than failing obscurely inside the transport.

## The version difference

Spoolman is in the middle of growing real tag support, so there are two ways to
identify a physical tag and this device works with both. The first scan decides
which one the server understands, and the Tag page reports it as **Tags
identified by**.

| Shown as | What it uses | Where it exists |
| --- | --- | --- |
| `nfc_uid field` | The UID in a spool extra field, `extra.nfc_uid` | Every release, including v0.26.1 |
| `tag table` | A real tag table: `POST /api/v1/tag/scan`, `PUT /api/v1/spool/{id}/tag` | `master` only, so far |

The extra-field route is the convention other scanners settled on, and linking a
tag creates the `nfc_uid` field in Spoolman if it does not exist yet. That step
is not cosmetic: **Spoolman silently ignores a filter on an extra field it has
never been told about and answers with the entire inventory.** A client that
trusted the first spool in that answer would put this dryer's weight on an
unrelated spool, so a match is only believed when the answer actually carries the
UID that was asked for, and an answer holding more than one spool is refused
rather than guessed at.

The UID is always written in Spoolman's canonical spelling — uppercase hex, no
separators — so a Spoolman upgraded to the tag table later finds the same tags.

The tag table is worth having when it lands: it enforces one tag per spool, which
the extra field cannot, and `openprinttag` is one of the formats it knows by
name.

## The hazard, and what guards it

Spoolman's `measure` endpoint treats a measurement heavier than a full spool as a
correction to what full means, and rewrites the spool's initial weight. That is
sensible when a person is weighing a spool whose catalogue weight was wrong. It
is destructive when the number arriving is wrong, because it does not just report
incorrectly — it redefines the baseline every later figure is derived from, and
nothing in Spoolman flags it.

The dryer weighs about 3 kg. A reading sent with the dryer's own weight still in
it would sail past any spool's full weight. So nothing is sent unless all of
this holds:

- the scale has been calibrated, and
- the dryer has been tared, and
- a weight has actually been measured, and it is above zero, and
- the reading has settled, and
- when the tag knows its own reference weights, the measurement is not more than
  20 g above a full spool.

When a check fails, the Spoolman card says which one rather than silently doing
nothing. `App::measurementUsable` is where they live, and
`tests/test_logic.cpp` aims numbers straight at it — including the untared-dryer
case, which is the one that would do real damage.

Two things follow from this that are worth knowing:

- **Calibrate before configuring Spoolman.** An uncalibrated device will refuse
  to send anything, which is the correct behaviour but looks like a broken
  integration.
- **Spoolman needs the same tare the tag has.** `measure` takes a gross weight and
  subtracts the empty spool weight Spoolman holds for that spool; if it has none,
  it takes the whole reading as filament. So copy the tag's
  `empty_container_weight` into the spool's *empty spool weight* field once, and
  copy it exactly — **including the adapter**. A 2025 Prusament spool plus this
  project's printed adapter is 186 + 31 = 217 g, not the 186 g the spool's page
  quotes, not the 201 g an older spool reports, and not the catalogue's default.

  Getting this one wrong is the likeliest way to hit the hazard above, because it
  fails in the dangerous direction: a tare 31 g light makes every spool look 31 g
  fuller than it is, and on a nearly-full spool that reads heavier than full.
  Spoolman's own `initial_weight` stays the plain filament weight either way —
  its schema calls it a net weight, so the adapter has no business in it.

## When something is wrong

The Tag page's Spoolman card carries the last error verbatim, including whatever
Spoolman said about it. The usual ones:

| What it says | What happened |
| --- | --- |
| `cannot reach Spoolman` | Wrong address or port, or the server is down. The dryer keeps weighing and writing tags regardless. |
| `Spoolman answered 404: Spool not found.` | The linked spool was deleted in Spoolman. Unlink and link again. |
| `Initial weight is not set.` | Spoolman has no full weight for that spool, so it cannot turn a gross weight into a used one. Fill it in there. |
| `answered with spools that do not carry this tag's UID` | The `nfc_uid` field is missing, so the filter was ignored. Link a tag once from this page, which creates it. |
| `already linked to spool N` | Tag-table Spoolman refusing to put one tag on two spools. Unlink it from `N` first. |
| `the dryer has not been tared` | Work through [calibration.md](calibration.md) first. |

All of those are the dryer's view. If Spoolman itself never finishes deploying,
the cause is on the server and the container log names it — most often the
directory permissions described [above](#permissions-which-is-where-this-usually-goes-wrong).

Nothing about Spoolman is on the critical path. If it is unreachable, or the URL
is empty, the device weighs, reads and writes tags exactly as it did before —
Spoolman is a place the measurements are copied to, never where they come from.
