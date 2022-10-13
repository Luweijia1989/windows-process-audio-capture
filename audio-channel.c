#include "audio-channel.h"
#include <inttypes.h>
#include <obs.h>
#include <util/platform.h>
#include <util/threading.h>

/* maximum timestamp variance in nanoseconds */
#define MAX_TS_VAR 2000000000ULL

/* maximum buffer size */
#define MAX_BUF_SIZE (1000 * AUDIO_OUTPUT_FRAMES * sizeof(float))

/* time threshold in nanoseconds to ensure audio timing is as seamless as
 * possible */
#define TS_SMOOTHING_THRESHOLD 70000000ULL

static inline uint64_t uint64_diff(uint64_t ts1, uint64_t ts2)
{
	return (ts1 < ts2) ? (ts2 - ts1) : (ts1 - ts2);
}

static inline size_t get_buf_placement(uint32_t sample_rate, uint64_t offset)
{
	return (size_t)(offset * (uint64_t)sample_rate / 1000000000ULL);
}

/* unless the value is 3+ hours worth of frames, this won't overflow */
static inline uint64_t conv_frames_to_time(const size_t sample_rate, const size_t frames)
{
	if (!sample_rate)
		return 0;

	return (uint64_t)frames * 1000000000ULL / (uint64_t)sample_rate;
}

static inline void reset_audio_timing(struct audio_channel *source, uint64_t timestamp, uint64_t os_time)
{
	source->timing_adjust = os_time - timestamp;
}

static void reset_audio_data(struct audio_channel *source, uint64_t os_time)
{
	blog(LOG_DEBUG, "audio channel:0x%p reset_audio_data, time: %lld", source, os_time);
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		if (source->audio_input_buf[i].size)
			circlebuf_pop_front(&source->audio_input_buf[i], NULL, source->audio_input_buf[i].size);
	}

	source->last_audio_input_buf_size = 0;
	source->audio_ts = os_time;
	source->next_audio_sys_ts_min = os_time;
}

static void handle_ts_jump(struct audio_channel *source, uint64_t expected, uint64_t ts, uint64_t diff, uint64_t os_time)
{
	blog(LOG_DEBUG,
	     "Timestamp for channel '0x%p' jumped by '%" PRIu64 "', "
	     "expected value %" PRIu64 ", input value %" PRIu64,
	     source, diff, expected, ts);

	pthread_mutex_lock(&source->audio_buf_mutex);
	reset_audio_timing(source, ts, os_time);
	pthread_mutex_unlock(&source->audio_buf_mutex);
}

static void audio_channel_output_audio_place(struct audio_channel *source, const struct audio_data *in)
{
	size_t buf_placement;
	size_t channels = get_audio_channels(source->out_sample_info.speakers);
	size_t size = in->frames * sizeof(float);

	if (!source->audio_ts || in->timestamp < source->audio_ts)
		reset_audio_data(source, in->timestamp);

	buf_placement = get_buf_placement(source->out_sample_info.samples_per_sec, in->timestamp - source->audio_ts) * sizeof(float);

#if DEBUG_AUDIO == 1
	blog(LOG_DEBUG, "frames: %lu, size: %lu, placement: %lu, base_ts: %llu, ts: %llu", (unsigned long)in->frames,
	     (unsigned long)source->audio_input_buf[0].size, (unsigned long)buf_placement, source->audio_ts, in->timestamp);
#endif

	/* do not allow the circular buffers to become too big */
	if ((buf_placement + size) > MAX_BUF_SIZE)
		return;

	for (size_t i = 0; i < channels; i++) {
		circlebuf_place(&source->audio_input_buf[i], buf_placement, in->data[i], size);
		circlebuf_pop_back(&source->audio_input_buf[i], NULL, source->audio_input_buf[i].size - (buf_placement + size));
	}

	source->last_audio_input_buf_size = 0;
}

static inline void audio_channel_output_audio_push_back(struct audio_channel *source, const struct audio_data *in)
{
	size_t channels = get_audio_channels(source->out_sample_info.speakers);
	size_t size = in->frames * sizeof(float);

	/* do not allow the circular buffers to become too big */
	if ((source->audio_input_buf[0].size + size) > MAX_BUF_SIZE)
		return;

	for (size_t i = 0; i < channels; i++)
		circlebuf_push_back(&source->audio_input_buf[i], in->data[i], size);

	/* reset audio input buffer size to ensure that audio doesn't get
	 * perpetually cut */
	source->last_audio_input_buf_size = 0;
}

