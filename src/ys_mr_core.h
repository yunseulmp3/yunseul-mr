// SPDX-License-Identifier: GPL-2.0-or-later
//
// Yunseul MR - dynamic resonance smoother for karaoke backing tracks.
// Copyright (C) 2026 Yunseul (yunseulmp3)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// ---------------------------------------------------------------------------
// How it works (zero latency, no FFT):
//
//   * A bank of constant-Q band-pass filters (fixed coefficients) splits the
//     input into narrow bands. The filters never change, so there is nothing
//     to zipper or blow up.
//   * Every 32 samples the energy of each band is compared with the average
//     of its neighbours (about one octave either side). A band that sticks
//     out above that local average is a "resonance".
//   * The output is  in - sum(g[k] * band[k]).  Subtracting a unity-gain
//     band-pass scaled by g is exactly a bell cut of depth (1 - g), so each
//     band is a dynamic notch whose depth follows how far it sticks out.
//   * Detection is relative, so it does not depend on input level.
//   * Tone balance: the same band energies are averaged over the song and
//     compared with a gentle target slope. The difference (smoothed to about
//     an octave, limited to +-5 dB) is applied slowly through the same bank,
//     so a boomy track loses some low end and a dull track gains some top.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <immintrin.h>
#define YSMR_X86 1
#endif

namespace ysmr {

constexpr int kMaxBands = 96; // padded to a multiple of 8
constexpr int kMaxCh = 8;     // OBS can carry up to 8 channels
constexpr int kCtl = 32;      // control interval in samples
constexpr int kNeigh = 6;     // neighbours used for overlap compensation

// Silence makes the filter states decay into denormal numbers, which are very
// slow on x86. Treat them as zero while we process, then restore the CPU flags.
struct NoDenormals {
#if defined(YSMR_X86)
	unsigned int saved;
	NoDenormals() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); } // FTZ | DAZ
	~NoDenormals() { _mm_setcsr(saved); }
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
	uint64_t saved;
	NoDenormals()
	{
		__asm__ __volatile__("mrs %0, fpcr" : "=r"(saved));
		uint64_t v = saved | (1ull << 24); // FZ
		__asm__ __volatile__("msr fpcr, %0" : : "r"(v));
	}
	~NoDenormals() { __asm__ __volatile__("msr fpcr, %0" : : "r"(saved)); }
#else
	NoDenormals() {}
#endif
};

class Core {
public:
	void prepare(double sampleRate, int channels)
	{
		fs_ = sampleRate > 8000.0 ? sampleRate : 48000.0;
		nch_ = std::clamp(channels, 1, kMaxCh);

		const double fLo = 60.0;
		const double fHi = std::min(16000.0, fs_ * 0.42);
		const double bpo = 8.0; // bands per octave
		// Q so that neighbouring bands cross near -3 dB
		const double r = std::pow(2.0, 1.0 / (2.0 * bpo));
		const double Q = 1.0 / (r - 1.0 / r);

		nb_ = (int)std::floor(bpo * std::log2(fHi / fLo)) + 1;
		nb_ = std::min(nb_, kMaxBands);
		nbPad_ = (nb_ + 7) & ~7;

		const double fc = fs_ / (double)kCtl; // control rate
		for (int k = 0; k < kMaxBands; ++k) {
			a1_[k] = a2_[k] = a3_[k] = kq_[k] = 0.f;
			w_[k] = 0.f;
			atkE_[k] = relE_[k] = 1.f;
			freq_[k] = 0.f;
		}
		for (int k = 0; k < nb_; ++k) {
			const double f = fLo * std::pow(2.0, k / bpo);
			const double g = std::tan(M_PI_ * f / fs_);
			const double kk = 1.0 / Q;
			const double a1 = 1.0 / (1.0 + g * (g + kk));
			a1_[k] = (float)a1;
			a2_[k] = (float)(g * a1);
			a3_[k] = (float)(g * g * a1);
			kq_[k] = (float)kk;
			freq_[k] = (float)f;

			// envelope followers: quick up, slower down. Narrow (low) bands
			// need longer averaging or noise-like material makes them jitter.
			const double bw = f / Q;
			const double tAtk = 0.004;
			const double tRel = std::clamp(4.0 / bw, 0.05, 0.30);
			atkE_[k] = (float)(1.0 - std::exp(-1.0 / (tAtk * fc)));
			relE_[k] = (float)(1.0 - std::exp(-1.0 / (tRel * fc)));

			w_[k] = (float)weight(f);

			// tone-balance target, as band level relative to the mid range:
			// -1.5 dB/oct on top of pink (about -4.5 dB/oct overall), with
			// the very top allowed to roll off further
			double t = -1.5 * std::log2(f / 700.0);
			if (f > 10000.0)
				t -= 3.0 * std::log2(f / 10000.0);
			tgt_[k] = (float)t;
		}
		refLo_ = 0;
		refHi_ = nb_ - 1;
		for (int k = 0; k < nb_; ++k) {
			if (freq_[k] < 250.f)
				refLo_ = k + 1;
			if (freq_[k] <= 2500.f)
				refHi_ = k;
		}
		refLo_ = std::min(refLo_, refHi_);
		tbEvery_ = std::max(1, (int)std::lround(0.0427 * fc)); // ~43 ms
		const double dt = (double)tbEvery_ / fc;
		nMax_ = std::max(1, (int)std::lround(20.0 / dt));     // learn over ~20 s
		nConf_ = std::max(1, (int)std::lround(3.0 / dt));     // trust after ~3 s
		silentMax_ = std::max(1, (int)std::lround(2.0 / dt)); // pause = new song
		tbSmooth_ = (float)(1.0 - std::exp(-dt / 3.0));
		for (int n = 0; n <= kNeigh; ++n) {
			const double rr = std::pow(2.0, n / bpo);
			const double x = Q * (rr - 1.0 / rr);
			A_[n] = (float)(1.0 / (1.0 + x * x)); // real part of BP, n bands away
		}
		win_ = (int)bpo; // +-1 octave reference window
		atkG_ = (float)(1.0 - std::exp(-1.0 / (0.003 * fc)));
		relG_ = (float)(1.0 - std::exp(-1.0 / (0.080 * fc)));
		reset();
	}

