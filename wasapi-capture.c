#include <windows.h>
#include <psapi.h>
#include <obs-module.h>
#include "app-helpers.h"
#include "../../libobs/util/windows/obfuscate.h"
#include "wasapi-capture.h"

#define SETTING_CAPTURE_PROCESS "process"

#define DEFAULT_RETRY_INTERVAL 2.0f
#define ERROR_RETRY_INTERVAL 4.0f

#define DEBUG_AUDIO 0
#define MAX_BUFFERING_TICKS 45

struct wasapi_offset offsets32 = {0};
struct wasapi_offset offsets64 = {0};

#define STOP_BEING_BAD                                                      \
	"  This is most likely due to security software. Please make sure " \
	"that the OBS installation folder is excluded/ignored in the "      \
	"settings of the security software you are using."

static bool check_file_integrity(struct wasapi_capture *wc, const char *file, const char *name)
{
	DWORD error;
	HANDLE handle;
	wchar_t *w_file = NULL;

	if (!file || !*file) {
		warn("Wasapi capture %s not found." STOP_BEING_BAD, name);
		return false;
	}

	if (!os_utf8_to_wcs_ptr(file, 0, &w_file)) {
		warn("Could not convert file name to wide string");
		return false;
	}

	handle = CreateFileW(w_file, GENERIC_READ | GENERIC_EXECUTE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);

	bfree(w_file);

	if (handle != INVALID_HANDLE_VALUE) {
		CloseHandle(handle);
		return true;
	}

	error = GetLastError();
	if (error == ERROR_FILE_NOT_FOUND) {
		warn("wasapi capture file '%s' not found." STOP_BEING_BAD, file);
	} else if (error == ERROR_ACCESS_DENIED) {
		warn("wasapi capture file '%s' could not be loaded." STOP_BEING_BAD, file);
	} else {
		warn("wasapi capture file '%s' could not be loaded: %lu." STOP_BEING_BAD, file, error);
	}

	return false;
}

static inline void find_min_ts(struct wasapi_capture *wc, uint64_t *min_ts)
{
	for (size_t i = 0; i < wc->audio_channels.num; i++) {
		struct audio_channel *source = wc->audio_channels.array[i].channel;
		if (!source->audio_pending && source->audio_ts && source->audio_ts < *min_ts) {
			*min_ts = source->audio_ts;
		}
	}
}

static inline bool mark_invalid_sources(struct wasapi_capture *wc, size_t sample_rate, uint64_t min_ts)
{
	bool recalculate = false;

	for (size_t i = 0; i < wc->audio_channels.num; i++) {
		struct audio_channel *source = wc->audio_channels.array[i].channel;
		recalculate |= audio_channel_audio_buffer_insuffient(source, sample_rate, min_ts);
	}

	return recalculate;
}

static inline void calc_min_ts(struct wasapi_capture *wc, size_t sample_rate, uint64_t *min_ts)
{
	find_min_ts(wc, min_ts);
	if (mark_invalid_sources(wc, sample_rate, *min_ts))
		find_min_ts(wc, min_ts);
}

static void wasapi_capture_add_audio_buffering(struct wasapi_capture *wc, size_t sample_rate, struct ts_info *ts, uint64_t min_ts)
{
	struct ts_info new_ts;
	uint64_t offset;
	uint64_t frames;
	size_t total_ms;
	size_t ms;
	int ticks;

	if (wc->total_buffering_ticks == MAX_BUFFERING_TICKS)
		return;

	if (!wc->buffering_wait_ticks)
		wc->buffered_ts = ts->start;

	offset = ts->start - min_ts;
	frames = ns_to_audio_frames(sample_rate, offset);
	ticks = (int)((frames + AUDIO_OUTPUT_FRAMES - 1) / AUDIO_OUTPUT_FRAMES);

	wc->total_buffering_ticks += ticks;

	if (wc->total_buffering_ticks >= MAX_BUFFERING_TICKS) {
		ticks -= wc->total_buffering_ticks - MAX_BUFFERING_TICKS;
		wc->total_buffering_ticks = MAX_BUFFERING_TICKS;
		blog(LOG_WARNING, "Max audio buffering reached!");
	}

	ms = ticks * AUDIO_OUTPUT_FRAMES * 1000 / sample_rate;
	total_ms = wc->total_buffering_ticks * AUDIO_OUTPUT_FRAMES * 1000 / sample_rate;

	blog(LOG_INFO,
	     "wasapi-capture===>: adding %d milliseconds of audio buffering, total "
	     "audio buffering is now %d milliseconds",
	     (int)ms, (int)total_ms);
#if DEBUG_AUDIO == 1
	blog(LOG_DEBUG,
	     "min_ts (%" PRIu64 ") < start timestamp "
	     "(%" PRIu64 ")",
	     min_ts, ts->start);
	blog(LOG_DEBUG, "old buffered ts: %" PRIu64 "-%" PRIu64, ts->start, ts->end);
#endif

	new_ts.start = wc->buffered_ts - audio_frames_to_ns(sample_rate, wc->buffering_wait_ticks * AUDIO_OUTPUT_FRAMES);

	while (ticks--) {
		uint64_t cur_ticks = ++wc->buffering_wait_ticks;

		new_ts.end = new_ts.start;
		new_ts.start = wc->buffered_ts - audio_frames_to_ns(sample_rate, cur_ticks * AUDIO_OUTPUT_FRAMES);

#if DEBUG_AUDIO == 1
		blog(LOG_DEBUG, "add buffered ts: %" PRIu64 "-%" PRIu64, new_ts.start, new_ts.end);
#endif

		circlebuf_push_front(&wc->buffered_timestamps, &new_ts, sizeof(new_ts));
	}

	*ts = new_ts;
}

