#!/usr/bin/env python3
"""Verify the wolfSSL override patch follows the configured dependency root.

Regression guard: the script used to glob `<project>/.pio/libdeps`, so a build
with PLATFORMIO_LIBDEPS_DIR set patched nothing and silently produced a
wolfSSL without WOLFSSL_SHA384 (device handshakes then failed with -232).
"""
from pathlib import Path
import runpy
import tempfile

root = Path(__file__).resolve().parents[2]
script = str(root / 'scripts/patch_wolfssl.py')


def make_env(project, libdeps, flags="-DFREEINK_NET_WOLFSSL=1"):
    class Env:
        def subst(self, key):
            return {"$PROJECT_DIR": str(project),
                    "$PROJECT_LIBDEPS_DIR": str(libdeps),
                    "$BUILD_FLAGS": flags}[key]
    return Env()


def add_wolfssl(libdeps, envname='waveshare_epaper_397'):
    target = libdeps / envname / 'Arduino-wolfSSL' / 'src' / 'user_settings.h'
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text('#define WOLFSSL_USER_SETTINGS\n')
    return target


with tempfile.TemporaryDirectory() as directory:
    base = Path(directory)
    project = base / 'project'
    project.mkdir()
    # The dependency tree lives outside the project, like the real build.
    libdeps = base / 'external-libdeps'
    settings = add_wolfssl(libdeps)

    runpy.run_path(script, init_globals={'env': make_env(project, libdeps), 'Import': lambda _: None})

    text = settings.read_text()
    assert '/* CrossPoint wolfSSL compatibility overrides */' in text, 'marker missing'
    assert '#define WOLFSSL_SHA384' in text, 'SHA-384 override missing'
    assert '#define WOLFSSL_IP_ALT_NAME' in text, 'IP address SAN override missing'
    assert '#define FP_MAX_BITS 8192' in text, 'FP_MAX_BITS override missing'

    # A second run must rewrite, not append a duplicate block.
    runpy.run_path(script, init_globals={'env': make_env(project, libdeps), 'Import': lambda _: None})
    assert settings.read_text().count('/* CrossPoint wolfSSL compatibility overrides */') == 1, \
        'override block duplicated on re-run'

    # Several environments in one dependency root are all patched.
    other = add_wolfssl(libdeps, 'gh_release')
    runpy.run_path(script, init_globals={'env': make_env(project, libdeps), 'Import': lambda _: None})
    assert '#define WOLFSSL_SHA384' in other.read_text(), 'second environment not patched'

    # Fail closed when wolfSSL is required but nothing can be patched.
    empty = base / 'empty-libdeps'
    empty.mkdir()
    try:
        runpy.run_path(script, init_globals={'env': make_env(project, empty), 'Import': lambda _: None})
        raise AssertionError('missing wolfSSL dependency was accepted')
    except RuntimeError:
        pass

    # Targets that do not link wolfSSL are left alone.
    runpy.run_path(script, init_globals={'env': make_env(project, empty, flags='-DFOO=1'),
                                         'Import': lambda _: None})

print('PASS: wolfSSL overrides follow the configured libdeps root; idempotent; fail closed when required')
