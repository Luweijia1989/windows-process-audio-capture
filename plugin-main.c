#include <windows.h>
#include <obs-module.h>
#include <util/windows/win-version.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/pipe.h>
#include <util/config-file.h>
#include "wasapi-hook-info.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("wasapi-capture", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Windows wasapi capture";
}

extern struct obs_source_info wasapi_capture_info;
extern struct wasapi_offset offsets32;
extern struct wasapi_offset offsets64;

static HANDLE init_hooks_thread = NULL;

/* temporary, will eventually be erased once we figure out how to create both
 * 32bit and 64bit versions of the helpers/hook */
#ifdef _WIN64
#define IS32BIT false
#else
#define IS32BIT true
#endif

static inline bool load_offsets_from_string(struct wasapi_offset *offsets, const char *str)
{
	config_t *config;

	if (config_open_string(&config, str) != CONFIG_SUCCESS) {
		return false;
	}

	offsets->release_buffer = (uint32_t)config_get_uint(config, "wasapi", "release_buffer");
	offsets->get_service = (uint32_t)config_get_uint(config, "wasapi", "get_service");
	offsets->audio_client_offset = (uint32_t)config_get_uint(config, "wasapi", "audio_client_offset");
	offsets->waveformat_offset = (uint32_t)config_get_uint(config, "wasapi", "waveformat_offset");
	offsets->buffer_offset = (uint32_t)config_get_uint(config, "wasapi", "buffer_offset");

	config_close(config);
	return true;
}

bool load_wasapi_offsets(bool is32bit)
{
	char *offset_exe_path = NULL;
	struct dstr offset_exe = {0};
	struct dstr str = {0};
	os_process_pipe_t *pp;
	bool success = false;
	char data[2048];

#ifndef _WIN64
	if (!is32bit && !is_64_bit_windows()) {
		return true;
	}
#endif

	dstr_copy(&offset_exe, "get-wasapi-offsets");
	dstr_cat(&offset_exe, is32bit ? "32.exe" : "64.exe");
	offset_exe_path = obs_module_file(offset_exe.array);

	pp = os_process_pipe_create(offset_exe_path, "r");
	if (!pp) {
		blog(LOG_INFO, "load_wasapi_offsets: Failed to start '%s'", offset_exe.array);
		goto error;
	}

	for (;;) {
		size_t len = os_process_pipe_read(pp, (uint8_t *)data, sizeof(data));
		if (!len)
			break;

		dstr_ncat(&str, data, len);
	}

	if (dstr_is_empty(&str)) {
		blog(LOG_INFO,
		     "load_wasapi_offsets: Failed to read "
		     "from '%s'",
		     offset_exe.array);
		goto error;
	}

	success = load_offsets_from_string(is32bit ? &offsets32 : &offsets64, str.array);
	if (!success) {
		blog(LOG_INFO, "load_wasapi_offsets: Failed to load string");
	}

	os_process_pipe_destroy(pp);

error:
	bfree(offset_exe_path);
	dstr_free(&offset_exe);
	dstr_free(&str);
	return success;
}

static DWORD WINAPI init_hooks(LPVOID param)
{
	if (load_wasapi_offsets(IS32BIT)) {
		load_wasapi_offsets(!IS32BIT);
	}
	return 0;
}

void wait_for_hook_initialization(void)
{
	static bool initialized = false;

	if (!initialized) {
		if (init_hooks_thread) {
			WaitForSingleObject(init_hooks_thread, INFINITE);
			CloseHandle(init_hooks_thread);
			init_hooks_thread = NULL;
		}
		initialized = true;
	}
}

bool obs_module_load(void)
{
	init_hooks_thread = CreateThread(NULL, 0, init_hooks, NULL, 0, NULL);
	obs_register_source(&wasapi_capture_info);
	return true;
}

void obs_module_unload(void)
{
	wait_for_hook_initialization();
}
