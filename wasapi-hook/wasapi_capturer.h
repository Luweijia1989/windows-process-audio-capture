#ifndef _WASAPI_AUDIO_CAPTURE_H_
#define _WASAPI_AUDIO_CAPTURE_H_

#include <windows.h>
#include <shlobj.h>
#include <psapi.h>
#pragma intrinsic(memcpy, memset, memcmp)

#include <objbase.h>
#include <string>
#include <sstream>
#include <fstream>
#include <assert.h>
#include <mutex>
#include <map>
#include <ctime>   // std::time
#include <iomanip> // std::put_time
#include <immintrin.h>
#include <emmintrin.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>

#include <initguid.h>
#include <wincodec.h>

#include "circlebuf.h"

class WASCaptureData {
public:
	struct audio_info {
		int32_t channels = 0;
		int32_t samplerate = 0;
		int32_t byte_per_sample = 0;
		uint32_t format = 0;
	};

	uint32_t waveformatOffset = 0;
	uint32_t audioClientOffset = 0;
	uint32_t bufferOffset = 0;

	WASCaptureData();
	~WASCaptureData();

	void capture_check(IAudioRenderClient *pAudioRenderClient);
	void out_audio_data(IAudioRenderClient *pAudioRenderClient, UINT32 nFrameWritten);

private:
	uint8_t *audio_data_pointer = nullptr;
	struct shmem_data *_shmem_data_info;
	std::mutex _mutex;
};

#endif
