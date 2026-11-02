#!/usr/bin/env python3
"""Run native regression checks without accessing a board."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = {
    "rotary_cache": ["-Ilib/runtime", "test/rotary_cache_host.cpp"],
    "rotary_input": ["-Itest/input_stubs", "-Ilib/button", "-Ilib/runtime", "test/rotary_input_host.cpp", "lib/button/button.cpp"],
    "receive_deadline": ["-Iinclude", "test/receive_deadline_host.cpp"],
    "transfer_gate": ["-Ilib/OTA", "test/transfer_gate_host.cpp"],
    "frame_publication": ["-Ilib/led", "test/frame_publication_host.cpp"],
    "frame_transition": ["-Ilib/led", "test/frame_transition_host.cpp"],
    "fm6373c_protocol": ["-Ilib/led", "test/fm6373c_protocol_host.cpp", "lib/led/fm6373c_protocol.cpp"],
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
