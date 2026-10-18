#!/usr/bin/env python3
"""Compile and exercise the actual firmware protocol client with a fake transport."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
# PLATFORMIO_LIBDEPS_DIR relocates libdeps out of .pio/libdeps, so accept it when
# the default location is absent (the clean-build workflow points it at a tmpdir).
libdeps = Path(os.environ['PLATFORMIO_LIBDEPS_DIR']) if os.environ.get('PLATFORMIO_LIBDEPS_DIR') else root / '.pio/libdeps'
json_headers = libdeps / 'waveshare_epaper_397/ArduinoJson/src'
with tempfile.TemporaryDirectory() as directory:
    binary = str(Path(directory) / 'reader-ai-test')
    subprocess.run(['c++', '-std=c++17', '-I'+str(root/'src'), '-I'+str(json_headers),
                    str(root/'src/network/ReaderAiClient.cpp'), str(root/'test/reader_ai/ReaderAiTest.cpp'),
                    '-o', binary], check=True)
    subprocess.run([binary], check=True)
