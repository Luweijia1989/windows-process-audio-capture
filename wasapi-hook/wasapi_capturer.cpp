#include "wasapi_capturer.h"
#include <process.h>
#include "wasapi-hook.h"

#include <set>
#include <detours.h>

#include "wasapi_capture_proxy.h"

#define SAFE_RELEASE(X)               \
	{                             \
		if (X) {              \
			X->Release(); \
			X = NULL;     \
		}                     \
	}

static WASCaptureData capture_data;

#ifdef __cplusplus
extern "C" {
#endif
typedef HRESULT(STDMETHODCALLTYPE *AudioRenderClientReleaseBuffer)(IAudioRenderClient *audio_render_client, UINT32 num_frames_written, DWORD dw_flags);
typedef HRESULT(STDMETHODCALLTYPE *AudioClientGetService)(IAudioClient *audio_client, REFIID riid, void **ppv);
#ifdef __cplusplus
}
#endif

AudioRenderClientReleaseBuffer realAudioRenderClientReleaseBuffer = NULL;
AudioClientGetService realAudioClientGetService = NULL;

HRESULT STDMETHODCALLTYPE hookAudioClientGetService(IAudioClient *audio_client, REFIID riid, void **ppv)
{
	auto ret = realAudioClientGetService(audio_client, riid, ppv);
	return ret;
}

HRESULT STDMETHODCALLTYPE hookAudioRenderClientReleaseBuffer(IAudioRenderClient *pAudioRenderClient, UINT32 nFrameWritten, DWORD dwFlags)
{
	if (!(dwFlags & AUDCLNT_BUFFERFLAGS_SILENT)) {
		capture_data.capture_check(pAudioRenderClient);
		capture_data.out_audio_data(pAudioRenderClient, nFrameWritten);
	}
	return realAudioRenderClientReleaseBuffer(pAudioRenderClient, nFrameWritten, dwFlags);
}

WASCaptureData::WASCaptureData() {}

WASCaptureData::~WASCaptureData() {}

static void waveformatToAudioInfo(WAVEFORMATEX *wfex, WASCaptureData::audio_info *info)
{
	bool bfloat = false;
	if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		WAVEFORMATEXTENSIBLE *wfext = (WAVEFORMATEXTENSIBLE *)wfex;
		if (wfext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
			bfloat = true;
	} else if (wfex->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
		bfloat = true;
	}

	info->format = 0;
	if (bfloat) {
		if (wfex->wBitsPerSample == 32)
			info->format = 4;
	} else {
		switch (wfex->wBitsPerSample) {
		case 8:
			info->format = 1;
			break;
		case 16:
			info->format = 2;
			break;
		case 32:
			info->format = 3;
			break;
		default:
			break;
		}
	}

	info->channels = wfex->nChannels;
	info->samplerate = wfex->nSamplesPerSec;
	info->byte_per_sample = wfex->wBitsPerSample / 8;
}

void WASCaptureData::out_audio_data(IAudioRenderClient *pAudioRenderClient, UINT32 nFrameWritten)
{
	if (nFrameWritten == 0)
		return;

	uint64_t timestamp = os_gettime_ns();

	std::unique_lock<std::mutex> lk(_mutex);
	if (!capture_active())
		return;

	IAudioClient *audio_client = *(IAudioClient **)((uintptr_t)pAudioRenderClient + global_hook_info->offset.audio_client_offset);
	uint8_t *buffer = *(uint8_t **)((uintptr_t)pAudioRenderClient + global_hook_info->offset.buffer_offset);
	WAVEFORMATEX *wfex = *(WAVEFORMATEX **)((uintptr_t)pAudioRenderClient + global_hook_info->offset.waveformat_offset);

	audio_info info;
	waveformatToAudioInfo(wfex, &info);

	uint32_t len = nFrameWritten * wfex->nChannels * wfex->wBitsPerSample / 8;

	DWORD wait_result = WAIT_FAILED;
	wait_result = WaitForSingleObject(audio_data_mutex, 0);
	if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
		uint32_t count = len + 36;
		if (_shmem_data_info->available_audio_size + count <= _shmem_data_info->buffer_size) {
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size, &count, 4);
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size + 4, &audio_client, 8);
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size + 12, &info.channels, 4);
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size + 16, &info.samplerate, 4);
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size + 20, &info.format, 4);
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size + 24, &info.byte_per_sample, 4);
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size + 28, &timestamp, 8);
			memcpy(audio_data_pointer + _shmem_data_info->available_audio_size + 36, buffer, len);
			_shmem_data_info->available_audio_size += count;
		}
		ReleaseMutex(audio_data_mutex);
		SetEvent(audio_data_event);
	}
}

void WASCaptureData::capture_check(IAudioRenderClient *pAudioRenderClient)
{
	std::unique_lock<std::mutex> lk(_mutex);

	if (capture_should_stop()) {
		capture_free();
	}

	WAVEFORMATEX *wfex = *(WAVEFORMATEX **)((uintptr_t)pAudioRenderClient + global_hook_info->offset.waveformat_offset);
	if (capture_should_init()) {
		audio_info info;
		waveformatToAudioInfo(wfex, &info);
		bool success = capture_init_shmem(&_shmem_data_info, &audio_data_pointer);
		if (!success)
			capture_free();
	}
}

bool hook_wasapi()
{
	if (!global_hook_info->offset.release_buffer || !global_hook_info->offset.get_service || !global_hook_info->offset.audio_client_offset ||
	    !global_hook_info->offset.waveformat_offset || !global_hook_info->offset.buffer_offset)
		return true;

	HMODULE module = load_system_library("AudioSes.dll");
	uint32_t size;
	void *release_buffer_addr = nullptr;
	void *get_service_addr = nullptr;

	if (!module) {
		return false;
	}

	size = module_size(module);

	if (global_hook_info->offset.release_buffer < size && global_hook_info->offset.get_service < size) {
		release_buffer_addr = get_offset_addr(module, global_hook_info->offset.release_buffer);
		get_service_addr = get_offset_addr(module, global_hook_info->offset.get_service);
	} else
		return true;

	if (!release_buffer_addr || !get_service_addr) {
		hlog("Invalid func addr");
		return true;
	}

	DetourTransactionBegin();
	realAudioRenderClientReleaseBuffer = (AudioRenderClientReleaseBuffer)release_buffer_addr;
	DetourAttach((PVOID *)&realAudioRenderClientReleaseBuffer, hookAudioRenderClientReleaseBuffer);
	realAudioClientGetService = (AudioClientGetService)get_service_addr;
	DetourAttach((PVOID *)&realAudioClientGetService, hookAudioClientGetService);

	const LONG error = DetourTransactionCommit();
	bool success = error == NO_ERROR;
	if (success) {
		hlog("Hooked IAudioRenderClient::ReleaseBuffer");
		hlog("Hooked wasapi");
	} else {
		realAudioRenderClientReleaseBuffer = nullptr;
		hlog("Failed to attach Detours hook: %ld", error);
	}

	return success;
}
