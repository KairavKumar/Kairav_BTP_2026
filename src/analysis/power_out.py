"""Preserved OUT parser and Welch implementation; extracted verbatim from secure_skew_demo/analyze_power.py."""
import re
from pathlib import Path
import numpy as np

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
