#include <inttypes.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include "wasapi-hook-info.h"
#include "audio-channel.h"

#define do_log(level, format, ...) blog(level, "[wasapi-capture: '%s'] " format, obs_source_get_name(wc->source), ##__VA_ARGS__)

#define warn(format, ...) do_log(LOG_WARNING, format, ##__VA_ARGS__)
#define info(format, ...) do_log(LOG_INFO, format, ##__VA_ARGS__)
#define debug(format, ...) do_log(LOG_DEBUG, format, ##__VA_ARGS__)

struct ts_info {
	uint64_t start;
	uint64_t end;
};

struct audio_channel_info {
	uint64_t ptr;
	struct audio_channel *channel;
};

struct wasapi_capture {
	obs_source_t *source;

	HANDLE injector_process;

	DWORD process_id;
	DWORD next_process_id;
	float retry_time;
	float retry_interval;

	struct dstr executable;

	enum window_priority priority;
	bool wait_for_target_startup;
	bool active;
	bool capturing;
	bool activate_hook;
	bool process_is_64bit;
	bool error_acquiring;
	bool initial_config;
	bool is_app;

	struct hook_info *global_hook_info;
	HANDLE keepalive_mutex;
	HANDLE hook_init;
	HANDLE hook_restart;
	HANDLE hook_stop;
	HANDLE hook_ready;
	HANDLE hook_exit;
	HANDLE hook_data_map;
	HANDLE global_hook_info_map;
	HANDLE target_process;
	HANDLE audio_data_mutex;
	HANDLE audio_data_event;
	wchar_t *app_sid;
	int retrying;

	union {
		struct {
			struct shmem_data *shmem_data;
			uint8_t *audio_data_buffer;
		};

		void *data;
	};

	HANDLE capture_thread;
	HANDLE mix_thread;
	struct resample_info out_sample_info;
	DARRAY(struct audio_channel_info) audio_channels;
	size_t block_size;
	size_t channels;
	size_t planes;
	float buffer[MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];

	DARRAY(struct audio_channel *) mix_channels;

	pthread_mutex_t channel_mutex;
	uint64_t buffered_ts;
	struct circlebuf buffered_timestamps;
	uint64_t buffering_wait_ticks;
	int total_buffering_ticks;
};

static inline HANDLE open_mutex_plus_id(struct wasapi_capture *wc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return wc->is_app ? open_app_mutex(wc->app_sid, new_name) : open_mutex(new_name);
}

static inline HANDLE open_mutex_gc(struct wasapi_capture *wc, const wchar_t *name)
{
	return open_mutex_plus_id(wc, name, wc->process_id);
}

static inline HANDLE open_event_plus_id(struct wasapi_capture *wc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);
	return wc->is_app ? open_app_event(wc->app_sid, new_name) : open_event(new_name);
}

static inline HANDLE open_event_gc(struct wasapi_capture *wc, const wchar_t *name)
{
	return open_event_plus_id(wc, name, wc->process_id);
}

static inline HANDLE open_map_plus_id(struct wasapi_capture *wc, const wchar_t *name, DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", name, id);

	debug("map id: %S", new_name);

	return wc->is_app ? open_app_map(wc->app_sid, new_name) : OpenFileMappingW(GC_MAPPING_FLAGS, false, new_name);
}

static inline HANDLE open_hook_info(struct wasapi_capture *wc)
{
	return open_map_plus_id(wc, SHMEM_HOOK_INFO, wc->process_id);
}

static inline void close_handle(HANDLE *p_handle)
{
	HANDLE handle = *p_handle;
	if (handle) {
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
		*p_handle = NULL;
	}
}

static inline HMODULE kernel32(void)
{
	static HMODULE kernel32_handle = NULL;
	if (!kernel32_handle)
		kernel32_handle = GetModuleHandleW(L"kernel32");
	return kernel32_handle;
}

HANDLE open_process(DWORD desired_access, bool inherit_handle, DWORD process_id)
{
	static HANDLE(WINAPI * open_process_proc)(DWORD, BOOL, DWORD) = NULL;
	if (!open_process_proc)
		open_process_proc = ms_get_obfuscated_func(kernel32(), "NuagUykjcxr", 0x1B694B59451ULL);

	return open_process_proc(desired_access, inherit_handle, process_id);
}

int s_cmp(const char *str1, const char *str2)
{
	if (!str1 || !str2)
		return -1;

	return strcmp(str1, str2);
}

static inline bool is_64bit_windows(void)
{
#ifdef _WIN64
	return true;
#else
	BOOL x86 = false;
	bool success = !!IsWow64Process(GetCurrentProcess(), &x86);
	return success && !!x86;
#endif
}

static inline bool is_64bit_process(HANDLE process)
{
	BOOL x86 = true;
	if (is_64bit_windows()) {
		bool success = !!IsWow64Process(process, &x86);
		if (!success) {
			return false;
		}
	}

	return !x86;
}
