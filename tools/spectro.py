"""Side-by-side spectrograms: python spectro.py out.png label1 a.wav label2 b.wav [more pairs...]
Rows = notes (pairs of ref/cand wavs given as ref.wav cand.wav per note)."""
import struct, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt


def read_wav(path):
    d = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(d):
        cid, sz = d[pos:pos + 4], struct.unpack('<I', d[pos + 4:pos + 8])[0]
        body = d[pos + 8:pos + 8 + sz]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            data = body
        pos += 8 + sz + (sz & 1)
    tag, ch, sr, _, _, bits = fmt
    x = np.frombuffer(data, '<f4' if tag == 3 else '<i2').astype(np.float32)
    if tag != 3: x /= 32768
    return x.reshape(-1, ch).mean(1), sr


def spec(x, sr, n=4096, hop=256):
    w = np.hanning(n)
    f = np.array([np.abs(np.fft.rfft(x[i:i + n] * w)) for i in range(0, len(x) - n, hop)]).T
    return 20 * np.log10(f + 1e-9), np.arange(f.shape[1]) * hop / sr


def main():
    """spectro.py out.png "col title|col title|..." "row label|a.wav|b.wav|..." ...  (one wav per column)"""
    out, titles, rows = sys.argv[1], sys.argv[2].split('|'), [r.split('|') for r in sys.argv[3:]]
    fig, ax = plt.subplots(len(rows), len(titles), figsize=(6.5 * len(titles), 3.6 * len(rows)), squeeze=False)
    for r, (label, *wavs) in enumerate(rows):
        specs = [spec(*read_wav(p)) for p in wavs]
        top = max(s[0].max() for s in specs)
        for a, (S, t), title in zip(ax[r], specs, titles):
            im = a.imshow(S, origin='lower', aspect='auto', cmap='magma', vmin=top - 90, vmax=top,
                          extent=[0, t[-1], 0, 24])
            a.set_ylim(0, 12); a.set_title(f'{label}: {title}'); a.set_xlabel('s'); a.set_ylabel('kHz')
        fig.colorbar(im, ax=ax[r].tolist(), label='dB')
    fig.savefig(out, dpi=90, bbox_inches='tight')


if __name__ == '__main__':
    main()
