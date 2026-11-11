# SPDX-License-Identifier: GPL-2.0-or-later
# Synthetic checks for the Yunseul MR core. Usage: python3 synth_test.py <render-binary> <workdir>
import subprocess, sys, os
import numpy as np
from scipy import signal
from scipy.io import wavfile

RENDER, WORK = sys.argv[1], sys.argv[2]
SR = 48000
rng = np.random.default_rng(7)


def wr(name, x):
    p = os.path.join(WORK, name)
    wavfile.write(p, SR, x.astype(np.float32))
    return p


def run(inp, out, *args):
    o = os.path.join(WORK, out)
    r = subprocess.run([RENDER, inp, o, *args], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    sr, y = wavfile.read(o)
    return y, r.stdout, r.stderr


def pink(n):
    w = rng.standard_normal(n)
    X = np.fft.rfft(w)
    f = np.fft.rfftfreq(n, 1 / SR)
    f[0] = f[1]
    x = np.fft.irfft(X / np.sqrt(f), n)
    return x / np.max(np.abs(x)) * 0.5


def peak(x, f0, gain_db, q):
    A = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * f0 / SR
    al = np.sin(w0) / (2 * q)
    b = [1 + al * A, -2 * np.cos(w0), 1 - al * A]
    a = [1 + al / A, -2 * np.cos(w0), 1 - al / A]
    return signal.lfilter(b, a, x)


def spec_db(x):
    f, p = signal.welch(x, SR, nperseg=8192, noverlap=4096)
    return f, 10 * np.log10(p + 1e-20)


def at(f, d, f0, width=0.04):
    m = (f > f0 * (1 - width)) & (f < f0 * (1 + width))
    return float(np.mean(d[m]))


def st(x):
    return np.stack([x, x], axis=1)


ok = True


def check(name, cond, detail):
    global ok
    ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name + " | " + detail)


N = SR * 12
pk = pink(N)

# 1. amount 0 = bit-exact
p = wr("pink.wav", st(pk))
y, _, _ = run(p, "pink_a0.wav", "--amount", "0")
check("bypass at amount 0", np.array_equal(y, st(pk).astype(np.float32)), "max diff %.3g" % np.max(np.abs(y - st(pk).astype(np.float32))))

# 2. zero latency: impulse + cross-correlation
imp = np.zeros(SR)
imp[1000] = 0.9
p = wr("imp.wav", st(imp))
y, _, _ = run(p, "imp_o.wav", "--amount", "1")
check("impulse stays on its sample", int(np.argmax(np.abs(y[:, 0]))) == 1000, "peak at %d" % int(np.argmax(np.abs(y[:, 0]))))
res = peak(peak(pk, 3000, 12, 8), 300, 10, 6)
res = res / np.max(np.abs(res)) * 0.5
p_res = wr("res.wav", st(res))
y_res, stats, err = run(p_res, "res_o.wav", "--amount", "0.5", "--stats")
seg = slice(SR * 2, SR * 6)
c = signal.correlate(y_res[seg, 0], res[seg], "full", method="fft")
lag = int(np.argmax(c)) - (len(res[seg]) - 1)
check("latency (cross-correlation)", lag == 0, "lag %d samples" % lag)

# 3. flat (pink) material is left alone
y, s_pink, _ = run(wr("pink.wav", st(pk)), "pink_o.wav", "--amount", "0.5", "--stats")
f, a = spec_db(pk[SR:])
_, b = spec_db(y[SR:, 0])
band = (f > 80) & (f < 15000)
diff = b - a
check("pink noise barely touched (amount 50%)", np.mean(diff[band]) > -1.0 and np.min(signal.medfilt(diff[band], 31)) > -2.5,
      "mean %.2f dB, worst (smoothed) %.2f dB" % (np.mean(diff[band]), np.min(signal.medfilt(diff[band], 31))))

# 4. resonances get pulled down, neighbours do not
f, a = spec_db(res[SR:])
_, b = spec_db(y_res[SR:, 0])
d = b - a
r3k, r300 = at(f, d, 3000), at(f, d, 300)
off = np.mean([at(f, d, x) for x in (1000, 1500, 6000, 9000)])
check("3 kHz resonance (+12 dB) reduced", r3k < -2.0, "%.1f dB at 3 kHz" % r3k)
check("300 Hz resonance (+10 dB) reduced", r300 < -1.5, "%.1f dB at 300 Hz" % r300)
check("away from resonances untouched", off > -0.8, "%.2f dB avg at 1k/1.5k/6k/9k" % off)
y1, _, _ = run(p_res, "res_o100.wav", "--amount", "1.0")
_, b1 = spec_db(y1[SR:, 0])
check("amount 100% cuts deeper than 50%", at(f, b1 - a, 3000) < r3k - 1.5, "%.1f dB at 3 kHz" % at(f, b1 - a, 3000))

# 5. level independent
q = wr("res_q.wav", st(res * 10 ** (-30 / 20)))
yq, _, _ = run(q, "res_q_o.wav", "--amount", "0.5")
_, aq = spec_db(res[SR:] * 10 ** (-30 / 20))
_, bq = spec_db(yq[SR:, 0])
check("same cut 30 dB quieter", abs(at(f, bq - aq, 3000) - r3k) < 0.3, "%.1f dB vs %.1f dB" % (at(f, bq - aq, 3000), r3k))

# 6. block size does not change the result
ya, _, _ = run(p_res, "res_b1.wav", "--amount", "0.5", "--block", "1")
yb, _, _ = run(p_res, "res_b1024.wav", "--amount", "0.5", "--block", "1024")
check("block size independent", np.array_equal(ya, yb) and np.array_equal(ya, y_res), "1 / 480 / 1024 samples")