static void audio_channel_output_audio_internal(struct audio_channel *source, const struct audio_data *data)
{
	size_t sample_rate = source->out_sample_info.samples_per_sec;
	struct audio_data in = *data;
	uint64_t diff;
	uint64_t os_time = os_gettime_ns();
	bool using_direct_ts = false;
	bool push_back = false;

	/* detects 'directly' set timestamps as long as they're within
	 * a certain threshold */
	if (uint64_diff(in.timestamp, os_time) < MAX_TS_VAR) {
		source->timing_adjust = 0;
		using_direct_ts = true;
	}

	if (source->next_audio_ts_min != 0) {
		diff = uint64_diff(source->next_audio_ts_min, in.timestamp);

		/* smooth audio if within threshold */
		if (diff > MAX_TS_VAR && !using_direct_ts)
			handle_ts_jump(source, source->next_audio_ts_min, in.timestamp, diff, os_time);
		else if (diff < TS_SMOOTHING_THRESHOLD) {
			in.timestamp = source->next_audio_ts_min;
		}
	}

	source->last_audio_ts = in.timestamp;
	source->next_audio_ts_min = in.timestamp + conv_frames_to_time(sample_rate, in.frames);

	in.timestamp += source->timing_adjust;

	pthread_mutex_lock(&source->audio_buf_mutex);

	if (source->next_audio_sys_ts_min == in.timestamp) {
		push_back = true;

	} else if (source->next_audio_sys_ts_min) {
		diff = uint64_diff(source->next_audio_sys_ts_min, in.timestamp);

		if (diff < TS_SMOOTHING_THRESHOLD) {
			push_back = true;

			/* This typically only happens if used with async video when
		 * audio/video start transitioning in to a timestamp jump.
		 * Audio will typically have a timestamp jump, and then video
		 * will have a timestamp jump.  If that case is encountered,
		 * just clear the audio data in that small window and force a
		 * resync.  This handles all cases rather than just looping. */
		} else if (diff > MAX_TS_VAR) {
			reset_audio_timing(source, data->timestamp, os_time);
			in.timestamp = data->timestamp + source->timing_adjust;
		}
	}

	//add extra 100ms to avoid loss audio data when capture jitter
	in.timestamp += 100000000;
	in.timestamp -= source->resample_offset;

	source->next_audio_sys_ts_min = source->next_audio_ts_min + source->timing_adjust;

	if (push_back && source->audio_ts)
		audio_channel_output_audio_push_back(source, &in);
	else
		audio_channel_output_audio_place(source, &in);

	pthread_mutex_unlock(&source->audio_buf_mutex);
}

void audio_channel_output_audio(struct audio_channel *c, struct obs_source_audio *audio)
{
	if (c->in_sample_info.samples_per_sec != audio->samples_per_sec || c->in_sample_info.format != audio->format ||
	    c->in_sample_info.speakers != audio->speakers) {
		c->in_sample_info.format = audio->format;
		c->in_sample_info.samples_per_sec = audio->samples_per_sec;
		c->in_sample_info.speakers = audio->speakers;

		audio_resampler_destroy(c->resampler);
		c->resampler = NULL;
		c->resample_offset = 0;

		c->resampler = audio_resampler_create(&c->out_sample_info, &c->in_sample_info);
		blog(LOG_ERROR, "create audio channel resampler: %p", c);
	}

	struct audio_data out_audio;
	memset(&out_audio, 0, sizeof(struct obs_audio_data));
	out_audio.timestamp = audio->timestamp;

	if (c->resampler) {
		uint8_t *output[MAX_AV_PLANES];
		memset(output, 0, sizeof(output));

		uint32_t frames = 0;
		audio_resampler_resample(c->resampler, output, &frames, &c->resample_offset, audio->data, audio->frames);

		out_audio.frames = frames;
		for (size_t i = 0; i < MAX_AV_PLANES; i++) {
			out_audio.data[i] = output[i];
		}
	} else {
		out_audio.frames = audio->frames;
		for (size_t i = 0; i < MAX_AV_PLANES; i++) {
			out_audio.data[i] = (uint8_t *)audio->data[i];
		}
	}

	audio_channel_output_audio_internal(c, &out_audio);
}

void audio_channel_pick_audio_data(struct audio_channel *source, size_t size, size_t channels)
{
	pthread_mutex_lock(&source->audio_buf_mutex);

	if (source->audio_input_buf[0].size < size) {
#if DEBUG_AUDIO == 1
		blog(LOG_DEBUG, "audio_channel_pick_audio_data: data not enough, current: %d, expected: %d", source->audio_input_buf[0].size, size);
#endif
		source->audio_pending = true;
		pthread_mutex_unlock(&source->audio_buf_mutex);
		return;
	}

	for (size_t ch = 0; ch < channels; ch++)
		circlebuf_peek_front(&source->audio_input_buf[ch], source->audio_output_buf[ch], size);

	pthread_mutex_unlock(&source->audio_buf_mutex);
	source->audio_pending = false;
}

bool audio_channel_audio_buffer_insuffient(struct audio_channel *source, size_t sample_rate, uint64_t min_ts)
{
	size_t total_floats = AUDIO_OUTPUT_FRAMES;
	size_t size;

	if (source->audio_pending || !source->audio_ts) {
		return false;
	}

	if (source->audio_ts != min_ts && source->audio_ts != (min_ts - 1)) {
		size_t start_point = convert_time_to_frames(sample_rate, source->audio_ts - min_ts);
		if (start_point >= AUDIO_OUTPUT_FRAMES)
			return false;

		total_floats -= start_point;
	}

	size = total_floats * sizeof(float);

	if (source->audio_input_buf[0].size < size) {
		source->audio_pending = true;
		return true;
	}

	return false;
}

struct audio_channel *audio_channel_create(struct resample_info *info)
{
	struct audio_channel *channel = bzalloc(sizeof(*channel));
	memcpy(&channel->out_sample_info, info, sizeof(struct resample_info));
	pthread_mutex_init_value(&channel->audio_buf_mutex);
	pthread_mutex_init(&channel->audio_buf_mutex, NULL);

	size_t size = sizeof(float) * AUDIO_OUTPUT_FRAMES * MAX_AUDIO_CHANNELS;
	float *ptr = bzalloc(size);

	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		channel->audio_output_buf[i] = ptr + sizeof(float) * AUDIO_OUTPUT_FRAMES * i;
	}

	return channel;
}

void audio_channel_destroy(struct audio_channel *source)
{
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++)
		circlebuf_free(&source->audio_input_buf[i]);

	audio_resampler_destroy(source->resampler);
	bfree(source->audio_output_buf[0]);
	pthread_mutex_destroy(&source->audio_buf_mutex);
	bfree(source);
}