static inline void wasapi_capture_mix_audio(struct audio_output_data *mixes, struct audio_channel *source, size_t channels, size_t sample_rate,
					    struct ts_info *ts)
{
	size_t total_floats = AUDIO_OUTPUT_FRAMES;
	size_t start_point = 0;

	if (source->audio_ts < ts->start || ts->end <= source->audio_ts)
		return;

	if (source->audio_ts != ts->start) {
		start_point = convert_time_to_frames(sample_rate, source->audio_ts - ts->start);
		if (start_point == AUDIO_OUTPUT_FRAMES)
			return;

		total_floats -= start_point;
	}

	for (size_t ch = 0; ch < channels; ch++) {
		register float *mix = mixes->data[ch];
		register float *aud = source->audio_output_buf[ch];
		register float *end;

		mix += start_point;
		end = aud + total_floats;

		while (aud < end)
			*(mix++) += *(aud++);
	}
}

static void ignore_audio(struct audio_channel *source, size_t channels, size_t sample_rate)
{
	size_t num_floats = source->audio_input_buf[0].size / sizeof(float);

	if (num_floats) {
		for (size_t ch = 0; ch < channels; ch++)
			circlebuf_pop_front(&source->audio_input_buf[ch], NULL, source->audio_input_buf[ch].size);

		source->last_audio_input_buf_size = 0;
		source->audio_ts += (uint64_t)num_floats * 1000000000ULL / (uint64_t)sample_rate;
	}
}

static bool discard_if_stopped(struct audio_channel *source, size_t channels)
{
	size_t last_size;
	size_t size;

	last_size = source->last_audio_input_buf_size;
	size = source->audio_input_buf[0].size;

	if (!size)
		return false;

	/* if perpetually pending data, it means the audio has stopped,
	 * so clear the audio data */
	if (last_size == size) {
		if (!source->pending_stop) {
			source->pending_stop = true;
#if DEBUG_AUDIO == 1
			blog(LOG_DEBUG, "doing pending stop trick: '0x%p'", source);
#endif
			return true;
		}

		for (size_t ch = 0; ch < channels; ch++)
			circlebuf_pop_front(&source->audio_input_buf[ch], NULL, source->audio_input_buf[ch].size);

		source->pending_stop = false;
		source->audio_ts = 0;
		source->last_audio_input_buf_size = 0;
#if DEBUG_AUDIO == 1
		blog(LOG_DEBUG, "source audio data appears to have "
				"stopped, clearing");
#endif
		return true;
	} else {
		source->last_audio_input_buf_size = size;
		return false;
	}
}

#define MAX_AUDIO_SIZE (AUDIO_OUTPUT_FRAMES * sizeof(float))

static inline void wasapi_capture_discard_audio(struct wasapi_capture *wc, struct audio_channel *source, size_t channels, size_t sample_rate,
						struct ts_info *ts)
{
	size_t total_floats = AUDIO_OUTPUT_FRAMES;
	size_t size;

	if (ts->end <= source->audio_ts) {
#if DEBUG_AUDIO == 1
		blog(LOG_DEBUG,
		     "can't discard, source "
		     "timestamp (%" PRIu64 ") >= "
		     "end timestamp (%" PRIu64 ")",
		     source->audio_ts, ts->end);
#endif
		return;
	}

	if (source->audio_ts < (ts->start - 1)) {
		if (source->audio_pending && source->audio_input_buf[0].size < MAX_AUDIO_SIZE && discard_if_stopped(source, channels))
			return;

#if DEBUG_AUDIO == 1
		blog(LOG_DEBUG,
		     "can't discard, source "
		     "timestamp (%" PRIu64 ") < "
		     "start timestamp (%" PRIu64 ")",
		     source->audio_ts, ts->start);
#endif
		if (wc->total_buffering_ticks == MAX_BUFFERING_TICKS)
			ignore_audio(source, channels, sample_rate);
		return;
	}

