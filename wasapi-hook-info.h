#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>

#include "hook-helpers.h"

#define EVENT_CAPTURE_RESTART L"CaptureHook_Restart"
#define EVENT_CAPTURE_STOP L"CaptureHook_Stop"

#define EVENT_HOOK_READY L"CaptureHook_HookReady"
#define EVENT_HOOK_EXIT L"CaptureHook_Exit"

#define EVENT_HOOK_INIT L"CaptureHook_Initialize"

#define WINDOW_HOOK_KEEPALIVE L"CaptureHook_KeepAlive"

#define AUDIO_DATA_MUTEX L"CaptureHook_Audio_Data_Mutex"
#define AUDIO_DATA_EVENT L"CaptureHook_Audio_Data_Event"

#define SHMEM_HOOK_INFO L"CaptureHook_HookInfo"
#define SHMEM_AUDIO L"CaptureHook_Audio"

#pragma pack(push, 8)

struct shmem_data {
	volatile uint32_t available_audio_size;
	uint32_t audio_offset;
	uint32_t buffer_size;
};

struct wasapi_offset {
	uint32_t release_buffer;
	uint32_t get_service;
	uint32_t audio_client_offset;
	uint32_t waveformat_offset;
	uint32_t buffer_offset;
};

struct hook_info {
	uint32_t map_id;
	uint32_t map_size;

	struct wasapi_offset offset;
};

#pragma pack(pop)

#define GC_MAPPING_FLAGS (FILE_MAP_READ | FILE_MAP_WRITE)

static inline HANDLE create_hook_info(DWORD id)
{
	wchar_t new_name[64];
	_snwprintf(new_name, 64, L"%s%lu", SHMEM_HOOK_INFO, id);

	return CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, sizeof(struct hook_info), new_name);
}