	void reset()
	{
		std::memset(s1_, 0, sizeof(s1_));
		std::memset(s2_, 0, sizeof(s2_));
		std::memset(acc_, 0, sizeof(acc_));
		std::memset(env_, 0, sizeof(env_));
		std::memset(g_, 0, sizeof(g_));
		std::memset(ginc_, 0, sizeof(ginc_));
		std::memset(gs_, 0, sizeof(gs_));
		std::memset(gt_, 0, sizeof(gt_));
		std::memset(redDb_, 0, sizeof(redDb_));
		std::memset(ltAcc_, 0, sizeof(ltAcc_));
		std::memset(lt_, 0, sizeof(lt_));
		std::memset(corrNew_, 0, sizeof(corrNew_));
		std::memset(held_, 0, sizeof(held_));
		std::memset(cur_, 0, sizeof(cur_));
		std::memset(corrS_, 0, sizeof(corrS_));
		std::memset(dT_, 0, sizeof(dT_));
		std::memset(gT_, 0, sizeof(gT_));
		ctl_ = 0;
		tbCount_ = 0;
		nAct_ = 0;
		silent_ = 0;
	}

	// 0..1. 0 = bit-exact bypass, 0.5 = everyday setting, 1 = heavy.
	void setAmount(float a) { amount_ = std::clamp(a, 0.f, 1.f); }
	// 0..1. How much of the automatic tone-balance correction to apply.
	void setTone(float t) { tone_ = std::clamp(t, 0.f, 1.f); }
	// true = output only what is being removed (for checking by ear)
	void setDelta(bool d) { delta_ = d; }

	int bands() const { return nb_; }
	float bandFreq(int k) const { return freq_[k]; }
	// current reduction asked of band k, in dB (positive number)
	float bandReductionDb(int k) const { return redDb_[k]; }
	// current tone-balance correction of band k, in dB (+ = boost)
	float bandToneDb(int k) const { return corrS_[k]; }