	if (source->audio_ts != ts->start && source->audio_ts != (ts->start - 1)) {
		size_t start_point = convert_time_to_frames(sample_rate, source->audio_ts - ts->start);
		if (start_point == AUDIO_OUTPUT_FRAMES) {
#if DEBUG_AUDIO == 1
			blog(LOG_DEBUG, "can't discard, start point is "
					"at audio frame count");
#endif
			return;
		}

		total_floats -= start_point;
	}

	size = total_floats * sizeof(float);

	if (source->audio_input_buf[0].size < size) {
		if (discard_if_stopped(source, channels))
			return;

#if DEBUG_AUDIO == 1
		blog(LOG_DEBUG, "can't discard, data still pending");
#endif
		source->audio_ts = ts->end;
		return;
	}

	for (size_t ch = 0; ch < channels; ch++)
		circlebuf_pop_front(&source->audio_input_buf[ch], NULL, size);

	source->last_audio_input_buf_size = 0;

#if DEBUG_AUDIO == 1
	blog(LOG_DEBUG, "audio discarded, new ts: %" PRIu64, ts->end);
#endif

	source->pending_stop = false;
	source->audio_ts = ts->end;
}

bool wasapi_capture_fetch_audio(struct wasapi_capture *wc, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts, struct audio_output_data *mixes)
{
	da_resize(wc->mix_channels, 0);

	struct ts_info ts;
	ts.start = start_ts_in;
	ts.end = end_ts_in;
	circlebuf_push_back(&wc->buffered_timestamps, &ts, sizeof(ts));
	circlebuf_peek_front(&wc->buffered_timestamps, &ts, sizeof(ts));

	uint64_t min_ts = ts.start;

	size_t audio_size = AUDIO_OUTPUT_FRAMES * sizeof(float);

#if DEBUG_AUDIO == 1
	blog(LOG_DEBUG, "ts %llu-%llu", ts.start, ts.end);
#endif

	pthread_mutex_lock(&wc->channel_mutex);
	for (size_t i = 0; i < wc->audio_channels.num; i++) {
		da_push_back(wc->mix_channels, &(wc->audio_channels.array[i].channel));
	}
	pthread_mutex_unlock(&wc->channel_mutex);
	/* ------------------------------------------------ */
	/* render audio data */
	for (size_t i = 0; i < wc->mix_channels.num; i++) {
		audio_channel_pick_audio_data(wc->mix_channels.array[i], audio_size, wc->channels);
	}

	/* ------------------------------------------------ */
	/* get minimum audio timestamp */
	pthread_mutex_lock(&wc->channel_mutex);
	calc_min_ts(wc, wc->out_sample_info.samples_per_sec, &min_ts);
	pthread_mutex_unlock(&wc->channel_mutex);

	/* ------------------------------------------------ */
	/* if a source has gone backward in time, buffer */
	if (min_ts < ts.start)
		wasapi_capture_add_audio_buffering(wc, wc->out_sample_info.samples_per_sec, &ts, min_ts);

	/* ------------------------------------------------ */
	/* mix audio */
	if (!wc->buffering_wait_ticks) {
		for (size_t i = 0; i < wc->mix_channels.num; i++) {
			struct audio_channel *source = wc->mix_channels.array[i];

			if (source->audio_pending)
				continue;

			pthread_mutex_lock(&source->audio_buf_mutex);

			if (source->audio_output_buf[0][0] && source->audio_ts)
				wasapi_capture_mix_audio(mixes, source, wc->channels, wc->out_sample_info.samples_per_sec, &ts);

			pthread_mutex_unlock(&source->audio_buf_mutex);
		}
	}

	/* ------------------------------------------------ */
	/* discard audio */
	pthread_mutex_lock(&wc->channel_mutex);
	for (size_t i = 0; i < wc->audio_channels.num; i++) {
		struct audio_channel *a_c = wc->audio_channels.array[i].channel;
		pthread_mutex_lock(&a_c->audio_buf_mutex);
		wasapi_capture_discard_audio(wc, a_c, wc->channels, wc->out_sample_info.samples_per_sec, &ts);
		pthread_mutex_unlock(&a_c->audio_buf_mutex);
	}
	pthread_mutex_unlock(&wc->channel_mutex);

	circlebuf_pop_front(&wc->buffered_timestamps, NULL, sizeof(ts));

	*out_ts = ts.start;

	if (wc->buffering_wait_ticks) {
		wc->buffering_wait_ticks--;
		return false;
	}

	return true;
}

static inline void clamp_audio_output(struct wasapi_capture *wc, size_t bytes)
{
	size_t float_size = bytes / sizeof(float);

	for (size_t plane = 0; plane < wc->planes; plane++) {
		float *mix_data = wc->buffer[plane];
		float *mix_end = &mix_data[float_size];

		while (mix_data < mix_end) {
			float val = *mix_data;
			val = (val > 1.0f) ? 1.0f : val;
			val = (val < -1.0f) ? -1.0f : val;
			*(mix_data++) = val;
		}
	}
}

