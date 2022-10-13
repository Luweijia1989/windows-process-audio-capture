#include <inttypes.h>
#include <stdio.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <set>
#include <algorithm>
#include "../wasapi-hook-info.h"

#define SAFE_RELEASE(X)               \
	{                             \
		if (X) {              \
			X->Release(); \
			X = NULL;     \
		}                     \
	}

static inline uint32_t vtable_offset(HMODULE module, void *cls, unsigned int offset)
{
	uintptr_t *vtable = *(uintptr_t **)cls;
	return (uint32_t)(vtable[offset] - (uintptr_t)module);
}

void search_memory(PVOID wave_format, DWORD size1, std::set<uintptr_t> &wave_format_check, PVOID audio_client, DWORD size2,
		   std::set<uintptr_t> &audio_client_check, PVOID buffer, DWORD size3, std::set<uintptr_t> &buffer_check)
{
	HANDLE process = ::GetCurrentProcess();
	if (NULL == process) {
		return;
	}

	SYSTEM_INFO info;
	GetSystemInfo(&info);
	BYTE *search_address = (BYTE *)info.lpMinimumApplicationAddress;
	MEMORY_BASIC_INFORMATION mbi = {0};
	SIZE_T ret = 0;
	BYTE *temp = nullptr;
	BYTE *buf = nullptr;

	while (true) {
		::RtlZeroMemory(&mbi, sizeof(mbi));
		ret = ::VirtualQueryEx(process, search_address, &mbi, sizeof(mbi));
		if (0 == ret) {
			break;
		}
		if ((MEM_COMMIT == mbi.State) && (PAGE_READONLY == mbi.Protect || PAGE_READWRITE == mbi.Protect || PAGE_EXECUTE_READ == mbi.Protect ||
						  PAGE_EXECUTE_READWRITE == mbi.Protect)) {
			buf = new BYTE[mbi.RegionSize];
			::RtlZeroMemory(buf, mbi.RegionSize);
			if (!ReadProcessMemory(process, mbi.BaseAddress, buf, mbi.RegionSize, &ret)) {
				break;
			}

			auto max = std::max<DWORD>(std::max<DWORD>(size1, size2), size3);
			for (DWORD i = 0; i < (mbi.RegionSize - max); i++) {
				temp = (BYTE *)buf + i;
				if (RtlEqualMemory(temp, wave_format, size1)) {
					wave_format_check.insert((uintptr_t)((BYTE *)mbi.BaseAddress + i));
				}

				if (RtlEqualMemory(temp, audio_client, size2)) {
					audio_client_check.insert((uintptr_t)((BYTE *)mbi.BaseAddress + i));
				}

				if (RtlEqualMemory(temp, buffer, size3)) {
					buffer_check.insert((uintptr_t)((BYTE *)mbi.BaseAddress + i));
				}
			}

			delete[] buf;
			buf = NULL;
		}
		search_address = search_address + mbi.RegionSize;
	}

	if (buf) {
		delete[] buf;
		buf = NULL;
	}
	::CloseHandle(process);
}

void get_wasapi_offset(struct wasapi_offset *ret)
{
	::CoInitializeEx(NULL, COINIT_MULTITHREADED);

	REFERENCE_TIME hns_req_duration = 10000000;
	IMMDeviceEnumerator *dev_enum = NULL;
	IMMDevice *imm_device = NULL;
	IAudioClient *audio_client = NULL;
	WAVEFORMATEX *audio_format = NULL;

	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void **)&dev_enum);
	if (hr == CO_E_NOTINITIALIZED) {
		hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
		hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&dev_enum);
		if (SUCCEEDED(hr)) {
			hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_INPROC_SERVER, __uuidof(IMMDeviceEnumerator), (void **)&dev_enum);
		}
	}

	dev_enum->GetDefaultAudioEndpoint(eRender, eConsole, &imm_device);
	imm_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void **)&audio_client);
	audio_client->GetMixFormat(&audio_format);
	audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hns_req_duration, 0, audio_format, NULL);
	IAudioRenderClient *audio_render_client = nullptr;
	audio_client->GetService(__uuidof(IAudioRenderClient), (void **)&audio_render_client);
	BYTE *data = NULL;
	audio_render_client->GetBuffer(1024, &data);

	std::set<uintptr_t> wave_format_check;
	std::set<uintptr_t> audio_client_check;
	std::set<uintptr_t> buffer_check;
	search_memory(audio_format, sizeof(WAVEFORMATEX), wave_format_check, &audio_client, sizeof(void *), audio_client_check, &data, sizeof(void *),
		      buffer_check);
	if (wave_format_check.size() > 0 && audio_client_check.size() > 0 && buffer_check.size() > 0) {
		uint8_t *pRawAudioClient = (uint8_t *)audio_render_client;
		bool gotWaveformatOffset = false;
		bool gotAudioClientOffset = false;
		bool gotBufferOffset = false;
		uint32_t offset = 0;
		while (offset < 1024) // The highest offset I got was 904, i never got any segfault errors. This value should maybe be tweaked
		{
			uintptr_t *curr = (uintptr_t *)(pRawAudioClient + offset);

			if (wave_format_check.find(*curr) != wave_format_check.end() && !gotWaveformatOffset) {
				gotWaveformatOffset = true;
				ret->waveformat_offset = offset;
			}

			if (audio_client_check.find((uintptr_t)curr) != audio_client_check.end() && !gotAudioClientOffset) {
				gotAudioClientOffset = true;
				ret->audio_client_offset = offset;
			}

			if (buffer_check.find((uintptr_t)curr) != buffer_check.end() && !gotBufferOffset) {
				gotBufferOffset = true;
				ret->buffer_offset = offset;
			}
			++offset;
		}
	}
	CoTaskMemFree(audio_format);

	auto module = GetModuleHandleA("AudioSes.dll");
	ret->release_buffer = vtable_offset(module, audio_render_client, 4);
	ret->get_service = vtable_offset(module, audio_client, 14);

	SAFE_RELEASE(dev_enum);
	SAFE_RELEASE(imm_device);
	SAFE_RELEASE(audio_client);
	SAFE_RELEASE(audio_render_client);

	CoUninitialize();
}

int main(int argc, char *argv[])
{
	struct wasapi_offset offset = {0};
	get_wasapi_offset(&offset);
	printf("[wasapi]\n");
	printf("release_buffer=0x%" PRIx32 "\n", offset.release_buffer);
	printf("get_service=0x%" PRIx32 "\n", offset.get_service);
	printf("audio_client_offset=0x%" PRIx32 "\n", offset.audio_client_offset);
	printf("waveformat_offset=0x%" PRIx32 "\n", offset.waveformat_offset);
	printf("buffer_offset=0x%" PRIx32 "\n", offset.buffer_offset);

	(void)argc;
	(void)argv;
	return 0;
}