# 7. torture: full-scale white, square, silence -> burst, DC
t = np.arange(SR * 4) / SR
tort = np.concatenate([
    rng.uniform(-1, 1, SR * 2),
    np.sign(np.sin(2 * np.pi * 220 * t[: SR * 2])),
    np.zeros(SR * 3),
    rng.uniform(-1, 1, SR // 2),
    np.ones(SR) * 0.8,
    1e-30 * np.ones(SR * 2),
])
yt, _, err_t = run(wr("tort.wav", st(tort)), "tort_o.wav", "--amount", "1.0")
check("no NaN / runaway on torture input", np.all(np.isfinite(yt)) and np.max(np.abs(yt)) < 2.0, "peak %.2f" % np.max(np.abs(yt)))

# 8. delta + processed = original
yd, _, _ = run(p_res, "res_delta.wav", "--amount", "0.5", "--delta")
check("delta + output = input", np.max(np.abs(yd[:, 0] + y_res[:, 0] - res.astype(np.float32))) < 1e-5, "max err %.2g" % np.max(np.abs(yd[:, 0] + y_res[:, 0] - res.astype(np.float32))))

# ---- tone balance -------------------------------------------------------------
def target_noise(n):
    # noise shaped like the built-in target: -4.5 dB/oct overall, extra roll-off above 10 kHz
    w = rng.standard_normal(n)
    X = np.fft.rfft(w)
    f = np.fft.rfftfreq(n, 1 / SR)
    f[0] = f[1]
    db = -10 * np.log10(f) - 1.5 * np.log2(f / 700.0)
    db = db - np.where(f > 10000, 3.0 * np.log2(f / 10000.0), 0.0)
    db[f < 40] = -200
    x = np.fft.irfft(X * 10 ** (db / 20), n)
    return x / np.max(np.abs(x)) * 0.5


def shelf_lo(x, f0, gain_db):
    A = 10 ** (gain_db / 40)
    w0 = 2 * np.pi * f0 / SR
    al = np.sin(w0) / 2 * np.sqrt(2)
    c = np.cos(w0)
    b = [A * ((A + 1) - (A - 1) * c + 2 * np.sqrt(A) * al), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - 2 * np.sqrt(A) * al)]
    a = [(A + 1) + (A - 1) * c + 2 * np.sqrt(A) * al, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - 2 * np.sqrt(A) * al]
    return signal.lfilter(b, a, x)


NT = SR * 30
tn = target_noise(NT)
late = slice(SR * 15, NT)

y, _, _ = run(wr("tn.wav", st(tn)), "tn_o.wav", "--amount", "0", "--tone", "1")
f, a = spec_db(tn[late])
_, b = spec_db(y[late, 0])
d = b - a
worst = max(abs(at(f, d, x, 0.15)) for x in (100, 200, 400, 800, 1600, 3200, 6400, 10000))
check("tone: on-target material left alone", worst < 1.0, "largest change %.2f dB" % worst)

boomy = shelf_lo(tn, 200, 8)
boomy = boomy / np.max(np.abs(boomy)) * 0.5
y, _, _ = run(wr("boomy.wav", st(boomy)), "boomy_o.wav", "--amount", "0", "--tone", "1")
f, a = spec_db(boomy[late])
_, b = spec_db(y[late, 0])
d = b - a
lo, mid = at(f, d, 100, 0.15), np.mean([at(f, d, x, 0.15) for x in (800, 1600, 3200)])
check("tone: +8 dB of extra low end is pulled back", lo < -3.0 and lo >= -5.5, "%.1f dB at 100 Hz" % lo)
check("tone: mids stay put while lows are cut", abs(mid) < 1.5, "%.2f dB avg at 0.8/1.6/3.2 kHz" % mid)
c = signal.correlate(y[SR * 20 : SR * 24, 0], boomy[SR * 20 : SR * 24], "full", method="fft")
lag = int(np.argmax(c)) - (SR * 4 - 1)
check("tone: no added delay", abs(lag) <= 1, "cross-correlation peak at %d samples" % lag)

dull = signal.lfilter(*signal.butter(2, 3500, fs=SR), tn)
dull = dull / np.max(np.abs(dull)) * 0.5
y, _, _ = run(wr("dull.wav", st(dull)), "dull_o.wav", "--amount", "0", "--tone", "1")
f, a = spec_db(dull[late])
_, b = spec_db(y[late, 0])
d = b - a
hi, top = at(f, d, 6000, 0.15), np.max(d[(f > 60) & (f < 16000)])
check("tone: dull top end is lifted, never more than +5 dB", hi > 2.0 and top < 5.8, "%.1f dB at 6 kHz, max %.1f dB" % (hi, top))

sparse = signal.lfilter(*signal.butter(4, [300, 600], btype="band", fs=SR), rng.standard_normal(NT))
sparse = sparse / np.max(np.abs(sparse)) * 0.3
y, _, _ = run(wr("sparse.wav", st(sparse)), "sparse_o.wav", "--amount", "0", "--tone", "1")
dmax = np.max(np.abs(y[late, 0] - sparse[late].astype(np.float32)))
check("tone: sparse material (one instrument) is not re-equalised", dmax < 1e-3, "max difference %.2g" % dmax)

yt2, _, _ = run(os.path.join(WORK, "tort.wav"), "tort_t.wav", "--amount", "1.0", "--tone", "1")
check("tone: no NaN / runaway on torture input", np.all(np.isfinite(yt2)) and np.max(np.abs(yt2)) < 2.5, "peak %.2f" % np.max(np.abs(yt2)))

print("CPU:", err.strip())
print("CPU (torture, denormal tail):", err_t.strip())
print("ALL PASS" if ok else "SOME FAILED")
