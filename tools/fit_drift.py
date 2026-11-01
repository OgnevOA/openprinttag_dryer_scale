#!/usr/bin/env python3
"""Fit a thermal drift coefficient from a drift log downloaded off the device.

The device fits a line itself and will happily apply it, but it only has room
for one number and no way to show its work. This does the parts that need the
whole run in front of you: it separates the heating leg from the cooling leg,
which is how hysteresis in the load cell mount shows up, and it prints the
scatter so a run spoiled by somebody leaning on the dryer is obvious rather
than averaged in.

    python3 tools/fit_drift.py drift.csv

Standard library only, so it runs wherever the CSV lands.
"""

import argparse
import csv
import math
import sys

# Below this the fit is extrapolation: the same threshold the firmware uses.
MIN_SPAN_C = 5.0


class Sample:
    __slots__ = ("seconds", "celsius", "raw", "grams")

    def __init__(self, seconds, celsius, raw, grams):
        self.seconds = seconds
        self.celsius = celsius
        self.raw = raw
        self.grams = grams


def read_log(path):
    samples = []
    skipped = 0
    with open(path, newline="") as handle:
        for row in csv.DictReader(handle):
            try:
                celsius = row["temperature_c"]
                if celsius is None or celsius.strip() == "":
                    skipped += 1
                    continue
                samples.append(
                    Sample(
                        int(row["seconds"]),
                        float(celsius),
                        int(row["raw_counts"]),
                        float(row["uncompensated_g"]),
                    )
                )
            except (KeyError, TypeError, ValueError):
                skipped += 1
    return samples, skipped


def fit(samples):
    """Least squares of grams against temperature."""
    n = len(samples)
    if n < 8:
        return None

    sum_t = sum(s.celsius for s in samples)
    sum_g = sum(s.grams for s in samples)
    sum_tt = sum(s.celsius * s.celsius for s in samples)
    sum_tg = sum(s.celsius * s.grams for s in samples)

    denominator = n * sum_tt - sum_t * sum_t
    if abs(denominator) < 1e-9:
        return None

    slope = (n * sum_tg - sum_t * sum_g) / denominator
    intercept = (sum_g - slope * sum_t) / n

    mean_g = sum_g / n
    residual = sum((s.grams - (intercept + slope * s.celsius)) ** 2 for s in samples)
    total = sum((s.grams - mean_g) ** 2 for s in samples)

    return {
        "slope": slope,
        "intercept": intercept,
        "r_squared": 1.0 - residual / total if total > 0 else 1.0,
        "rms": math.sqrt(residual / n),
        "min_c": min(s.celsius for s in samples),
        "max_c": max(s.celsius for s in samples),
        "min_g": min(s.grams for s in samples),
        "max_g": max(s.grams for s in samples),
        "n": n,
    }


def split_legs(samples):
    """Heating up to the peak temperature, cooling after it.

    Split on the peak rather than the midpoint of the run, because a dry cycle
    holds temperature for hours and then cools on its own schedule.
    """
    peak = max(range(len(samples)), key=lambda i: samples[i].celsius)
    return samples[: peak + 1], samples[peak:]


def hysteresis(heating, cooling):
    """Largest gap between the two legs where their temperatures overlap.

    A cell that reads differently at 40 C on the way up and on the way down is
    not being fooled by temperature alone; something in the mount is moving,
    and no single coefficient will fix it.
    """
    if len(heating) < 4 or len(cooling) < 4:
        return None

    low = max(min(s.celsius for s in heating), min(s.celsius for s in cooling))
    high = min(max(s.celsius for s in heating), max(s.celsius for s in cooling))
    if high - low < 1.0:
        return None

    def interpolate(leg, celsius):
        ordered = sorted(leg, key=lambda s: s.celsius)
        for before, after in zip(ordered, ordered[1:]):
            if before.celsius <= celsius <= after.celsius:
                span = after.celsius - before.celsius
                if span < 1e-6:
                    return before.grams
                ratio = (celsius - before.celsius) / span
                return before.grams + ratio * (after.grams - before.grams)
        return None

    worst = 0.0
    worst_at = low
    steps = 20
    for step in range(steps + 1):
        celsius = low + (high - low) * step / steps
        up = interpolate(heating, celsius)
        down = interpolate(cooling, celsius)
        if up is None or down is None:
            continue
        if abs(up - down) > abs(worst):
            worst = up - down
            worst_at = celsius
    return worst, worst_at


