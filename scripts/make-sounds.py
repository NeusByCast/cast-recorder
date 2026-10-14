#!/usr/bin/env python3
"""Procedural Cast Recorder sounds (v2): soft glide / swoosh cues.
Filtered noise swept through a gentle band-pass plus a faint sine glide, Hann-shaped, no clicks.
48 kHz mono 16-bit, peak -34 dBFS (pause tick -38 dBFS). Usage: make-sounds.py OUTDIR"""
import sys, wave
import numpy as np
SR = 48000
rng = np.random.default_rng(7)

def swoosh(length, f0, f1, peak_db, tone=0.35, q=2.2, shape=0.45):
    n = int(SR * length)
    t = np.arange(n) / SR
    x = rng.standard_normal(n)
    # log sweep of the band centre, state-variable band-pass per sample
    fc = f0 * (f1 / f0) ** (t / length)
    y = np.zeros(n); lp = bp = 0.0
    for i in range(n):
        f = 2 * np.sin(np.pi * fc[i] / SR)
        hp = x[i] - lp - bp / q
        bp += f * hp
        lp += f * bp
        y[i] = bp
    y /= np.max(np.abs(y)) + 1e-12
    # faint sine glide an octave below the band for body
    ph = 2 * np.pi * np.cumsum(fc * 0.5) / SR
    y = (1 - tone) * y + tone * np.sin(ph)
    # asymmetric smooth envelope: rise to `shape` of the length, then a long soft tail
    a = int(n * shape)
    env = np.concatenate([0.5 - 0.5 * np.cos(np.linspace(0, np.pi, a)), (0.5 + 0.5 * np.cos(np.linspace(0, np.pi, n - a))) ** 1.5])
    y *= env
    return y / np.max(np.abs(y)) * 10 ** (peak_db / 20)

def save(path, s):
    pcm = np.round(np.clip(s, -1, 1) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(pcm.tobytes())

out = sys.argv[1] if len(sys.argv) > 1 else "."
save(f"{out}/start.wav", swoosh(0.34, 500, 2600, -34))           # upward glide
save(f"{out}/stop.wav", swoosh(0.34, 2600, 500, -34, shape=0.3))  # downward glide
save(f"{out}/tick.wav", swoosh(0.16, 1400, 1100, -38, tone=0.5, shape=0.35))  # pause / resume
# replay saved: two quick rising glides
r = np.concatenate([swoosh(0.18, 700, 2000, -34), np.zeros(int(SR * 0.02)), swoosh(0.22, 900, 2600, -34)])
save(f"{out}/replay.wav", r)
save(f"{out}/shot.wav", swoosh(0.14, 3200, 1600, -36, tone=0.15, shape=0.2))  # screenshot: short airy swish