static void wasapi_capture_input_and_output(struct wasapi_capture *wc, uint64_t audio_time, uint64_t prev_time)
{
	size_t bytes = AUDIO_OUTPUT_FRAMES * wc->block_size;
	struct audio_output_data data;
	uint64_t new_ts = 0;
	bool success;

	memset(&data, 0, sizeof(struct audio_output_data));

#if DEBUG_AUDIO == 1
	blog(LOG_DEBUG, "audio_time: %llu, prev_time: %llu, bytes: %lu", audio_time, prev_time, bytes);
#endif

	/* clear mix buffers */
	memset(wc->buffer[0], 0, AUDIO_OUTPUT_FRAMES * MAX_AUDIO_CHANNELS * sizeof(float));

	for (size_t i = 0; i < wc->planes; i++)
		data.data[i] = wc->buffer[i];

	/* get new audio data */
	success = wasapi_capture_fetch_audio(wc, prev_time, audio_time, &new_ts, &data);
	if (!success)
		return;

	///* clamps audio data to -1.0..1.0 */
	clamp_audio_output(wc, bytes);

	struct obs_source_audio audio;
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.frames = AUDIO_OUTPUT_FRAMES;
	audio.samples_per_sec = wc->out_sample_info.samples_per_sec;
	audio.speakers = wc->out_sample_info.speakers;
	audio.timestamp = new_ts;
	for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
		audio.data[i] = (const uint8_t *)data.data[i];
	}
	obs_source_output_audio(wc->source, &audio);
}

static void mix_thread_proc(LPVOID param)
{
	os_set_thread_name("wasapi-capture: audio mix thread");

	struct wasapi_capture *wc = param;
	size_t rate = wc->out_sample_info.samples_per_sec;
	uint64_t samples = 0;
	uint64_t start_time = os_gettime_ns();
	uint64_t prev_time = start_time;
	uint64_t audio_time = prev_time;
	uint32_t audio_wait_time = (uint32_t)(audio_frames_to_ns(rate, AUDIO_OUTPUT_FRAMES) / 1000000);

	while (wc->capturing) {
		os_sleep_ms(audio_wait_time);

		uint64_t cur_time = os_gettime_ns();
		while (audio_time <= cur_time) {
			samples += AUDIO_OUTPUT_FRAMES;
			audio_time = start_time + audio_frames_to_ns(rate, samples);

			wasapi_capture_input_and_output(wc, audio_time, prev_time);

			prev_time = audio_time;
		}
	}
}

static void output_audio_data(struct wasapi_capture *wc, struct obs_source_audio *data, uint64_t ptr)
{
	struct audio_channel *channel = NULL;
	pthread_mutex_lock(&wc->channel_mutex);
	for (size_t i = 0; i < wc->audio_channels.num; i++) {
		if (wc->audio_channels.array[i].ptr == ptr) {
			channel = wc->audio_channels.array[i].channel;
			break;
		}
	}
	pthread_mutex_unlock(&wc->channel_mutex);

	if (!channel) {
		channel = audio_channel_create(&wc->out_sample_info);
		struct audio_channel_info info;
		info.channel = channel;
		info.ptr = ptr;
		pthread_mutex_lock(&wc->channel_mutex);
		da_push_back(wc->audio_channels, &info);
		pthread_mutex_unlock(&wc->channel_mutex);
	}

	audio_channel_output_audio(channel, data);
}

static void capture_thread_proc(LPVOID param)
{
	os_set_thread_name("wasapi-capture: audio capture thread");
	struct wasapi_capture *wc = param;
	while (wc->capturing) {
		if (WaitForSingleObject(wc->audio_data_event, 100) == WAIT_OBJECT_0) {
			if (!wc->capturing)
				break;

			if (WaitForSingleObject(wc->audio_data_mutex, 10) == WAIT_OBJECT_0) {
				uint32_t audio_size = wc->shmem_data->available_audio_size;
				if (audio_size > 0) {
					uint32_t offset = 0;
					while (true) {
						if (offset >= audio_size)
							break;

						uint8_t *buffer_data = wc->audio_data_buffer + offset + 4;

						uint32_t nf = 0;
						memcpy(&nf, wc->audio_data_buffer + offset, 4);
						offset += nf;

						uint64_t ptr = 0;
						uint64_t timestamp = 0;
						uint32_t channel = 0, samplerate = 0, format = 0, byte_per_sample = 0;
						memcpy(&ptr, buffer_data, 8);
						memcpy(&channel, buffer_data + 8, 4);
						memcpy(&samplerate, buffer_data + 12, 4);
						memcpy(&format, buffer_data + 16, 4);
						memcpy(&byte_per_sample, buffer_data + 20, 4);
						memcpy(&timestamp, buffer_data + 24, 8);

						struct obs_source_audio data = {0};
						data.data[0] = (const uint8_t *)(buffer_data + 32);
						data.frames = (uint32_t)((nf - 36) / (channel * byte_per_sample));
						data.speakers = (enum speaker_layout)channel;
						data.samples_per_sec = samplerate;
						data.format = format;
						data.timestamp = timestamp;
						output_audio_data(wc, &data, ptr);

						wc->shmem_data->available_audio_size -= nf;
					}
				}
				ReleaseMutex(wc->audio_data_mutex);
			}
		}
	}
}

