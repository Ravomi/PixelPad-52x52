#!/usr/bin/env python3
"""Estimate scan rates from the effective PIO flags and pinned HUB75 3.0.15.

This is a timing calculation, not a measurement. No device is accessed.
"""
import configparser
import json
from pathlib import Path
import re
import shlex

ROOT = Path(__file__).resolve().parents[1]


def estimate(name, configured_clock, actual_clock, bits, minimum, double_buffer):
    # Same integer time calculation used by setupDMA() in HUB75 3.0.15.
    ns_per_latch = (52 * (1_000_000_000_000 // configured_clock)) // 1000
    for transition in range(bits):
        passes = bits + (1 << (bits - transition - 1)) - 1
        driver_hz = 1_000_000_000 // (ns_per_latch * passes * 26)
        if driver_hz >= minimum:
            break
    # Each scan is a descriptor ring: 26 rows, 52 words per bitplane pass.
    scan_hz = actual_clock / (52 * 26 * passes)
    descriptors = 26 * (1 << (bits - transition - 1))
    return {
        "profile": name, "clock_hz": actual_clock, "bits_per_channel": bits,
        "double_buffer": double_buffer, "transition_bit": transition,
        "driver_scan_hz_est": driver_hz, "scan_hz_est": round(scan_hz, 3),
        "dma_bytes_est": (52 * 26 * bits * 2 + descriptors * 12) * (2 if double_buffer else 1),
    }


def main():
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / "platformio.ini")

    def expand(section, option):
        value = config.get(section, option)
        return re.sub(r"\$\{([^}]+)\}",
                      lambda match: expand(*match[1].rsplit(".", 1)), value)

    reports = [estimate("legacy_3.2.3", 8_000_000, 10_000_000, 8, 60, False)]
    for section in config.sections():
        if not section.startswith("env:"):
            continue
        flags = shlex.split(expand(section, "build_flags"))
        defines = dict(flag[2:].split("=", 1) for flag in flags if flag.startswith("-D") and "=" in flag)
        clock = 160_000_000 // int(defines["S3_LCD_DIV_NUM"])
        reports.append(estimate(section[4:], clock, clock, int(defines["PIXEL_COLOR_DEPTH_BITS"]),
                                int(defines["PIXELPAD_SCAN_MIN_HZ"]),
                                bool(int(defines["PIXELPAD_DOUBLE_BUFFER"]))))
    baseline = reports[0]["scan_hz_est"]
    for report in reports:
        report["scan_ratio_est"] = round(report["scan_hz_est"] / baseline, 3)
    print(json.dumps({"measured": False, "driver_version": "3.0.15", "profiles": reports}, indent=2))


if __name__ == "__main__":
    main()
