"""Link Espressif's MP3/simple-decoder archives for the Waveshare S3 profile."""

import os

Import("env")
project_dir = env.subst("$PROJECT_DIR")
codec_root = os.path.join(project_dir, "lib", "esp-audio-codec")
include_root = os.path.join(codec_root, "include")
env.Append(CPPPATH=[
    include_root,
    os.path.join(include_root, "decoder"),
    os.path.join(include_root, "decoder", "impl"),
    os.path.join(include_root, "simple_dec"),
    os.path.join(include_root, "simple_dec", "impl"),
])
env.Append(LIBPATH=[os.path.join(codec_root, "lib", "esp32s3")])
# The simple decoder references the MP3 decoder, so keep this order for linkers
# that process archives left to right. PlatformIO also wraps project libs in a
# link group for ESP targets.
env.Append(LIBS=["esp_audio_simple_dec", "esp_audio_codec"])