static inline bool init_shmem_capture(struct wasapi_capture *wc)
{
	wc->audio_data_buffer = (uint8_t *)wc->data + wc->shmem_data->audio_offset;
	return true;
}

static void start_capture(struct wasapi_capture *wc)
{
	debug("Starting capture");

	if (!init_shmem_capture(wc)) {
		return;
	}

	info("memory capture successful");

	wc->capturing = true;

	wc->capture_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)capture_thread_proc, wc, 0, NULL);
	wc->mix_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)mix_thread_proc, wc, 0, NULL);
}

static void stop_capture(struct wasapi_capture *wc)
{
	info("stop capture called");

	wc->capturing = false;
	SetEvent(wc->audio_data_event);
	if (wc->capture_thread != INVALID_HANDLE_VALUE) {
		WaitForSingleObject(wc->capture_thread, INFINITE);
		wc->capture_thread = INVALID_HANDLE_VALUE;
	}
	if (wc->mix_thread != INVALID_HANDLE_VALUE) {
		WaitForSingleObject(wc->mix_thread, INFINITE);
		wc->mix_thread = INVALID_HANDLE_VALUE;
	}

	if (wc->hook_stop) {
		info("set hook stop event");
		SetEvent(wc->hook_stop);
	}
	if (wc->global_hook_info) {
		UnmapViewOfFile(wc->global_hook_info);
		wc->global_hook_info = NULL;
	}
	if (wc->data) {
		UnmapViewOfFile(wc->data);
		wc->data = NULL;
	}

	if (wc->app_sid) {
		LocalFree(wc->app_sid);
		wc->app_sid = NULL;
	}

	close_handle(&wc->hook_restart);
	close_handle(&wc->hook_stop);
	close_handle(&wc->hook_ready);
	close_handle(&wc->hook_exit);
	close_handle(&wc->hook_init);
	close_handle(&wc->hook_data_map);
	close_handle(&wc->keepalive_mutex);
	close_handle(&wc->global_hook_info_map);
	close_handle(&wc->target_process);
	close_handle(&wc->audio_data_mutex);
	close_handle(&wc->audio_data_event);

	if (wc->active)
		info("capture stopped");

	wc->wait_for_target_startup = false;
	wc->active = false;

	if (wc->retrying)
		wc->retrying--;
}

static const char *wasapi_capture_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "windows wasapi capture";
}

extern void wait_for_hook_initialization(void);
static void wasapi_capture_update(void *data, obs_data_t *settings);
static void *wasapi_capture_create(obs_data_t *settings, obs_source_t *source)
{
	wait_for_hook_initialization();

	struct wasapi_capture *wc = bzalloc(sizeof(*wc));
	wc->source = source;
	wc->initial_config = true;
	wc->retry_interval = DEFAULT_RETRY_INTERVAL;
	wc->capture_thread = INVALID_HANDLE_VALUE;
	pthread_mutex_init_value(&wc->channel_mutex);
	pthread_mutex_init(&wc->channel_mutex, NULL);
	da_init(wc->audio_channels);

	struct obs_audio_info audio_info;
	obs_get_audio_info(&audio_info);
	wc->out_sample_info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	wc->out_sample_info.samples_per_sec = audio_info.samples_per_sec;
	wc->out_sample_info.speakers = audio_info.speakers;

	bool planar = is_audio_planar(wc->out_sample_info.format);
	wc->channels = get_audio_channels(wc->out_sample_info.speakers);
	wc->planes = planar ? wc->channels : 1;
	wc->block_size = (planar ? 1 : wc->channels) * get_audio_bytes_per_channel(wc->out_sample_info.format);

	wasapi_capture_update(wc, settings);
	return wc;
}

static void wasapi_capture_destroy(void *data)
{
	struct wasapi_capture *wc = data;
	stop_capture(wc);

	dstr_free(&wc->executable);

	for (size_t i = 0; i < wc->audio_channels.num; i++) {
		audio_channel_destroy(wc->audio_channels.array[i].channel);
	}
	da_free(wc->audio_channels);

	pthread_mutex_destroy(&wc->channel_mutex);
	circlebuf_free(&wc->buffered_timestamps);
	da_free(wc->mix_channels);

	bfree(wc);
}

static void wasapi_capture_defaults(obs_data_t *settings) {}

