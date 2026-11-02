#!/usr/bin/env python3
"""Run native regression checks without accessing a board."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "color_menu_input": ["-Itest/input_stubs", "-Ilib/button", "-Ilib/runtime", "test/color_menu_input_host.cpp", "lib/button/button.cpp"],
    "tween": ["-Ilib/ui", "test/tween_host.cpp"],
    "rotary_cache": ["-Ilib/runtime", "test/rotary_cache_host.cpp"],
    "rotary_input": ["-Itest/input_stubs", "-Ilib/button", "-Ilib/runtime", "test/rotary_input_host.cpp", "lib/button/button.cpp"],
    "fm6124_presentation": ["-Itest/display_stubs", "-Iinclude", "-Ilib/led", "-DPIXEL_COLOR_DEPTH_BITS=8", "-DS3_LCD_DIV_NUM=10", "test/fm6124_presentation_host.cpp", "lib/led/led.cpp"],
    "frame_publication": ["-Ilib/led", "test/frame_publication_host.cpp"],
    "frame_transition": ["-Ilib/led", "test/frame_transition_host.cpp"],
    "receive_deadline": ["-Iinclude", "test/receive_deadline_host.cpp"],
    "transfer_gate": ["-Ilib/OTA", "test/transfer_gate_host.cpp"],
    "pixel_frame": ["-Ilib/led", "test/pixel_frame_host.cpp"],
    "protocol": ["-Iinclude", "test/protocol_host.cpp"],
    "brightness": ["-Ilib/led", "test/brightness_policy_host.cpp"],
    "life_redraw": ["-Itest/host_stubs", "-Ilib/game", "test/life_redraw_host.cpp", "lib/game/GameOfLife.cpp"],
}
parser = argparse.ArgumentParser()
parser.add_argument("--sanitize", action="store_true")
options = parser.parse_args()
san_flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if options.sanitize else []
with tempfile.TemporaryDirectory(prefix="pixelpad-rtos-tests-") as directory:
    for name, args in CASES.items():
        binary = str(Path(directory) / name)
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++11", "-Wall", "-Wextra", "-Werror",
                        *san_flags, "-g", *args, "-o", binary],
                       cwd=ROOT, check=True)
        subprocess.run([binary], check=True)
        print(f"PASS {name}", flush=True)
