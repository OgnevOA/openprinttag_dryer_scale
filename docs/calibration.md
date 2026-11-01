# Calibration

Four numbers live in NVS and survive reflashing, as long as "Erase All Flash
Before Sketch Upload" stays disabled:

| Constant | What it is | How often |
| --- | --- | --- |
| Zero | The cell's own offset with an empty platform | Once, and after any mechanical change |
| Span | Counts per gram | Once |
| Dryer tare | What the empty dryer weighs | Once, and whenever the dryer is moved |
| Drift coefficient | Grams per °C of cell temperature | Once, from a full drying cycle |

All four are on the portal's **Scale** page except the drift coefficient, which
is measured on the **Self-test** page and applied from there.

## Order

Zero, then span, then tare. Each one depends on the one before it, and doing
them out of order produces a device that looks calibrated and is not.

### Zero

Nothing on the platform — no dryer, no spool. Press **1. Zero empty platform**.

This is the cell's electrical offset plus the weight of the platform itself.
Redo it after any mechanical change: a screw retightened, a mount reprinted, a
cable rerouted.

### Span

Put a known mass on the platform, enter it, press **Calibrate span**.

A 2 kg reference is the sweet spot. Heavier means a gram of error in the
reference matters less; lighter means you can lift it without disturbing the
platform. Two things to be careful about:

- **Trust the mass, not the label.** Kitchen scales are often 1% out, which on
  2 kg is 20 g of span error, which is 20 g of error at every reading. If you
  have nothing better, a 2 litre bottle filled by weight on a scale you have
  reason to believe is fine.
- **Put it in the middle.** On a single point cell, an off-centre load reads
  differently, and calibrating with the mass in a corner bakes that in.

Check afterwards that the reading goes back to zero when you take the mass off.
Anything more than a couple of grams means something is binding — usually the
overload stop set too close.

### Dryer tare

Put the empty dryer on the platform with its lid closed and press **Tare
dryer**. Empty means empty: no spool, no desiccant bag you forgot about.

The lid position matters because a 1.14 kg dryer with its lid open has its
centre of mass somewhere else, and on a single point cell that reads slightly
differently.

Redo the tare whenever the dryer is moved on the platform, or whenever anything
is added to it permanently. Everything downstream — remaining weight, consumed
weight, the value written back to the tag — is measured from this number.

### If you print straight from the dryer

Feeding the printer through the dryer's pass-through puts a PTFE tube between the
dryer and a machine that does not move, and the whole dryer is what sits on the
load cell. A bent tube pushes or pulls with a force of its own, and the cell
cannot tell that from filament.

**So tare with the tube connected and routed exactly as it will be while
printing**, and redo the tare if you ever re-route it or move the printer. That
folds the static force into the tare, where it belongs. Leave a gentle loop with
slack in it rather than a taut line: a tube under tension changes its pull as the
dryer's weight drops, and no single tare can absorb a force that moves.

The extruder's own tugging is a different problem and needs no fixing. It is
intermittent, so readings during a print are never stable, and the device only
acts on stable ones — nothing is written to a tag or sent to Spoolman mid-print.
The figures land once the printer stops pulling. This is also why a spool that
feeds a printer shows its consumption in steps between prints rather than as a
smooth decline.

## Checking it, rather than trusting it

The tare has an independent second measurement built in, and it costs nothing to
use. Put a **sealed** spool in the dryer — one whose tag reports nothing consumed
— and the tag's own reference weights say what the dryer must weigh:

```
dryer_tare = gross - (filament weight + spool weight)
```

The **Scale** page's cross-checks card shows that figure and how far the stored
tare is from it. Agreement within 5 g means the whole chain is sound: cell,
converter, span, tare and the tag's data all agree. That is
[validation test 5](validation.md#5-tare-cross-check), and it is the single most
informative number this device produces.

If they disagree, work out which to believe before adopting either. A spool that
has been printed from, or a tag provisioned from a mistyped weight, gives a
confident and wrong answer. **Use the implied tare** takes the tag's version.

## Drift compensation

A load cell reads differently when it is warm, and this one lives under a
70 °C dryer. The compensation is a single coefficient in grams per degree,
referenced to a temperature, and it is measured rather than guessed:

1. Put a load that will not change on the platform. A sealed spool or a plain
   1 kg mass — not a wet spool, which loses grams of water over a cycle and
   would look exactly like drift.
2. Start from cold. The cold end of the run is the part that cannot be
   reproduced later.
3. On the **Self-test** page, start the drift log, run a full drying cycle at the
   temperature you actually use, and let it cool with the log still running.
4. Stop the log and press **Apply fit**.

The device refuses a fit over less than 5 °C of range, or with an R² below 0.5,
because a slope drawn through noise fits anything. For the analysis it cannot do
— separating the heating leg from the cooling leg, which is how mechanical
hysteresis shows itself — download `drift.csv` and run:

```bash
python3 tools/fit_drift.py drift.csv
```

Full procedure and the failure modes:
[validation test 6](validation.md#6-thermal-drift-run).

The coefficient is also settable by hand on the Scale page, which is useful for
trying a value from someone else's build before committing to a six-hour run of
your own. Expect a few tenths of a gram per degree with the isolator feet fitted.
Several grams per degree means heat is reaching the cell, and no coefficient
fixes that honestly.

## What good looks like

| Check | Expected |
| --- | --- |
| Return to zero after unloading | within 2 g |
| 500 g / 1000 g / 2000 g test masses | within 3 / 4 / 6 g |
| Tare against a sealed spool | within 5 g |
| Reading through a dry cycle, compensated | within 3 g |

A gram or two is the floor. A 5 kg cell carrying a 1.14 kg dryer and a 1.25 kg
spool is working in the bottom quarter of its range, through a heated enclosure.
Remaining filament good to a few grams is a couple of metres of PLA, which is a
better answer than any estimate from print time.

## When to redo what

| You did this | Redo |
| --- | --- |
| Moved the dryer on the platform | Tare |
| Took the platform off and put it back | Zero, tare |
| Reprinted a `cell_mount` or retightened the cell bolts | Zero, span, tare |
| Changed the isolator feet | Tare, and the drift run |
| Reflashed with flash erase enabled | All of it |
| Moved the whole thing to a different shelf | Zero, tare |
