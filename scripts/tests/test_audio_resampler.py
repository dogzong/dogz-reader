#!/usr/bin/env python3
"""Compile and test the production streaming resampler without hardware."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as directory:
    exe=Path(directory)/'resampler-test'
    subprocess.run(['c++','-std=c++17','-include','initializer_list','-I',str(root/'lib/hal'),str(root/'test/reader_ai/AudioResamplerTest.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: 8/16/22.05/44.1/48 kHz duration, DC continuity and 440 Hz pitch')
