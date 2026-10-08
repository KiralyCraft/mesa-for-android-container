#!/usr/bin/env python3
"""Exercise the production cache/retirement code with fake X/DRI resources.

Run on a build host: python3 hdmi_pipeline_cache_test.py [--cc gcc]
No GPU, X server or device is used. Allocation, destruction, cache admission,
locking and the real retirement worker are compiled from the production header.
"""
import argparse
import pathlib
import subprocess
import tempfile

here = pathlib.Path(__file__).resolve().parent
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cc', default='cc')
args = parser.parse_args()
source = (here.parent / 'loader_dri3_hdmi_pipeline.h').read_text()
policy = source.split('static unsigned\nhdmi_pipe_outstanding(', 1)[1].split('static int\nhdmi_pipe_thread(', 1)[0]
policy = 'static unsigned\nhdmi_pipe_outstanding(' + policy
source = source.split('static void\nhdmi_pipe_events(', 1)[0]
fixture = (here / 'hdmi_pipeline_cache_test.c').read_text()
assert fixture.count('/* PRODUCTION */') == 1
with tempfile.TemporaryDirectory(prefix='hdmi-cache-test-') as temp:
    root = pathlib.Path(temp)
    (root / 'test.c').write_text(fixture.replace('/* PRODUCTION */', source + policy))
    subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Wno-unused-function',
                    '-Wno-sign-compare', '-g', '-O1', '-fsanitize=address,undefined',
                    '-pthread', '-I', str(here.parent), str(root / 'test.c'),
                    str(here.parent / 'loader_dri3_pacer.c'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True, timeout=30)
