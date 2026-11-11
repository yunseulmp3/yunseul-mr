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
		}
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
		ctl_ = 0;
	}

	// 0..1. 0 = bit-exact bypass, 0.5 = everyday setting, 1 = heavy.
	void setAmount(float a) { amount_ = std::clamp(a, 0.f, 1.f); }
	// true = output only what is being removed (for checking by ear)
	void setDelta(bool d) { delta_ = d; }

	int bands() const { return nb_; }
	float bandFreq(int k) const { return freq_[k]; }
	// current reduction asked of band k, in dB (positive number)
	float bandReductionDb(int k) const { return redDb_[k]; }

	// planar, in place. Any block size.
	void process(float *const *ch, int n)
	{
		if (amount_ <= 0.f && idle_) {
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
			env_[k] += (p > env_[k] ? atkE_[k] : relE_[k]) * (p - env_[k]);
			E[k] = 10.f * std::log10(env_[k] + 1e-14f);
			pre[k + 1] = pre[k] + E[k];
		}
		for (int k = nb; k < nbPad_; ++k)
			acc_[k] = 0.f;

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
			ginc_[k] = (gs_[k] - g_[k]) * inv;
			if (gs_[k] > 0.f || g_[k] > 1e-6f)
				any = true;
		}
		const bool nowIdle = !any && amount_ <= 0.f;
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

	double fs_ = 48000.0;
	int nch_ = 2;
	int nb_ = 0, nbPad_ = 0, win_ = 8;
	int ctl_ = 0;
	float amount_ = 0.5f;
	bool delta_ = false;
	bool idle_ = true;

	alignas(32) float a1_[kMaxBands], a2_[kMaxBands], a3_[kMaxBands], kq_[kMaxBands];
	alignas(32) float s1_[kMaxCh][kMaxBands], s2_[kMaxCh][kMaxBands];
	alignas(32) float acc_[kMaxBands], env_[kMaxBands];
	alignas(32) float g_[kMaxBands], ginc_[kMaxBands], gs_[kMaxBands], gt_[kMaxBands];
	float w_[kMaxBands], atkE_[kMaxBands], relE_[kMaxBands], freq_[kMaxBands], redDb_[kMaxBands];
	float A_[kNeigh + 1];
	float atkG_ = 1.f, relG_ = 1.f;
};

} // namespace ysmr
