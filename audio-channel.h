#pragma once

#include <media-io/audio-resampler.h>
#include <util/circlebuf.h>
#include <pthread.h>

struct audio_channel {
	bool audio_pending;
	bool pending_stop;
	uint64_t audio_ts;
	struct circlebuf audio_input_buf[MAX_AUDIO_CHANNELS];
	size_t last_audio_input_buf_size;

	volatile uint64_t timing_adjust;
	uint64_t resample_offset;
	uint64_t last_audio_ts;
	uint64_t next_audio_ts_min;
	uint64_t next_audio_sys_ts_min;
	uint64_t last_frame_ts;
	uint64_t last_sys_timestamp;

	float *audio_output_buf[MAX_AUDIO_CHANNELS];

	struct resample_info in_sample_info;
	struct resample_info out_sample_info;
	audio_resampler_t *resampler;

	pthread_mutex_t audio_buf_mutex;
};

inline size_t convert_time_to_frames(size_t sample_rate, uint64_t t)
{
	return (size_t)(t * (uint64_t)sample_rate / 1000000000ULL);
}

struct audio_channel *audio_channel_create(struct resample_info *info);
void audio_channel_destroy(struct audio_channel *source);
void audio_channel_output_audio(struct audio_channel *c, struct obs_source_audio *audio);
void audio_channel_pick_audio_data(struct audio_channel *source, size_t size, size_t channels);
bool audio_channel_audio_buffer_insuffient(struct audio_channel *source, size_t sample_rate, uint64_t min_ts);
