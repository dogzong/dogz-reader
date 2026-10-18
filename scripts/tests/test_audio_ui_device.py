#!/usr/bin/env python3
"""Real Waveshare board regression: page transitions and clean framebuffers.

Opens USB serial (the ESP32-S3 may reset), then only navigates and reads frames.
Never records audio, writes the SD card, or changes settings.
"""
import argparse
import hashlib
from pathlib import Path
import time
import serial


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--port', default='/dev/cu.usbmodem101')
    parser.add_argument('--output', type=Path, default=Path('/tmp/crossmux-audio-ui-test'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with serial.Serial(args.port, 115200, timeout=.2) as device:
        transcript = (args.output / 'serial.log').open('wb')

        def line_until(needle, seconds=15):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                line = device.readline()
                transcript.write(line)
                transcript.flush()
                if b'Guru Meditation' in line or b'assert failed' in line:
                    raise AssertionError(line.decode(errors='replace'))
                if needle in line:
                    return line
            raise TimeoutError(f'No serial response: {needle!r}')

        def command(name):
            device.write(f'CMD:{name}\n'.encode())
            device.flush()

        def navigate(name, activity):
            command(name)
            line_until(f'Entering activity: {activity}'.encode())
            line_until(b'Wait complete: refresh')

        def frame(name):
            command('SCREENSHOT')
            marker = line_until(b'SCREENSHOT_START:')
            size = int(marker.split(b'SCREENSHOT_START:')[1].strip())
            assert size == 48000, f'Unexpected panel buffer: {size}'
            deadline = time.monotonic() + 15
            data = bytearray()
            while len(data) < size and time.monotonic() < deadline:
                data.extend(device.read(size - len(data)))
            assert len(data) == size, f'Incomplete screenshot: {len(data)}'
            line_until(b'SCREENSHOT_END')
            assert len(set(data)) > 2, 'Framebuffer appears blank'
            (args.output / f'{name}.bin').write_bytes(data)
            try:
                from PIL import Image
                Image.frombytes('1', (800, 480), bytes(data)).rotate(270, expand=True).save(args.output / f'{name}.png')
            except ImportError:
                pass
            print(f'{name}: {hashlib.sha256(data).hexdigest()}', flush=True)
            return data

        # Wait through SD font discovery and initial screen rendering.
        time.sleep(8)
        device.reset_input_buffer()
        navigate('OPEN_APPS', 'AppsMenu')
        apps = frame('apps')
        navigate('OPEN_AUDIO', 'Audio')
        audio = frame('audio')
        assert audio != apps, 'Audio page retained the apps framebuffer'
        # Re-enter from a very different screen three times. Missing clearScreen
        # or navigation deadlocks would change the pixels or stop responding.
        for iteration in range(3):
            navigate('OPEN_APPS', 'AppsMenu')
            navigate('OPEN_AUDIO', 'Audio')
            repeated = frame(f'audio-repeat-{iteration}')
            assert repeated == audio, 'Audio home is not stable across transitions'
        navigate('OPEN_MUSIC', 'Music')
        music = frame('music')
        assert music != audio, 'Music app did not open its own page'
        navigate('OPEN_AI', 'VoiceAssistant')
        ai = frame('ai-home')
        assert ai != audio and ai != music, 'AI assistant did not open its own page'
        navigate('OPEN_APPS', 'AppsMenu')
        transcript.close()
    print('PASS: 4 audio entries, clean stable framebuffer, serial responsive')


if __name__ == '__main__':
    main()
