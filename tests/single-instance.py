#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Nuaptan Evalisk
# SPDX-License-Identifier: GPL-3.0-only
"""Run under a private dbus-run-session; never invoke the real Tailscale."""
import os, pathlib, subprocess, sys, tempfile, time
with tempfile.TemporaryDirectory(prefix='tailcourier-instance-') as tmp:
    root = pathlib.Path(tmp)
    cli = root / 'tailscale'
    cli.write_text('#!/bin/sh\nexit 1\n')
    cli.chmod(0o700)
    env = dict(os.environ, PATH=tmp, QT_QPA_PLATFORM='offscreen',
               XDG_CONFIG_HOME=tmp+'/config', XDG_DATA_HOME=tmp+'/data', XDG_RUNTIME_DIR=tmp)
    first = subprocess.Popen([sys.argv[1], '--background'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(1)
        assert first.poll() is None, first.stderr.read().decode()
        second = subprocess.run([sys.argv[1], '--background'], env=env, capture_output=True, timeout=5)
        assert second.returncode == 0, second.stderr.decode()
        assert first.poll() is None
    finally:
        first.terminate()
        first.wait(timeout=5)
    restarted = subprocess.Popen([sys.argv[1], '--background'], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        time.sleep(.7)
        assert restarted.poll() is None, restarted.stderr.read().decode()
    finally:
        restarted.terminate()
        restarted.wait(timeout=5)
print('Single instance, graceful exit and restart: PASS')
