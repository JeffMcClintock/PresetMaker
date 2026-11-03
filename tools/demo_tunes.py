"""Render short demo tunes for fitted FretPlucked presets, target (gm.dls) and fitted side by side.

Usage: python tools/demo_tunes.py [outdir]   (default examples/FretPlucked/audio)
Each note is rendered once with pmcli, then mixed at its onset in Python; the same sequence
drives the gm.dls render and the plugin render, so the two files are note-for-note identical.
"""
import os, subprocess, sys, tempfile, wave
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PMCLI = os.path.join(ROOT, "build", "Release", "pmcli.exe")
PLUGIN = r"C:\Program Files\Common Files\VST3\FretPlucked.vst3"
PRESETS = os.path.join(ROOT, "examples", "FretPlucked", "presets")
SR = 48000
BEAT = 0.5          # 120 BPM
LENGTH = 6.5        # seconds per file
PEAK = 0.89         # -1 dBFS
CACHE = os.path.join(tempfile.gettempdir(), "pm_demo_notes")

# (beat, key, hold seconds, velocity)
def melody(vel=100):
    keys = [76, 79, 76, 74, 72, 74, 71, 74]
    return [(b, k, 0.45, vel) for b, k in enumerate(keys)] + [(8, 72, 2.0, vel)]

def block(beat, keys, hold, vel, stagger=0.0):
    return [(beat + i * stagger / BEAT, k, hold, vel) for i, k in enumerate(keys)]

def piano():
    s = melody()
    s += block(0, [48, 52, 55], 0.95, 80) + block(2, [48, 52, 55], 0.95, 70)
    s += block(4, [45, 48, 53], 0.95, 80) + block(6, [47, 50, 55], 0.95, 80)
    s += block(8, [36, 48, 52, 55, 60], 2.0, 85, 0.012)
    return s

def harpsichord():
    s = melody()
    for bar, chord in ((0, [48, 55, 52, 55]), (4, [41, 48, 45, 48]), (6, [43, 50, 47, 50])):
        n = 8 if bar == 0 else 4
        s += [(bar + i * 0.5, chord[i % 4], 0.22, 100) for i in range(n)]
    s += block(8, [36, 48, 52, 55, 60], 2.0, 100, 0.012)
    return s

def nylon_guitar():
    s = melody()
    s += block(0, [48, 52, 55, 60, 64], 0.95, 75, 0.02) + block(2, [48, 52, 55, 60, 64], 0.95, 65, 0.02)
    s += block(4, [41, 45, 48, 53, 57], 0.95, 75, 0.02) + block(6, [43, 47, 50, 55, 59], 0.95, 75, 0.02)
    s += block(8, [48, 52, 55, 60, 64, 67], 2.0, 80, 0.025)
    return s

def acoustic_bass():
    keys = [36, 40, 43, 45, 41, 45, 43, 47]
    return [(b, k, 0.45, 100) for b, k in enumerate(keys)] + [(8, 36, 2.0, 100)]

def harp():
    s = melody()
    s += block(0, [48, 52, 55, 60], 1.9, 85, 0.06)
    s += block(4, [41, 45, 48, 53], 0.95, 85, 0.06) + block(6, [43, 47, 50, 55], 0.95, 85, 0.06)
    s += block(8, [36, 43, 48, 52, 55, 60, 64, 67], 2.0, 85, 0.05)
    return s

TUNES = [  # (preset name, gm program, file stem, sequence)
    ("Acoustic Grand Piano", 0, "acoustic_grand_piano", piano),
    ("Harpsichord", 6, "harpsichord", harpsichord),
    ("Nylon Guitar", 24, "nylon_guitar", nylon_guitar),
    ("Acoustic Bass", 32, "acoustic_bass", acoustic_bass),
    ("Harp", 46, "harp", harp),
]

def preset_args(name):
    args = []
    for line in open(os.path.join(PRESETS, name + ".txt"), encoding="utf-8"):
        parts = line.rstrip("\n").split("\t")
        if len(parts) == 3 and int(parts[0]) < 10000:
            args.append(f"{parts[2]}={parts[1]}")
    return args

def read_wav(path):
    """Minimal RIFF reader: pmcli writes 32-bit float, which the wave module rejects."""
    data = open(path, "rb").read()
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        tag, size = data[pos:pos + 4], int.from_bytes(data[pos + 4:pos + 8], "little")
        body = data[pos + 8:pos + 8 + size]
        if tag == b"fmt ":
            fmt = (int.from_bytes(body[0:2], "little"), int.from_bytes(body[2:4], "little"),
                   int.from_bytes(body[4:8], "little"), int.from_bytes(body[14:16], "little"))
        elif tag == b"data":
            pcm = body
        pos += 8 + size + (size & 1)
    tag, ch, sr, bits = fmt
    assert sr == SR and ch == 1, (path, fmt)
    if tag == 3 and bits == 32:
        return np.frombuffer(pcm, np.float32).astype(np.float64)
    if tag == 1 and bits == 16:
        return np.frombuffer(pcm, np.int16).astype(np.float64) / 32768
    raise ValueError(f"{path}: format {fmt}")

def write_wav(path, x):
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes((np.clip(x, -1, 1) * 32767).astype(np.int16).tobytes())

def render_note(kind, ident, key, vel, hold, extra):
    os.makedirs(CACHE, exist_ok=True)
    out = os.path.join(CACHE, f"{kind}_{ident}_{key}_{vel}_{hold:.2f}.wav")
    if not os.path.exists(out):
        common = ["--hold", f"{hold}", "--tail", "1.0"]
        if kind == "gm":
            cmd = [PMCLI, "gm-render", str(ident), str(key), str(vel), out] + common
        else:
            cmd = [PMCLI, "vst-render", PLUGIN, str(key), str(vel), out, "--preroll", "0.25"] + common + extra
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode or not os.path.exists(out):
            raise RuntimeError(f"{' '.join(cmd[:6])}: {r.stdout}{r.stderr}")
    x = read_wav(out)
    onset = np.argmax(np.abs(x) > 1e-3 * max(np.abs(x).max(), 1e-9))  # trim leading silence
    return x[onset:]

def mix(kind, ident, seq, extra):
    n = int(LENGTH * SR)
    y = np.zeros(n)
    for beat, key, hold, vel in seq:
        x = render_note(kind, ident, key, vel, hold, extra)
        start = int(beat * BEAT * SR)
        x = x[: n - start]
        y[start:start + len(x)] += x
    fade = int(0.3 * SR)
    y[-fade:] *= np.linspace(1, 0, fade)
    return y * (PEAK / max(np.abs(y).max(), 1e-9))

def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "examples", "FretPlucked", "audio")
    os.makedirs(outdir, exist_ok=True)
    for name, prog, stem, tune in TUNES:
        seq = tune()
        for kind, ident, extra, suffix in (("gm", prog, [], "target"), ("vst", stem, preset_args(name), "fitted")):
            y = mix(kind, ident, seq, extra)
            path = os.path.join(outdir, f"{stem}_{suffix}.wav")
            write_wav(path, y)
            rms = 20 * np.log10(np.sqrt(np.mean(y * y)))
            print(f"{os.path.basename(path):36} {len(y) / SR:.2f}s  peak {np.abs(y).max():.3f}  rms {rms:6.1f} dBFS  {os.path.getsize(path) // 1024} KB")

if __name__ == "__main__":
    main()