def scatter(samples, width=64, height=16):
    """Weight against temperature, so a spoiled run is visible at a glance."""
    min_t = min(s.celsius for s in samples)
    max_t = max(s.celsius for s in samples)
    min_g = min(s.grams for s in samples)
    max_g = max(s.grams for s in samples)
    if max_t - min_t < 1e-6 or max_g - min_g < 1e-6:
        return ["(nothing to plot: the run did not move)"]

    peak = max(range(len(samples)), key=lambda i: samples[i].celsius)
    grid = [[" "] * width for _ in range(height)]
    for index, sample in enumerate(samples):
        column = int((sample.celsius - min_t) / (max_t - min_t) * (width - 1))
        row = int((sample.grams - min_g) / (max_g - min_g) * (height - 1))
        mark = "+" if index <= peak else "o"
        existing = grid[height - 1 - row][column]
        # Marking overlaps rather than letting the cooling leg paint over the
        # heating one: where the two legs do not overlap is the hysteresis.
        if existing == " ":
            grid[height - 1 - row][column] = mark
        elif existing != mark:
            grid[height - 1 - row][column] = "#"

    label_width = 9
    lines = []
    for index, row in enumerate(grid):
        value = max_g - (max_g - min_g) * index / (height - 1)
        label = f"{value:8.1f} " if index in (0, height - 1) else " " * label_width
        lines.append(label + "|" + "".join(row))
    lines.append(" " * label_width + "+" + "-" * width)
    footer = f"{min_t:.1f} C"
    lines.append(
        " " * (label_width + 1)
        + footer
        + " " * max(1, width - len(footer) - len(f"{max_t:.1f} C"))
        + f"{max_t:.1f} C"
    )
    lines.append(" " * label_width +
                 " + heating   o cooling   # both   (grams up the side)")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", help="drift.csv downloaded from /drift.csv")
    parser.add_argument(
        "--no-plot", action="store_true", help="skip the ASCII scatter"
    )
    args = parser.parse_args()

    samples, skipped = read_log(args.csv)
    if not samples:
        print("no usable samples: was a temperature sensor fitted?", file=sys.stderr)
        return 1

    print(f"{len(samples)} samples spanning "
          f"{(samples[-1].seconds - samples[0].seconds) / 3600.0:.1f} hours")
    if skipped:
        print(f"{skipped} rows skipped for having no temperature")

    overall = fit(samples)
    if overall is None:
        print("not enough spread to fit", file=sys.stderr)
        return 1

    span = overall["max_c"] - overall["min_c"]
    print()
    print(f"temperature   {overall['min_c']:.1f} to {overall['max_c']:.1f} C "
          f"({span:.1f} C span)")
    print(f"weight        {overall['min_g']:.1f} to {overall['max_g']:.1f} g "
          f"({overall['max_g'] - overall['min_g']:.1f} g of drift)")
    print(f"drift         {overall['slope']:.4f} g/C")
    print(f"R^2           {overall['r_squared']:.3f}")
    print(f"residual      {overall['rms']:.2f} g RMS after compensation")

    heating, cooling = split_legs(samples)
    up = fit(heating)
    down = fit(cooling)
    if up and down:
        print()
        print(f"heating leg   {up['slope']:.4f} g/C over {up['n']} samples, "
              f"R^2 {up['r_squared']:.3f}")
        print(f"cooling leg   {down['slope']:.4f} g/C over {down['n']} samples, "
              f"R^2 {down['r_squared']:.3f}")
        gap = hysteresis(heating, cooling)
        if gap is not None:
            worst, worst_at = gap
            print(f"hysteresis    {worst:+.2f} g at {worst_at:.1f} C")

    print()
    warnings = []
    if span < MIN_SPAN_C:
        warnings.append(
            f"the run only covered {span:.1f} C, so this slope is extrapolation. "
            "Log a full cycle from cold."
        )
    if overall["r_squared"] < 0.8:
        warnings.append(
            "R^2 is low, so much of the movement was not temperature. Check that "
            "nothing touched the dryer and that the load stayed put."
        )
    if abs(overall["slope"]) > 5.0:
        warnings.append(
            "that is a very large coefficient. Suspect the cell is being heated "
            "directly rather than compensating for it in software: fit the "
            "isolator feet first."
        )
    if up and down and abs(up["slope"] - down["slope"]) > 0.5 * abs(overall["slope"]):
        warnings.append(
            "the two legs disagree, which is mechanical hysteresis rather than "
            "drift. Check the mounting screws and that nothing rubs."
        )
    for warning in warnings:
        print(f"warning: {warning}")
    if not warnings:
        print("the run looks clean.")

    print()
    if warnings:
        print("If the warnings above are acceptable, the coefficient goes on the")
        print("device's Settings page, or into config.h as the new default:")
    else:
        print("Apply it on the device's Settings page as the drift coefficient,")
        print("or bake it into config.h as the new default:")
    print()
    print(f"    #define SCALE_TEMP_DRIFT_G_PER_C  {overall['slope']:.4f}f")
    print(f"    #define SCALE_TEMP_REFERENCE_C    {overall['min_c']:.1f}f")

    if not args.no_plot:
        print()
        for line in scatter(samples):
            print(line)

    return 0


if __name__ == "__main__":
    sys.exit(main())
