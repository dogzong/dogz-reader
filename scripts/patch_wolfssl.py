from pathlib import Path

Import("env")


PROJECT_DIR = Path(env.subst("$PROJECT_DIR"))
# Read the configured dependency root instead of assuming `.pio/libdeps`.
# PlatformIO moves the whole directory when PLATFORMIO_LIBDEPS_DIR is set, and
# a hardcoded default then matched nothing: the script became a silent no-op
# and shipped a wolfSSL without WOLFSSL_SHA384, which broke every handshake to
# the Reader AI server with -232 (HASH_TYPE_E). Mirrors patch_sdfat.py.
LIBDEPS_DIR = Path(env.subst("$PROJECT_LIBDEPS_DIR"))
MARKER = "/* CrossPoint wolfSSL compatibility overrides */"
OVERRIDES = f"""

{MARKER}
#undef NO_DH
#ifndef HAVE_FFDHE_2048
#define HAVE_FFDHE_2048
#endif
/* MEMFIX-PORT: 8192 handles up to RSA-4096 keys (the public-CA maximum,
   ISRG Root X1 included) with half the per-bignum heap of 16384: with
   WOLFSSL_SMALL_STACK each fast-math temp is FP_MAX_BITS/8 * 2 bytes on the
   heap, and TLS cert verification allocates dozens at once. */
#undef FP_MAX_BITS
#define FP_MAX_BITS 8192
/* Waveshare Reader AI verifies public ECDSA/SHA-384 certificate chains.
   Enable only this target; other devices keep their existing crypto budget. */
#if defined(FREEINK_DEVICE_WAVESHARE_EPAPER_397)
#ifndef WOLFSSL_SHA384
#define WOLFSSL_SHA384
#endif
/* The Reader AI endpoint may be the server's LAN address (an IP literal) so
   uploads stop crossing the public internet. wolfSSL only compares iPAddress
   SANs when this is on: CheckAltName() reads altName->ipString behind
   `#if defined(OPENSSL_ALL) || defined(WOLFSSL_IP_ALT_NAME)` (internal.c). */
#ifndef WOLFSSL_IP_ALT_NAME
#define WOLFSSL_IP_ALT_NAME
#endif
#endif
"""


def display(path: Path) -> str:
    # The dependency tree can live outside the project (PLATFORMIO_LIBDEPS_DIR),
    # where relative_to() raises; fall back to the absolute path.
    try:
        return str(path.relative_to(PROJECT_DIR))
    except ValueError:
        return str(path)


def patch_user_settings(path: Path) -> None:
    text = path.read_text()
    if MARKER in text:
        text = text.split(MARKER, 1)[0].rstrip()
    path.write_text(text + OVERRIDES + "\n")
    print(f"Patched wolfSSL settings: {display(path)}")


patched = 0
for settings in sorted(LIBDEPS_DIR.glob("*/Arduino-wolfSSL/src/user_settings.h")):
    patch_user_settings(settings)
    patched += 1

# Fail closed. A build that links wolfSSL but patched nothing compiles an
# unreviewed crypto configuration, and the loss of SHA-384 only shows up as a
# runtime -232 on the device.
if not patched and "FREEINK_NET_WOLFSSL=1" in env.subst("$BUILD_FLAGS"):
    raise RuntimeError(
        f"Arduino-wolfSSL user_settings.h not found under {LIBDEPS_DIR}; "
        "the Reader AI build requires the SHA-384 overrides")