static bool window_changed_callback(obs_properties_t *ppts, obs_property_t *p, obs_data_t *settings)
{
	const char *cur_val;
	bool match = false;
	size_t i = 0;

	cur_val = obs_data_get_string(settings, SETTING_CAPTURE_PROCESS);
	if (!cur_val) {
		return false;
	}

	for (;;) {
		const char *val = obs_property_list_item_string(p, i++);
		if (!val)
			break;

		if (strcmp(val, cur_val) == 0) {
			match = true;
			break;
		}
	}

	if (cur_val && *cur_val && !match) {
		obs_property_list_insert_string(p, 1, cur_val, cur_val);
		obs_property_list_item_disable(p, 1, true);
		return true;
	}

	UNUSED_PARAMETER(ppts);
	return false;
}

extern void fill_process_list(obs_property_t *p);
static obs_properties_t *wasapi_capture_properties(void *data)
{
	obs_properties_t *ppts = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(ppts, SETTING_CAPTURE_PROCESS, "Process", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "", "");
	fill_process_list(p);

	obs_property_set_modified_callback(p, window_changed_callback);

	UNUSED_PARAMETER(data);
	return ppts;
}

static void wasapi_capture_update(void *data, obs_data_t *settings)
{
	struct wasapi_capture *wc = data;
	bool reset_capture = false;
	const char *process = obs_data_get_string(settings, SETTING_CAPTURE_PROCESS);

	reset_capture = s_cmp(process, wc->executable.array) != 0;

	wc->error_acquiring = false;
	wc->activate_hook = !!process && !!*process;

	wc->retry_interval = DEFAULT_RETRY_INTERVAL;
	wc->wait_for_target_startup = false;

	dstr_free(&wc->executable);

	dstr_copy(&wc->executable, process);

	if (!wc->initial_config) {
		if (reset_capture) {
			stop_capture(wc);
		}
	} else {
		wc->initial_config = false;
	}
}

static inline bool open_target_process(struct wasapi_capture *wc)
{
	wc->target_process = open_process(PROCESS_QUERY_INFORMATION | SYNCHRONIZE, false, wc->process_id);
	if (!wc->target_process) {
		warn("could not open process: %s", wc->executable);
		return false;
	}

	wc->process_is_64bit = is_64bit_process(wc->target_process);
	wc->is_app = is_app(wc->target_process);
	if (wc->is_app) {
		wc->app_sid = get_app_sid(wc->target_process);
	}
	return true;
}

static inline bool init_keepalive(struct wasapi_capture *wc)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", WINDOW_HOOK_KEEPALIVE, wc->process_id);

	wc->keepalive_mutex = CreateMutexW(NULL, false, new_name);
	if (!wc->keepalive_mutex) {
		warn("Failed to create keepalive mutex: %lu", GetLastError());
		return false;
	}

	return true;
}

/* if there's already a hook in the process, then signal and start */
static inline bool attempt_existing_hook(struct wasapi_capture *wc)
{
	wc->hook_restart = open_event_gc(wc, EVENT_CAPTURE_RESTART);
	if (wc->hook_restart) {
		debug("existing hook found, signaling process: %s", wc->executable);
		SetEvent(wc->hook_restart);
		return true;
	}

	return false;
}

