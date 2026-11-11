#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ReadyMedia.no and Himmel contributors
"""
Make the HAP test clips in media/test/ (not in git: they are ~90 MB and
easy to recreate).

    python tools/make_test_clips.py [--force]

Needs ffmpeg on PATH (an external tool, never linked into Himmel). Each clip
covers one case the HAP decoder and the clip player must handle:

    hap_1080p_chunks4.mov           Hap (DXT1), 1920x1080, 30 fps, 10 s, 4 chunks
    hapq_1080p.mov                  Hap Q (YCoCg DXT5), 1920x1080, 30 fps, 5 s
    hapalpha_720p.mov               Hap Alpha (DXT5), 1280x720, 30 fps, 5 s, alpha ramp
    hap_716p_nocompress_25fps.mov   Hap, 1276x716 (not 16:9), 25 fps, 3 s, no Snappy

The pictures are ffmpeg's built-in test pattern (testsrc), so there is
nothing to license. Existing files are kept unless --force is given.
Afterwards each clip is checked with hap_probe.exe if it has been built.
"""

import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "media", "test")
PROBE = os.path.join(ROOT, "build", "bin", "Release", "hap_probe.exe")

# name -> (size, fps, seconds, extra filter, encoder options)
CLIPS = {
    "hap_1080p_chunks4.mov": ("1920x1080", 30, 10, "", ["-format", "hap", "-chunks", "4"]),
    "hapq_1080p.mov": ("1920x1080", 30, 5, "", ["-format", "hap_q"]),
    # alpha ramps from transparent (left) to opaque (right)
    "hapalpha_720p.mov": ("1280x720", 30, 5, ",geq=r='r(X,Y)':g='g(X,Y)':b='b(X,Y)':a='255*X/W'",
                          ["-format", "hap_alpha"]),
    "hap_716p_nocompress_25fps.mov": ("1276x716", 25, 3, "", ["-format", "hap", "-compressor", "none"]),
}


def main():
    force = "--force" in sys.argv[1:]
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        print("ffmpeg not found on PATH. Install it (for example: winget install Gyan.FFmpeg) and try again.")
        return 1
    os.makedirs(OUT, exist_ok=True)
    failed = 0
    for name, (size, fps, secs, vf_extra, opts) in CLIPS.items():
        path = os.path.join(OUT, name)
        if os.path.exists(path) and not force:
            print("keep  %s (exists; --force remakes it)" % name)
        else:
            cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y",
                   "-f", "lavfi", "-i", "testsrc=size=%s:rate=%d:duration=%d" % (size, fps, secs),
                   "-vf", "format=rgba" + vf_extra, "-c:v", "hap", *opts, path]
            print("make  %s" % name)
            if subprocess.run(cmd).returncode != 0:
                print("      ffmpeg failed")
                failed += 1
                continue
        if os.path.exists(PROBE):
            r = subprocess.run([PROBE, path], capture_output=True, text=True)
            for line in r.stdout.strip().splitlines()[1:]:
                print("     " + line)
            if r.returncode != 0:
                failed += 1
    print("done, %d failed" % failed if failed else "done: %s" % OUT)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
