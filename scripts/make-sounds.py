#!/usr/bin/env python3
"""Procedural Cast Recorder sounds: soft bell chimes (start rising, stop falling) and a soft tick.
48 kHz mono 16-bit, peak -24 dBFS (tick -28 dBFS). Usage: make-sounds.py OUTDIR"""
import math, struct, sys, wave
SR = 48000
def bell(f, t, decay):
    # fundamental + soft upper partials (slightly stretched, like a small felt-mallet bell)
    parts = [(1.0, 1.0, 1.0), (2.0, 0.22, 1.6), (3.01, 0.07, 2.4), (4.2, 0.025, 3.2)]
    return sum(a * math.sin(2 * math.pi * f * m * t) * math.exp(-t * decay * k) for m, a, k in parts)
def env(t, attack):
    return 0.5 - 0.5 * math.cos(math.pi * t / attack) if t < attack else 1.0
def render(notes, length, peak_db, decay=14.0, attack=0.006):
    n = int(SR * length)
    out = [0.0] * n
    for start, f, gain in notes:
        for i in range(int(SR * start), n):
            t = (i - SR * start) / SR
            out[i] += gain * env(t, attack) * bell(f, t, decay)
    fade = int(SR * 0.04)  # smooth tail to zero
    for i in range(fade): out[n - fade + i] *= 0.5 + 0.5 * math.cos(math.pi * i / fade)
    pk = max(abs(x) for x in out)
    g = 10 ** (peak_db / 20) / pk
    return [x * g for x in out]
def save(path, s):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(round(x * 32767))) for x in s))
out = sys.argv[1] if len(sys.argv) > 1 else "."
C5, G5 = 523.25, 783.99
save(f"{out}/start.wav", render([(0.0, C5, 0.8), (0.075, G5, 1.0)], 0.23, -24))
save(f"{out}/stop.wav", render([(0.0, G5, 0.8), (0.075, C5, 1.0)], 0.23, -24))
save(f"{out}/tick.wav", render([(0.0, 1318.5, 1.0)], 0.15, -28, decay=40.0, attack=0.003))
