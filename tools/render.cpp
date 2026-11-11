// SPDX-License-Identifier: GPL-2.0-or-later
// Offline renderer / test bench for the Yunseul MR core.
//   render in.wav out.wav [--amount 0.5] [--delta] [--block 480] [--stats]
#include "../src/ys_mr_core.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

struct Wav {
	int sr = 0, ch = 0;
	std::vector<std::vector<float>> d; // planar
};

static uint32_t rd32(const uint8_t *p)
{
	return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static bool readWav(const char *path, Wav &w)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	std::vector<uint8_t> b;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	b.resize((size_t)sz);
	if (fread(b.data(), 1, (size_t)sz, f) != (size_t)sz) {
		fclose(f);
		return false;
	}
	fclose(f);
	if (sz < 12 || memcmp(b.data(), "RIFF", 4) || memcmp(b.data() + 8, "WAVE", 4))
		return false;
	size_t p = 12;
	int fmt = 0, bits = 0;
	const uint8_t *data = nullptr;
	size_t dlen = 0;
	while (p + 8 <= b.size()) {
		uint32_t len = rd32(&b[p + 4]);
		if (!memcmp(&b[p], "fmt ", 4)) {
			fmt = rd16(&b[p + 8]);
			w.ch = rd16(&b[p + 10]);
			w.sr = (int)rd32(&b[p + 12]);
			bits = rd16(&b[p + 22]);
			if (fmt == 0xFFFE && len >= 26)
				fmt = rd16(&b[p + 32]);
		} else if (!memcmp(&b[p], "data", 4)) {
			data = &b[p + 8];
			dlen = std::min((size_t)len, b.size() - p - 8);
			break;
		}
		p += 8 + len + (len & 1);
	}
	if (!data || w.ch < 1 || w.ch > ysmr::kMaxCh)
		return false;
	const int bps = bits / 8;
	const size_t n = dlen / (size_t)(bps * w.ch);
	w.d.assign((size_t)w.ch, std::vector<float>(n));
	for (size_t i = 0; i < n; ++i)
		for (int c = 0; c < w.ch; ++c) {
			const uint8_t *s = data + (i * (size_t)w.ch + (size_t)c) * (size_t)bps;
			float v = 0;
			if (fmt == 3 && bits == 32)
				memcpy(&v, s, 4);
			else if (fmt == 1 && bits == 16)
				v = (int16_t)rd16(s) / 32768.f;
			else if (fmt == 1 && bits == 24)
				v = (float)((int32_t)((s[0] << 8) | (s[1] << 16) | ((uint32_t)s[2] << 24)) >> 8) /
				    8388608.f;
			else if (fmt == 1 && bits == 32)
				v = (float)((int32_t)rd32(s) / 2147483648.0);
			else
				return false;
			w.d[(size_t)c][i] = v;
		}
	return true;
}

static bool writeWavF32(const char *path, const Wav &w)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	const uint32_t n = (uint32_t)w.d[0].size();
	const uint32_t dlen = n * (uint32_t)w.ch * 4;
	uint8_t h[44];
	memcpy(h, "RIFF", 4);
	uint32_t v = 36 + dlen;
	memcpy(h + 4, &v, 4);
	memcpy(h + 8, "WAVEfmt ", 8);
	v = 16;
	memcpy(h + 16, &v, 4);
	uint16_t s = 3;
	memcpy(h + 20, &s, 2);
	s = (uint16_t)w.ch;
	memcpy(h + 22, &s, 2);
	v = (uint32_t)w.sr;
	memcpy(h + 24, &v, 4);
	v = (uint32_t)(w.sr * w.ch * 4);
	memcpy(h + 28, &v, 4);
	s = (uint16_t)(w.ch * 4);
	memcpy(h + 32, &s, 2);
	s = 32;
	memcpy(h + 34, &s, 2);
	memcpy(h + 36, "data", 4);
	memcpy(h + 40, &dlen, 4);
	fwrite(h, 1, 44, f);
	std::vector<float> il((size_t)n * (size_t)w.ch);
	for (uint32_t i = 0; i < n; ++i)
		for (int c = 0; c < w.ch; ++c)
			il[(size_t)i * (size_t)w.ch + (size_t)c] = w.d[(size_t)c][i];
	fwrite(il.data(), 4, il.size(), f);
	fclose(f);
	return true;
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: render in.wav out.wav [--amount a] [--delta] [--block n] [--stats]\n");
		return 2;
	}
	float amount = 0.5f;
	bool delta = false, stats = false;
	int block = 480;
	for (int i = 3; i < argc; ++i) {
		std::string a = argv[i];
		if (a == "--amount" && i + 1 < argc)
			amount = (float)atof(argv[++i]);
		else if (a == "--delta")
			delta = true;
		else if (a == "--stats")
			stats = true;
		else if (a == "--block" && i + 1 < argc)
			block = atoi(argv[++i]);
	}
	Wav w;
	if (!readWav(argv[1], w)) {
		fprintf(stderr, "cannot read %s\n", argv[1]);
		return 1;
	}
	static ysmr::Core core;
	core.prepare(w.sr, w.ch);
	core.setAmount(amount);
	core.setDelta(delta);

	const int nb = core.bands();
	std::vector<double> sumRed((size_t)nb, 0.0), maxRed((size_t)nb, 0.0);
	long ticks = 0;
	const int n = (int)w.d[0].size();
	float *ptr[ysmr::kMaxCh];
	auto t0 = std::chrono::steady_clock::now();
	for (int pos = 0; pos < n; pos += block) {
		const int m = std::min(block, n - pos);
		for (int c = 0; c < w.ch; ++c)
			ptr[c] = w.d[(size_t)c].data() + pos;
		core.process(ptr, m);
		if (stats) {
			for (int k = 0; k < nb; ++k) {
				const double r = core.bandReductionDb(k);
				sumRed[(size_t)k] += r;
				maxRed[(size_t)k] = std::max(maxRed[(size_t)k], r);
			}
			++ticks;
		}
	}
	auto t1 = std::chrono::steady_clock::now();
	const double sec = std::chrono::duration<double>(t1 - t0).count();
	const double dur = (double)n / w.sr;
	fprintf(stderr, "bands %d  audio %.2fs  cpu %.3fs  load %.2f%% of one core\n", nb, dur, sec, 100.0 * sec / dur);
	if (stats) {
		for (int k = 0; k < nb; ++k)
			printf("%.1f %.3f %.3f\n", core.bandFreq(k), sumRed[(size_t)k] / (double)std::max(1L, ticks),
			       maxRed[(size_t)k]);
	}
	if (!writeWavF32(argv[2], w)) {
		fprintf(stderr, "cannot write %s\n", argv[2]);
		return 1;
	}
	return 0;
}