	// planar, in place. Any block size.
	void process(float *const *ch, int n)
	{
		if (amount_ <= 0.f && tone_ <= 0.f && idle_) {
			if (delta_)
				for (int c = 0; c < nch_; ++c)
					std::memset(ch[c], 0, sizeof(float) * (size_t)n);
			return;
		}
		const NoDenormals guard;
		const int nb = nbPad_;
		float bp[kMaxBands];
		for (int i = 0; i < n; ++i) {
			for (int c = 0; c < nch_; ++c) {
				const float x = ch[c][i];
				float *s1 = s1_[c];
				float *s2 = s2_[c];
				for (int k = 0; k < nb; ++k) {
					const float v3 = x - s2[k];
					const float v1 = a1_[k] * s1[k] + a2_[k] * v3;
					const float v2 = s2[k] + a2_[k] * s1[k] + a3_[k] * v3;
					s1[k] = 2.f * v1 - s1[k];
					s2[k] = 2.f * v2 - s2[k];
					const float b = kq_[k] * v1;
					bp[k] = b;
					acc_[k] += b * b;
				}
				float lane[8] = {0, 0, 0, 0, 0, 0, 0, 0};
				for (int k = 0; k < nb; k += 8)
					for (int j = 0; j < 8; ++j)
						lane[j] += g_[k + j] * bp[k + j];
				const float sub = ((lane[0] + lane[1]) + (lane[2] + lane[3])) +
						  ((lane[4] + lane[5]) + (lane[6] + lane[7]));
				ch[c][i] = delta_ ? sub : (ch[c][i] - sub);
			}
			for (int k = 0; k < nb; ++k)
				g_[k] += ginc_[k];
			if (++ctl_ >= kCtl) {
				ctl_ = 0;
				tick();
			}
		}
	}

private:
	static constexpr double M_PI_ = 3.14159265358979323846;

	// Where the filter is allowed to work, tuned for full karaoke mixes:
	// keep hands off kick/bass fundamentals, work hardest on low-mid mud and
	// the 2-8 kHz harsh zone, ease off in the air band.
	static double weight(double f)
	{
		struct P {
			double f, w;
		};
		static const P pts[] = {{60, 0.25},  {100, 0.35}, {200, 0.9},    {400, 1.0},  {1000, 0.8},
					{2500, 1.0}, {7000, 1.0}, {11000, 0.75}, {16000, 0.5}};
		const int np = (int)(sizeof(pts) / sizeof(pts[0]));
		if (f <= pts[0].f)
			return pts[0].w;
		for (int i = 1; i < np; ++i) {
			if (f <= pts[i].f) {
				const double t = std::log(f / pts[i - 1].f) / std::log(pts[i].f / pts[i - 1].f);
				return pts[i - 1].w + t * (pts[i].w - pts[i - 1].w);
			}
		}
		return pts[np - 1].w;
	}

	void tick()
	{
		const int nb = nb_;
		float E[kMaxBands];
		float pre[kMaxBands + 1];
		const float inv = 1.f / (float)(kCtl);
		pre[0] = 0.f;
		for (int k = 0; k < nb; ++k) {
			const float p = acc_[k] * inv;
			acc_[k] = 0.f;
			ltAcc_[k] += p;
			env_[k] += (p > env_[k] ? atkE_[k] : relE_[k]) * (p - env_[k]);
			E[k] = 10.f * std::log10(env_[k] + 1e-14f);
			pre[k + 1] = pre[k] + E[k];
		}
		for (int k = nb; k < nbPad_; ++k)
			acc_[k] = 0.f;
		if (++tbCount_ >= tbEvery_) {
			tbCount_ = 0;
			toneUpdate();
		}

		const float depth = amount_ * 2.f; // 0..2
		const float thr = 2.5f;            // dB a band must stick out before we act
		const float exMax = 12.0f;         // dB of excess that counts
		const float floorDb = -78.0f;      // ignore bands this quiet
		float d[kMaxBands];
		bool any = false;
		for (int k = 0; k < nb; ++k) {
			const int lo = std::max(0, k - win_);
			const int hi = std::min(nb - 1, k + win_);
			// local average WITHOUT the band itself and its two direct
			// neighbours, so a peak does not raise its own reference
			const int clo = std::max(0, k - 1);
			const int chi = std::min(nb - 1, k + 1);
			const float ref = ((pre[hi + 1] - pre[lo]) - (pre[chi + 1] - pre[clo])) /
					  (float)((hi - lo + 1) - (chi - clo + 1));
			float ex = E[k] - ref - thr;
			ex = std::clamp(ex, 0.f, exMax);
			float red = 0.5f * depth * ex * w_[k];
			if (E[k] < floorDb)
				red = 0.f;
			redDb_[k] = red;
			d[k] = red > 0.f ? 1.f - std::pow(10.f, -red * 0.05f) : 0.f;
		}
		// Neighbouring notches overlap; solve for per-band gains so the depth
		// at each band centre is what was asked for (one Gauss-Seidel sweep
		// per tick, warm-started from the previous tick).
		for (int k = 0; k < nb; ++k) {
			float s = 0.f;
			for (int n = 1; n <= kNeigh; ++n) {
				if (k - n >= 0)
					s += A_[n] * gt_[k - n];
				if (k + n < nb)
					s += A_[n] * gt_[k + n];
			}
			gt_[k] = std::clamp(d[k] - s, 0.f, d[k]);
		}
		for (int k = 0; k < nb; ++k) {
			const float c = gt_[k] > gs_[k] ? atkG_ : relG_;
			gs_[k] += c * (gt_[k] - gs_[k]);
			if (gs_[k] < 1e-6f)
				gs_[k] = 0.f;
			// tone part plus resonance part; the resonance cut is scaled so the
			// two behave like filters in series: (1 - T)(1 - R)
			const float target = gT_[k] + gs_[k] * (1.f - dT_[k]);
			ginc_[k] = (target - g_[k]) * inv;
			if (gs_[k] > 0.f || std::fabs(g_[k]) > 1e-6f || gT_[k] != 0.f)
				any = true;
		}
		const bool nowIdle = !any && amount_ <= 0.f && tone_ <= 0.f;
		if (nowIdle && !idle_) {
			// going to sleep: clear the filters so waking up later starts clean
			std::memset(s1_, 0, sizeof(s1_));
			std::memset(s2_, 0, sizeof(s2_));
			std::memset(env_, 0, sizeof(env_));
			std::memset(g_, 0, sizeof(g_));
			std::memset(ginc_, 0, sizeof(ginc_));
		}
		idle_ = nowIdle;
	}

