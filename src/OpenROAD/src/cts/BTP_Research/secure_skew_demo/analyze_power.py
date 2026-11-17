"""TVLA on Synopsys aggregate power, never on synthetic VCD toggle counts."""
import argparse
import csv
import hashlib
import json
import re
import os
from pathlib import Path
os.environ['MPLCONFIGDIR'] = str(Path(__file__).resolve().parent/'out/runtime/matplotlib')
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def read_power_out(path, bin_ps, last_tick, signal_name=None):
    """Read the installed PT's OUT 5.3 Pc aggregate waveform (ns library units).

    Values are piecewise-constant power samples. Require grid-aligned timestamps
    rather than silently interpolating or confusing filename suffixes with units.
    """
    values = np.full(int(last_tick // bin_ps), np.nan, dtype=np.float64)
    scale_ns = None
    signal = None
    timestamp = None
    previous_bin = None
    previous_value = None
    last_timestamp = -1
    with Path(path).open() as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith(";"):
                continue
            if line.startswith(".time_resolution"):
                scale_ns = float(line.split()[1])
            elif line.startswith(".index"):
                fields = line.split()
                if fields[1].startswith("Pc(") and fields[-1] == "Pc" and (signal_name is None or fields[1] == signal_name):
                    if signal is not None:
                        raise ValueError("More than one Pc signal; select one explicitly")
                    signal = int(fields[2])
            elif re.fullmatch(r"\d+", line):
                if scale_ns is None:
                    raise ValueError("Missing OUT time resolution")
                timestamp = int(line) * scale_ns * 1000.0
                if abs(timestamp - round(timestamp)) > 1e-5:
                    raise ValueError("Sub-picosecond timestamp unsupported")
                timestamp = round(timestamp)
                if timestamp < last_timestamp or timestamp % bin_ps:
                    raise ValueError("Nonmonotonic or off-grid power timestamp")
                last_timestamp = timestamp
            elif line.startswith("."):
                # Metadata in this installed format does not carry sample data.
                continue
            else:
                fields = line.split()
                if len(fields) != 2 or not fields[0].isdigit():
                    raise ValueError(f"Unsupported OUT record: {line}")
                if int(fields[0]) != signal:
                    continue
                if timestamp is None:
                    raise ValueError("Power value without timestamp")
                value = float(fields[1])
                if not np.isfinite(value):
                    raise ValueError("Nonfinite power sample")
                index = timestamp // bin_ps
                if previous_bin is not None:
                    values[previous_bin:min(index, len(values))] = previous_value
                previous_bin, previous_value = index, value
    if signal is None or previous_bin is None or last_timestamp < last_tick:
        raise ValueError("Missing aggregate signal or incomplete waveform coverage")
    values[previous_bin:] = previous_value
    if not np.isfinite(values).all():
        raise ValueError("Uncovered samples in power waveform")
    return values, scale_ns


def welch(fixed, random):
    if len(fixed) < 2 or len(random) < 2:
        raise ValueError("Each class needs at least two traces")
    difference = fixed.mean(axis=0) - random.mean(axis=0)
    denominator = np.sqrt(fixed.var(axis=0, ddof=1) / len(fixed) + random.var(axis=0, ddof=1) / len(random))
    t = np.divide(difference, denominator, out=np.zeros_like(difference), where=denominator > 0)
    deterministic_difference = (denominator == 0) & (difference != 0)
    t[deterministic_difference] = np.sign(difference[deterministic_difference]) * np.inf
    return t, int(((denominator == 0) & (difference == 0)).sum()), int(deterministic_difference.sum())


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--run", type=Path, required=True)
    p.add_argument("--bin-ps", type=int, default=5)
    p.add_argument("--time-unit-ns", type=float, default=1.0,
                   help="Must match the ns unit confirmed in the PrimeTime report")
    args = p.parse_args()
    if args.bin_ps <= 0 or args.time_unit_ns != 1.0:
        p.error("This validated format adapter requires ns units and positive bin size")
    with (args.run / "trace_manifest.csv").open() as f:
        rows = list(csv.DictReader(f))
    if not rows or [int(r["trace_id"]) for r in rows] != list(range(len(rows))):
        raise ValueError("Empty, duplicate or discontinuous trace manifest")
    labels = np.array([int(r["class"]) for r in rows])
    if set(labels) != {0, 1}:
        raise ValueError("Manifest must contain classes 0 and 1")
    starts = np.rint([float(r["start_ns"]) * 1000 for r in rows]).astype(np.int64)
    ends = np.rint([float(r["end_ns"]) * 1000 for r in rows]).astype(np.int64)
    widths = ends - starts
    if (starts % args.bin_ps).any() or (widths <= 0).any() or (widths % args.bin_ps).any():
        raise ValueError("Invalid/off-grid trace windows")
    if not (widths == widths[0]).all() or (starts[1:] < ends[:-1]).any():
        raise ValueError("Unequal or overlapping trace windows")
    power, resolution = read_power_out(args.run / "power.out", args.bin_ps, int(ends[-1]))
    traces = np.stack([power[s // args.bin_ps:e // args.bin_ps] for s, e in zip(starts, ends)])
    fixed, random = traces[labels == 0], traces[labels == 1]
    t, constant_bins, deterministic_bins = welch(fixed, random)
    finite_t = t[np.isfinite(t)]
    maximum = float(np.max(np.abs(finite_t))) if len(finite_t) else 0.0
    summary = {
        "measurement": "Synopsys PrimeTime PX/PrimePower aggregate Pc waveform",
        "power_unit": "W", "library_time_unit": "ns", "out_tick_ns": resolution,
        "bin_ps": args.bin_ps, "trace_width_ps": int(widths[0]),
        "trace_count": len(rows), "class_counts": [len(fixed), len(random)],
        "max_abs_t": maximum if not deterministic_bins else "infinite_deterministic_difference",
        "bins_above_4_5": int((np.abs(t) > 4.5).sum()),
        "identical_constant_bins": constant_bins,
        "deterministic_difference_bins": deterministic_bins,
        "mean_measured_power_W": float(traces.mean()),
        "conventional_screen_crossed": bool((np.abs(t) > 4.5).any()),
        "interpretation": "Pilot leakage screening; no security or attack-success claim.",
        "power_sha256": hashlib.sha256((args.run / "power.out").read_bytes()).hexdigest(),
    }
    (args.run / "tvla_summary.json").write_text(json.dumps(summary, indent=2, allow_nan=False) + "\n")
    time_ns = np.arange(traces.shape[1]) * args.bin_ps / 1000
    fig, axes = plt.subplots(2, 1, figsize=(10, 6), sharex=True, constrained_layout=True)
    axes[0].plot(time_ns, fixed.mean(axis=0) * 1e3, label="Class 0")
    axes[0].plot(time_ns, random.mean(axis=0) * 1e3, label="Class 1", alpha=0.75)
    axes[0].set_ylabel("Mean Synopsys power (mW)")
    axes[0].legend()
    axes[1].plot(time_ns, np.where(np.isfinite(t), t, np.nan))
    axes[1].axhline(4.5, color="firebrick", linestyle="--")
    axes[1].axhline(-4.5, color="firebrick", linestyle="--")
    axes[1].set_ylabel("Welch t")
    axes[1].set_xlabel("Time from measured stimulus (ns)")
    fig.suptitle(f"{args.run.name}: {len(rows)} traces, {args.bin_ps} ps bins")
    fig.savefig(args.run / "tvla.png", dpi=150)
    plt.close(fig)
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