static inline bool create_inject_process(struct wasapi_capture *wc, const char *inject_path, const char *hook_dll)
{
	wchar_t *command_line_w = malloc(4096 * sizeof(wchar_t));
	wchar_t *inject_path_w;
	wchar_t *hook_dll_w;
	PROCESS_INFORMATION pi = {0};
	STARTUPINFO si = {0};
	bool success = false;

	os_utf8_to_wcs_ptr(inject_path, 0, &inject_path_w);
	os_utf8_to_wcs_ptr(hook_dll, 0, &hook_dll_w);

	si.cb = sizeof(si);

	swprintf(command_line_w, 4096, L"\"%s\" \"%s\" %lu %lu", inject_path_w, hook_dll_w, (unsigned long)false, wc->next_process_id);

	success = !!CreateProcessW(inject_path_w, command_line_w, NULL, NULL, false, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	if (success) {
		CloseHandle(pi.hThread);
		wc->injector_process = pi.hProcess;
	} else {
		warn("Failed to create inject helper process: %lu", GetLastError());
	}

	free(command_line_w);
	bfree(inject_path_w);
	bfree(hook_dll_w);
	return success;
}

static inline bool inject_hook(struct wasapi_capture *wc)
{
	bool success = false;
	const char *hook_dll;
	char *inject_path;
	char *hook_path;

	if (wc->process_is_64bit) {
		hook_dll = "wasapi-hook64.dll";
		inject_path = obs_module_file("wasapi-inject-helper64.exe");
	} else {
		hook_dll = "wasapi-hook32.dll";
		inject_path = obs_module_file("wasapi-inject-helper32.exe");
	}

	hook_path = obs_module_file(hook_dll);

	if (!check_file_integrity(wc, inject_path, "inject helper")) {
		goto cleanup;
	}
	if (!check_file_integrity(wc, hook_path, "wasapi hook")) {
		goto cleanup;
	}

	info("using helper (%s hook)", "compatibility");
	success = create_inject_process(wc, inject_path, hook_dll);

cleanup:
	bfree(inject_path);
	bfree(hook_path);
	return success;
}

static inline bool init_audio_data_mutexes(struct wasapi_capture *wc)
{
	wc->audio_data_mutex = open_mutex_gc(wc, AUDIO_DATA_MUTEX);

	if (!wc->audio_data_mutex) {
		DWORD error = GetLastError();
		if (error == 2) {
			if (!wc->retrying) {
				wc->retrying = 2;
				info("hook not loaded yet, retrying..");
			}
		} else {
			warn("failed to open audio data mutexes: %lu", GetLastError());
		}
		return false;
	}

	wc->audio_data_event = open_event_gc(wc, AUDIO_DATA_EVENT);

	if (!wc->audio_data_event) {
		DWORD error = GetLastError();
		if (error == 2) {
			if (!wc->retrying) {
				wc->retrying = 2;
				info("hook not loaded yet, retrying..");
			}
		} else {
			warn("failed to open audio data events: %lu", GetLastError());
		}
		return false;
	}

	return true;
}

static inline bool init_hook_info(struct wasapi_capture *wc)
{
	wc->global_hook_info_map = open_hook_info(wc);
	if (!wc->global_hook_info_map) {
		warn("init_hook_info: get_hook_info failed: %lu", GetLastError());
		return false;
	}

	wc->global_hook_info = MapViewOfFile(wc->global_hook_info_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*wc->global_hook_info));
	wc->global_hook_info->offset = wc->process_is_64bit ? offsets64 : offsets32;
	if (!wc->global_hook_info) {
		warn("init_hook_info: failed to map data view: %lu", GetLastError());
		return false;
	}

	return true;
}

static inline bool init_events(struct wasapi_capture *wc)
{
	if (!wc->hook_restart) {
		wc->hook_restart = open_event_gc(wc, EVENT_CAPTURE_RESTART);
		if (!wc->hook_restart) {
			warn("init_events: failed to get hook_restart "
			     "event: %lu",
			     GetLastError());
			return false;
		}
	}

	if (!wc->hook_stop) {
		wc->hook_stop = open_event_gc(wc, EVENT_CAPTURE_STOP);
		if (!wc->hook_stop) {
			warn("init_events: failed to get hook_stop event: %lu", GetLastError());
			return false;
		} else
			object_signalled(wc->hook_stop);
	}

	if (!wc->hook_init) {
		wc->hook_init = open_event_gc(wc, EVENT_HOOK_INIT);
		if (!wc->hook_init) {
			warn("init_events: failed to get hook_init event: %lu", GetLastError());
			return false;
		}
	}

	if (!wc->hook_ready) {
		wc->hook_ready = open_event_gc(wc, EVENT_HOOK_READY);
		if (!wc->hook_ready) {
			warn("init_events: failed to get hook_ready event: %lu", GetLastError());
			return false;
		}
	}

	if (!wc->hook_exit) {
		wc->hook_exit = open_event_gc(wc, EVENT_HOOK_EXIT);
		if (!wc->hook_exit) {
			warn("init_events: failed to get hook_exit event: %lu", GetLastError());
			return false;
		}
	}

	return true;
}

static bool init_hook(struct wasapi_capture *wc)
{
	info("attempting to hook process: %s", wc->executable.array);

	if (!open_target_process(wc)) {
		return false;
	}
	if (!init_keepalive(wc)) {
		return false;
	}
	if (!attempt_existing_hook(wc)) {
		if (!inject_hook(wc)) {
			info("inject hook failed");
			return false;
		}
	}
	if (!init_audio_data_mutexes(wc)) {
		info("init audio datex mutex failed");
		return false;
	}
	if (!init_hook_info(wc)) {
		info("init hook info failed");
		return false;
	}
	if (!init_events(wc)) {
		info("init events failed");
		return false;
	}

	SetEvent(wc->hook_init);

	wc->process_id = wc->next_process_id;
	wc->next_process_id = 0;
	wc->active = true;
	wc->retrying = 0;
	return true;
}

static void setup_process(struct wasapi_capture *wc, DWORD id)
{
	HANDLE hook_restart;
	HANDLE process;

	wc->process_id = id;
	if (wc->process_id) {
		process = open_process(PROCESS_QUERY_INFORMATION, false, wc->process_id);
		if (process) {
			wc->is_app = is_app(process);
			if (wc->is_app) {
				wc->app_sid = get_app_sid(process);
			}
			CloseHandle(process);
		}
	}

	/* do not wait if we're re-hooking a process */
	hook_restart = open_event_gc(wc, EVENT_CAPTURE_RESTART);
	if (hook_restart) {
		wc->wait_for_target_startup = false;
		CloseHandle(hook_restart);
	}

	/* otherwise if it's an unhooked process, always wait a bit for the
	 * target process to start up before starting the hook process;
	 * sometimes they have important modules to load first or other hooks
	 * (such as steam) need a little bit of time to load.  ultimately this
	 * helps prevent crashes */
	if (wc->wait_for_target_startup) {
		wc->retry_interval = 3.0f;
		wc->wait_for_target_startup = false;
	} else {
		wc->next_process_id = id;
	}
}