	// Runs about every 43 ms. Learns the long-term level of every band,
	// compares it with the target slope and moves the correction slowly.
	void toneUpdate()
	{
		const int nb = nb_;
		float blk[kMaxBands];
		float tot = 0.f;
		const float invN = 1.f / (float)tbEvery_;
		for (int k = 0; k < nb; ++k) {
			blk[k] = ltAcc_[k] * invN;
			ltAcc_[k] = 0.f;
			tot += blk[k];
		}
		if (tot > 1e-6f) { // roughly -60 dBFS: something is playing
			if (silent_ >= silentMax_) {
				// first sound after a real pause: treat it as a new song,
				// keep the previous correction until the new one is trusted
				std::memcpy(held_, cur_, sizeof(held_));
				nAct_ = 0;
			}
			silent_ = 0;
			nAct_ = std::min(nAct_ + 1, nMax_);
			const float a = 1.f / (float)nAct_;
			float L[kMaxBands], tmp[kMaxBands], raw[kMaxBands];
			for (int k = 0; k < nb; ++k) {
				lt_[k] += a * (blk[k] - lt_[k]);
				L[k] = 10.f * std::log10(lt_[k] + 1e-14f);
			}
			// reference = average power of the mid range (250 Hz - 2.5 kHz).
			// A power average follows the bands that actually carry sound,
			// so a sparse arrangement cannot drag the reference into the gaps.
			float msum = 0.f;
			for (int k = refLo_; k <= refHi_; ++k)
				msum += lt_[k];
			const float ref = 10.f * std::log10(msum / (float)(refHi_ - refLo_ + 1) + 1e-14f);
			// A band far below the target is an instrument that is not playing
			// (or a hard roll-off), not a tone problem: do not boost it.
			float fill[kMaxBands];
			int present = 0, counted = 0;
			for (int k = 0; k < nb; ++k) {
				const float r = ref + tgt_[k] - L[k];
				fill[k] = std::clamp((24.f - r) * 0.1f, 0.f, 1.f);
				raw[k] = std::clamp(r, -12.f, 12.f);
				if (freq_[k] >= 100.f && freq_[k] <= 8000.f) {
					++counted;
					if (r < 12.f)
						++present;
				}
			}
			// A sparse arrangement (solo intro) says little about the tone of
			// the track: only act once most of the spectrum is in use.
			const float fullness = counted > 0 ? (float)present / (float)counted : 0.f;
			const float strength = std::clamp((fullness - 0.5f) * (1.f / 0.3f), 0.f, 1.f);
			// smooth across frequency (two passes of +-3 bands, about an octave)
			for (int pass = 0; pass < 2; ++pass) {
				for (int k = 0; k < nb; ++k) {
					float sum = 0.f, sumF = 0.f;
					for (int j = -3; j <= 3; ++j) {
						const int q = std::clamp(k + j, 0, nb - 1);
						sum += raw[q];
						sumF += fill[q];
					}
					tmp[k] = sum * (1.f / 7.f);
					L[k] = sumF * (1.f / 7.f); // L is free to reuse from here on
				}
				std::memcpy(raw, tmp, sizeof(float) * (size_t)nb);
				std::memcpy(fill, L, sizeof(float) * (size_t)nb);
			}
			float mean = 0.f;
			int cnt = 0;
			for (int k = 0; k < nb; ++k) {
				float c = std::clamp(raw[k], -5.f, 5.f);
				if (c > 0.f)
					c *= fill[k];
				raw[k] = c;
				if (freq_[k] >= 200.f && freq_[k] <= 5000.f) {
					mean += c;
					++cnt;
				}
			}
			// keep it a tone control, not a volume control
			mean = cnt > 0 ? mean / (float)cnt : 0.f;
			for (int k = 0; k < nb; ++k) {
				float c = std::clamp(raw[k] - mean, -5.f, 5.f);
				if (freq_[k] < 70.f)
					c = std::min(c, 0.f); // never boost sub-bass
				corrNew_[k] = c * strength;
			}
		} else if (silent_ < silentMax_) {
			++silent_;
		}

		const float conf = std::min(1.f, (float)nAct_ / (float)nConf_);
		bool anyTone = false;
		for (int k = 0; k < nb; ++k) {
			cur_[k] = conf * corrNew_[k] + (1.f - conf) * held_[k];
			corrS_[k] += tbSmooth_ * (tone_ * cur_[k] - corrS_[k]);
			if (tone_ <= 0.f && std::fabs(corrS_[k]) < 1e-3f)
				corrS_[k] = 0.f;
			dT_[k] = corrS_[k] != 0.f ? 1.f - std::pow(10.f, corrS_[k] * 0.05f) : 0.f;
			if (dT_[k] != 0.f)
				anyTone = true;
		}
		if (!anyTone) {
			std::memset(gT_, 0, sizeof(gT_));
			return;
		}
		// per-band gains that give the wanted broad curve at the band centres
		for (int sweep = 0; sweep < 4; ++sweep) {
			for (int k = 0; k < nb; ++k) {
				float s = 0.f;
				for (int n = 1; n <= kNeigh; ++n) {
					if (k - n >= 0)
						s += A_[n] * gT_[k - n];
					if (k + n < nb)
						s += A_[n] * gT_[k + n];
				}
				gT_[k] = dT_[k] - s;
			}
		}
	}

