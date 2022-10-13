#include <Windows.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include "wasapi-hook.h"

static struct func_hook structHookAudioClientInitialize;
static struct func_hook structHookAudioClientStop;
static struct func_hook structHookAudioRenderClientGetBuffer;
static struct func_hook structHookAudioRenderClientReleaseBuffer;

#ifdef __cplusplus
extern "C" {
#endif
typedef HRESULT(STDMETHODCALLTYPE *AudioClientInitialize)(
	IAudioClient *pAudioClient, AUDCLNT_SHAREMODE ShareMode,
	DWORD stream_flags, REFERENCE_TIME hns_buffer_duration,
	REFERENCE_TIME hns_periodicity, const WAVEFORMATEX *p_format,
	LPCGUID audio_session_guid);
typedef HRESULT(STDMETHODCALLTYPE *AudioClientStop)(IAudioClient *pAudioClient);
typedef HRESULT(STDMETHODCALLTYPE *AudioRenderClientGetBuffer)(
	IAudioRenderClient *audio_render_client, UINT32 num_frames_requested,
	BYTE **ppData);
typedef HRESULT(STDMETHODCALLTYPE *AudioRenderClientReleaseBuffer)(
	IAudioRenderClient *audio_render_client, UINT32 num_frames_written,
	DWORD dw_flags);
#ifdef __cplusplus
}
#endif

AudioClientInitialize realAudioClientInitialize = NULL;
AudioClientStop realAudioClientStop = NULL;
AudioRenderClientGetBuffer realAudioRenderClientGetBuffer = NULL;
AudioRenderClientReleaseBuffer realAudioRenderClientReleaseBuffer = NULL;

class WASAPICapture {
public:
	WASAPICapture() {}
};
HRESULT STDMETHODCALLTYPE hookAudioClientInitialize(
	IAudioClient *pAudioClient, AUDCLNT_SHAREMODE share_mode,
	DWORD stream_flags, REFERENCE_TIME hns_buffer_duration,
	REFERENCE_TIME hns_periodicity, const WAVEFORMATEX *pFormat,
	LPCGUID audio_session_guid)
{
	if (pFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		WAVEFORMATEXTENSIBLE *wFormatex =
			(WAVEFORMATEXTENSIBLE *)pFormat;
		WORD size = wFormatex->Format.cbSize;
	}
	/*gWASAPIAudioCaptureProxy->on_audioclient_initialized(pAudioClient,
							     pFormat);*/
	HRESULT hr = realAudioClientInitialize(
		pAudioClient, share_mode, stream_flags, hns_buffer_duration,
		hns_periodicity, pFormat, audio_session_guid);
	//LOGGER::make_trace_log(ELASC, "%s()_%d : called!! hr=%d, %s", __FUNCTION__, __LINE__, hr, gWASAPIAudioCaptureProxy->AsHumanReadableString(pFormat).c_str());
	return hr;
}

HRESULT STDMETHODCALLTYPE hookAudioClientStop(IAudioClient *pAudioClient)
{
	HRESULT hr = S_OK;
	hr = realAudioClientStop(pAudioClient);

	if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING)
		hlog("[%d] AudioClientStopHook: called!! (%s)",
		     GetCurrentThreadId(), "AUDCLNT_E_SERVICE_NOT_RUNNING");
	if (hr == AUDCLNT_E_NOT_INITIALIZED)
		hlog("[%d] AudioClientStopHook: called!! (%s)",
		     GetCurrentThreadId(), "AUDCLNT_E_NOT_INITIALIZED");

	if (SUCCEEDED(hr)) {
		/*gWASAPIAudioCaptureProxy->on_audioclient_stopped(pAudioClient,
								 FALSE);*/
	} else {
		/*gWASAPIAudioCaptureProxy->on_audioclient_stopped(pAudioClient,
								 TRUE);*/
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE
hookAudioRenderClientGetBuffer(IAudioRenderClient *pAudioRenderClient,
			       UINT32 nFrameRequested, BYTE **ppData)
{
	HRESULT hr = realAudioRenderClientGetBuffer(pAudioRenderClient,
						    nFrameRequested, ppData);
	/*gWASAPIAudioCaptureProxy->push_audio_data(pAudioRenderClient, ppData);*/
	return hr;
}

HRESULT STDMETHODCALLTYPE
hookAudioRenderClientReleaseBuffer(IAudioRenderClient *pAudioRenderClient,
				   UINT32 nFrameWritten, DWORD dwFlags)
{
	HRESULT hr = S_OK;

	/* AUDCLNT_BUFFERFLAGS_SILENT = 0x2
	Treat all of the data in the packet as silence and ignore the actual data values */
	if (SUCCEEDED(hr)) {
		if (dwFlags == AUDCLNT_BUFFERFLAGS_SILENT) {
			//gWASAPIAudioCaptureProxy->output_stream_added(pAudioRenderClient);
		} else {
			/*gWASAPIAudioCaptureProxy->capture_audio(
				pAudioRenderClient,
				gWASAPIAudioCaptureProxy->front_audio_data(
					pAudioRenderClient),
				nFrameWritten);
			gWASAPIAudioCaptureProxy->pop_audio_data(
				pAudioRenderClient);*/
		}
	}

	hr = realAudioRenderClientReleaseBuffer(pAudioRenderClient,
						nFrameWritten, dwFlags);
	return hr;
}

bool hook_wasapi()
{
	HMODULE user_module = get_system_module("User32.dll");
	void *init = nullptr;
	void *stop = nullptr;
	void *get_buffer = nullptr;
	void *release_buffer = nullptr;

	if (!user_module) {
		return false;
	}

	init =
		GetProcAddress(user_module, "UpdateLayeredWindow");
	if (!update_layered_window_addr) {
		hlog("Invalid Lyric values");
		return true;
	}
	if (update_layered_window_addr) {
		hook_init(&updatelayeredwindow, update_layered_window_addr,
			  (void *)hook_update_layered_window,
			  "UpdateLayeredWindow");
		rehook(&updatelayeredwindow);
	}

	update_layered_window_indirect_addr =
		GetProcAddress(user_module, "UpdateLayeredWindowIndirect");
	if (!update_layered_window_indirect_addr) {
		hlog("Invalid Lyric values");
		return true;
	}
	if (update_layered_window_indirect_addr) {
		hook_init(&updateLayeredWindowIndirect,
			  update_layered_window_indirect_addr,
			  (void *)hook_update_layered_window_indirect,
			  "UpdateLayeredWindowIndirect");
		rehook(&updateLayeredWindowIndirect);
	}

	hlog("Hooked Lyric");

	return true;
}
