/*
Yunseul MR - dynamic resonance smoother for karaoke backing tracks
Copyright (C) 2026 Yunseul (yunseulmp3)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

#include <atomic>
#include <new>

#include "ys_mr_core.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Yunseul MR - automatic clean-up filter for karaoke backing tracks";
}

#define S_AMOUNT "amount"
#define S_TONE "tone"
#define S_DELTA "delta"

namespace {

struct MrFilter {
	obs_source_t *context = nullptr;
	ysmr::Core core;
	std::atomic<float> amount{0.5f};
	std::atomic<float> tone{1.0f};
	std::atomic<bool> delta{false};
	uint32_t sampleRate = 48000;
	int channels = 0; // what the core is currently prepared for
};

const char *mr_name(void *)
{
	return obs_module_text("YunseulMR.Name");
}

void mr_update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<MrFilter *>(data);
	f->amount.store((float)obs_data_get_double(settings, S_AMOUNT) / 100.0f, std::memory_order_relaxed);
	f->tone.store((float)obs_data_get_double(settings, S_TONE) / 100.0f, std::memory_order_relaxed);
	f->delta.store(obs_data_get_bool(settings, S_DELTA), std::memory_order_relaxed);
}

void *mr_create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new (std::nothrow) MrFilter();
	if (!f)
		return nullptr;
	f->context = source;

	struct obs_audio_info oai = {};
	if (obs_get_audio_info(&oai) && oai.samples_per_sec > 0)
		f->sampleRate = oai.samples_per_sec;

	mr_update(f, settings);
	return f;
}

void mr_destroy(void *data)
{
	delete static_cast<MrFilter *>(data);
}

struct obs_audio_data *mr_filter_audio(void *data, struct obs_audio_data *audio)
{
	auto *f = static_cast<MrFilter *>(data);

	float *planes[ysmr::kMaxCh];
	int n = 0;
	for (int c = 0; c < MAX_AV_PLANES && n < ysmr::kMaxCh; ++c) {
		if (audio->data[c])
			planes[n++] = reinterpret_cast<float *>(audio->data[c]);
	}
	if (n == 0 || audio->frames == 0)
		return audio;

	if (n != f->channels) {
		// first block, or the channel layout changed: no allocation happens here
		f->core.prepare((double)f->sampleRate, n);
		f->channels = n;
	}

	f->core.setAmount(f->amount.load(std::memory_order_relaxed));
	f->core.setTone(f->tone.load(std::memory_order_relaxed));
	f->core.setDelta(f->delta.load(std::memory_order_relaxed));
	f->core.process(planes, (int)audio->frames);
	return audio;
}

void mr_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, S_AMOUNT, 50.0);
	obs_data_set_default_double(settings, S_TONE, 100.0);
	obs_data_set_default_bool(settings, S_DELTA, false);
}

obs_properties_t *mr_properties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p =
		obs_properties_add_float_slider(props, S_AMOUNT, obs_module_text("YunseulMR.Amount"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	p = obs_properties_add_float_slider(props, S_TONE, obs_module_text("YunseulMR.Tone"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(p, " %");
	obs_properties_add_bool(props, S_DELTA, obs_module_text("YunseulMR.Delta"));
	return props;
}

} // namespace

bool obs_module_load(void)
{
	static struct obs_source_info info = {};
	info.id = "yunseul_mr_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_AUDIO;
	info.get_name = mr_name;
	info.create = mr_create;
	info.destroy = mr_destroy;
	info.update = mr_update;
	info.filter_audio = mr_filter_audio;
	info.get_defaults = mr_defaults;
	info.get_properties = mr_properties;
	obs_register_source(&info);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