	double fs_ = 48000.0;
	int nch_ = 2;
	int nb_ = 0, nbPad_ = 0, win_ = 8;
	int ctl_ = 0;
	float amount_ = 0.5f;
	float tone_ = 0.f;
	bool delta_ = false;
	bool idle_ = true;

	alignas(32) float a1_[kMaxBands], a2_[kMaxBands], a3_[kMaxBands], kq_[kMaxBands];
	alignas(32) float s1_[kMaxCh][kMaxBands], s2_[kMaxCh][kMaxBands];
	alignas(32) float acc_[kMaxBands], env_[kMaxBands];
	alignas(32) float g_[kMaxBands], ginc_[kMaxBands], gs_[kMaxBands], gt_[kMaxBands];
	float w_[kMaxBands], atkE_[kMaxBands], relE_[kMaxBands], freq_[kMaxBands], redDb_[kMaxBands];
	float A_[kNeigh + 1];
	float atkG_ = 1.f, relG_ = 1.f;

	// tone balance
	float tgt_[kMaxBands], ltAcc_[kMaxBands], lt_[kMaxBands];
	float corrNew_[kMaxBands], held_[kMaxBands], cur_[kMaxBands], corrS_[kMaxBands];
	float dT_[kMaxBands], gT_[kMaxBands];
	int refLo_ = 0, refHi_ = 0;
	int tbEvery_ = 64, tbCount_ = 0;
	int nAct_ = 0, nMax_ = 470, nConf_ = 70;
	int silent_ = 0, silentMax_ = 47;
	float tbSmooth_ = 0.03f;
};

} // namespace ysmr