extern bool find_selectd_process(const char *process_image_name, DWORD *id, bool *changed, char *new_name);
static void try_hook(struct wasapi_capture *wc)
{
	DWORD id = 0;
	bool changed = false;
	char new_name[MAX_PATH] = {0};
	bool ret = find_selectd_process(wc->executable.array, &id, &changed, new_name);
	if (ret) {
		setup_process(wc, id);
		if (changed) {
			dstr_free(&wc->executable);
			dstr_copy(&wc->executable, new_name);
			obs_data_t *s = obs_source_get_settings(wc->source);
			obs_data_set_string(s, SETTING_CAPTURE_PROCESS, new_name);
			obs_data_release(s);
		}
	} else {
		wc->wait_for_target_startup = true;
	}

	if (wc->next_process_id) {
		if (wc->process_id == GetCurrentProcessId())
			return;

		if (!wc->process_id) {
			warn("error acquiring, failed to get process ids: %lu", GetLastError());
			wc->error_acquiring = true;
			return;
		}

		if (!init_hook(wc)) {
			stop_capture(wc);
		}
	} else {
		wc->active = false;
	}
}

enum capture_result { CAPTURE_FAIL, CAPTURE_RETRY, CAPTURE_SUCCESS };
static inline enum capture_result init_capture_data(struct wasapi_capture *wc)
{
	if (wc->data) {
		UnmapViewOfFile(wc->data);
		wc->data = NULL;
	}

	CloseHandle(wc->hook_data_map);

	wc->hook_data_map = open_map_plus_id(wc, SHMEM_AUDIO, wc->global_hook_info->map_id);
	if (!wc->hook_data_map) {
		DWORD error = GetLastError();
		if (error == 2) {
			return CAPTURE_RETRY;
		} else {
			warn("init_capture_data: failed to open file "
			     "mapping: %lu",
			     error);
		}
		return CAPTURE_FAIL;
	}

	wc->data = MapViewOfFile(wc->hook_data_map, FILE_MAP_ALL_ACCESS, 0, 0, wc->global_hook_info->map_size);
	if (!wc->data) {
		warn("init_capture_data: failed to map data view: %lu", GetLastError());
		return CAPTURE_FAIL;
	}

	return CAPTURE_SUCCESS;
}

static void wasapi_capture_tick(void *data, float seconds)
{
	struct wasapi_capture *wc = data;

	if (wc->hook_stop && object_signalled(wc->hook_stop)) {
		debug("hook stop signal received");
		stop_capture(wc);
	}

	if (wc->active && !wc->hook_ready && wc->process_id) {
		wc->hook_ready = open_event_gc(wc, EVENT_HOOK_READY);
	}

	if (wc->injector_process && object_signalled(wc->injector_process)) {
		DWORD exit_code = 0;

		GetExitCodeProcess(wc->injector_process, &exit_code);
		close_handle(&wc->injector_process);

		if (exit_code != 0) {
			warn("inject process failed: %ld", (long)exit_code);
			wc->error_acquiring = true;
		}
	}

	if (wc->hook_ready && object_signalled(wc->hook_ready)) {
		debug("capture initializing!");
		enum capture_result result = init_capture_data(wc);

		if (result == CAPTURE_SUCCESS)
			start_capture(wc);
		else
			debug("init_capture_data failed");

		if (result != CAPTURE_RETRY && !wc->capturing) {
			wc->retry_interval = ERROR_RETRY_INTERVAL;
			stop_capture(wc);
		}
	}

	wc->retry_time += seconds;

	if (!wc->active) {
		if (!wc->error_acquiring && wc->retry_time > wc->retry_interval) {
			if (wc->activate_hook) {
				try_hook(wc);
				wc->retry_time = 0.0f;
			}
		}
	} else {
		if (object_signalled(wc->target_process)) {
			info("capture process no longer exists, "
			     "terminating capture");
			stop_capture(wc);
		}
	}
}

struct obs_source_info wasapi_capture_info = {
	.id = "wasapi_capture",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = wasapi_capture_name,
	.create = wasapi_capture_create,
	.destroy = wasapi_capture_destroy,
	.get_defaults = wasapi_capture_defaults,
	.get_properties = wasapi_capture_properties,
	.update = wasapi_capture_update,
	.video_tick = wasapi_capture_tick,
};
