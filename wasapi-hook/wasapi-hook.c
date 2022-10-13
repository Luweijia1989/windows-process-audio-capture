#include <windows.h>
#include <psapi.h>
#include "wasapi-hook.h"
#include "../../libobs/util/windows/obfuscate.h"

#define DEBUG_OUTPUT

#ifdef DEBUG_OUTPUT
#define DbgOut(x) OutputDebugStringA(x)
#else
#define DbgOut(x)
#endif

HANDLE signal_restart = NULL;
HANDLE signal_stop = NULL;
HANDLE signal_ready = NULL;
HANDLE signal_exit = NULL;
static HANDLE signal_init = NULL;
HANDLE audio_data_mutex = NULL;
HANDLE audio_data_event = NULL;
static HANDLE filemap_hook_info = NULL;

static volatile bool stop_loop = false;
static HANDLE capture_thread = NULL;
char system_path[MAX_PATH] = {0};
char process_name[MAX_PATH] = {0};
wchar_t keepalive_name[64] = {0};

static unsigned int shmem_id_counter = 0;
static void *shmem_info = NULL;
static HANDLE shmem_file_handle = 0;

volatile bool active = false;
struct hook_info *global_hook_info = NULL;

static inline void wait_for_dll_main_finish(HANDLE thread_handle)
{
	if (thread_handle) {
		WaitForSingleObject(thread_handle, 100);
		CloseHandle(thread_handle);
	}
}

static HANDLE init_event(const wchar_t *name, DWORD pid)
{
	HANDLE handle = create_event_plus_id(name, pid);
	if (!handle)
		hlog("Failed to get event '%s': %lu", name, GetLastError());
	return handle;
}

static HANDLE init_mutex(const wchar_t *name, DWORD pid)
{
	HANDLE handle = create_mutex_plus_id(name, pid);
	if (!handle)
		hlog("Failed to open mutex '%s': %lu", name, GetLastError());
	return handle;
}

static inline bool init_signals(void)
{
	DWORD pid = GetCurrentProcessId();

	signal_restart = init_event(EVENT_CAPTURE_RESTART, pid);
	if (!signal_restart) {
		return false;
	}

	signal_stop = init_event(EVENT_CAPTURE_STOP, pid);
	if (!signal_stop) {
		return false;
	}

	signal_ready = init_event(EVENT_HOOK_READY, pid);
	if (!signal_ready) {
		return false;
	}

	signal_exit = init_event(EVENT_HOOK_EXIT, pid);
	if (!signal_exit) {
		return false;
	}

	signal_init = init_event(EVENT_HOOK_INIT, pid);
	if (!signal_init) {
		return false;
	}

	return true;
}

static inline bool init_mutexes(void)
{
	DWORD pid = GetCurrentProcessId();

	audio_data_mutex = init_mutex(AUDIO_DATA_MUTEX, pid);
	if (!audio_data_mutex) {
		return false;
	}

	audio_data_event = init_event(AUDIO_DATA_EVENT, pid);
	if (!audio_data_event) {
		return false;
	}

	return true;
}

static inline bool init_system_path(void)
{
	UINT ret = GetSystemDirectoryA(system_path, MAX_PATH);
	if (!ret) {
		hlog("Failed to get windows system path: %lu", GetLastError());
		return false;
	}

	return true;
}

static inline void log_current_process(void)
{
	DWORD len = GetModuleBaseNameA(GetCurrentProcess(), NULL, process_name, MAX_PATH);
	if (len > 0) {
		process_name[len] = 0;
		hlog("Hooked to process: %s", process_name);
	}

	hlog("(half life scientist) everything..  seems to be in order");
}

static inline bool init_hook_info(void)
{
	filemap_hook_info = create_hook_info(GetCurrentProcessId());
	if (!filemap_hook_info) {
		hlog("Failed to create hook info file mapping: %lu", GetLastError());
		return false;
	}

	global_hook_info = MapViewOfFile(filemap_hook_info, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct hook_info));
	if (!global_hook_info) {
		hlog("Failed to map the hook info file mapping: %lu", GetLastError());
		return false;
	}

	return true;
}

static inline bool init_hook(HANDLE thread_handle)
{
	wait_for_dll_main_finish(thread_handle);

	_snwprintf(keepalive_name, sizeof(keepalive_name) / sizeof(wchar_t), L"%s%lu", WINDOW_HOOK_KEEPALIVE, GetCurrentProcessId());

	log_current_process();

	SetEvent(signal_restart);
	return true;
}

static inline void close_handle(HANDLE *handle)
{
	if (*handle) {
		CloseHandle(*handle);
		*handle = NULL;
	}
}

static void free_hook(void)
{
	if (filemap_hook_info) {
		CloseHandle(filemap_hook_info);
		filemap_hook_info = NULL;
	}
	if (global_hook_info) {
		UnmapViewOfFile(global_hook_info);
		global_hook_info = NULL;
	}

	close_handle(&audio_data_mutex);
	close_handle(&audio_data_event);
	close_handle(&signal_exit);
	close_handle(&signal_ready);
	close_handle(&signal_stop);
	close_handle(&signal_restart);
}

static inline bool attempt_hook(void)
{
	static bool wasapi_hooked = false;

	if (!wasapi_hooked) {
		wasapi_hooked = hook_wasapi();
		if (wasapi_hooked) {
			return true;
		}
	}

	return false;
}

static inline void capture_loop(void)
{
	WaitForSingleObject(signal_init, INFINITE);

	while (!attempt_hook())
		Sleep(40);

	for (size_t n = 0; !stop_loop; n++) {
		/* this causes it to check every 4 seconds, but still with
		 * a small sleep interval in case the thread needs to stop */
		if (n % 100 == 0)
			attempt_hook();
		Sleep(40);
	}
}

static DWORD WINAPI main_capture_thread(HANDLE thread_handle)
{
	if (!init_hook(thread_handle)) {
		DbgOut("Failed to init hook\n");
		free_hook();
		return 0;
	}

	capture_loop();
	return 0;
}

static inline void hlogv(const char *format, va_list args)
{
	char message[1024] = "";
	int num = _vsprintf_p(message, 1024, format, args);
	if (num) {
		DbgOut(message);
		DbgOut("\n");
	}
}

void hlog(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	hlogv(format, args);
	va_end(args);
}

void hlog_hr(const char *text, HRESULT hr)
{
	LPSTR buffer = NULL;

	FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, hr,
		       MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), (LPSTR)&buffer, 0, NULL);

	if (buffer) {
		hlog("%s (0x%08lX): %s", text, hr, buffer);
		LocalFree(buffer);
	} else {
		hlog("%s (0x%08lX)", text, hr);
	}
}

static inline uint64_t get_clockfreq(void)
{
	static bool have_clockfreq = false;
	static LARGE_INTEGER clock_freq;

	if (!have_clockfreq) {
		QueryPerformanceFrequency(&clock_freq);
		have_clockfreq = true;
	}

	return clock_freq.QuadPart;
}

uint64_t os_gettime_ns(void)
{
	LARGE_INTEGER current_time;
	double time_val;

	QueryPerformanceCounter(&current_time);
	time_val = (double)current_time.QuadPart;
	time_val *= 1000000000.0;
	time_val /= (double)get_clockfreq();

	return (uint64_t)time_val;
}

static inline bool init_shared_info(size_t size)
{
	wchar_t name[64];
	_snwprintf(name, 64, L"%s%u", SHMEM_AUDIO, ++shmem_id_counter);

	shmem_file_handle = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)size, name);
	if (!shmem_file_handle) {
		hlog("init_shared_info: Failed to create shared memory: %d", GetLastError());
		return false;
	}

	shmem_info = MapViewOfFile(shmem_file_handle, FILE_MAP_ALL_ACCESS, 0, 0, size);
	if (!shmem_info) {
		hlog("init_shared_info: Failed to map shared memory: %d", GetLastError());
		return false;
	}

	return true;
}

#ifndef ALIGN
#define ALIGN(bytes, align) (((bytes) + ((align)-1)) & ~((align)-1))
#endif

bool capture_init_shmem(struct shmem_data **data, uint8_t **data_pointer)
{
	uint32_t audio_size = 1024 * 1024;
	uint32_t aligned_header = ALIGN(sizeof(struct shmem_data), 32);
	uint32_t aligned_audio = ALIGN(audio_size, 32);
	uint32_t total_size = aligned_header + aligned_audio;
	uintptr_t align_pos;

	if (!init_shared_info(total_size)) {
		hlog("capture_init_shmem: Failed to initialize memory");
		return false;
	}

	*data = shmem_info;

	/* to ensure fast copy rate, align texture data to 256bit addresses */
	align_pos = (uintptr_t)shmem_info;
	align_pos += aligned_header;
	align_pos &= ~(32 - 1);
	align_pos -= (uintptr_t)shmem_info;

	if (align_pos < sizeof(struct shmem_data))
		align_pos += 32;

	(*data)->available_audio_size = 0;
	(*data)->audio_offset = align_pos;
	(*data)->buffer_size = aligned_audio;
	*data_pointer = (uint8_t *)shmem_info + align_pos;

	global_hook_info->map_id = shmem_id_counter;
	global_hook_info->map_size = total_size;

	if (!SetEvent(signal_ready)) {
		hlog("capture_init_shmem: Failed to signal ready: %d", GetLastError());
		return false;
	}

	active = true;
	return true;
}

void capture_free(void)
{
	if (shmem_info) {
		UnmapViewOfFile(shmem_info);
		shmem_info = NULL;
	}

	close_handle(&shmem_file_handle);

	SetEvent(signal_restart);
	active = false;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID unused1)
{
	if (reason == DLL_PROCESS_ATTACH) {
		wchar_t name[MAX_PATH];

		HANDLE cur_thread;
		bool success = DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &cur_thread, SYNCHRONIZE, false, 0);

		if (!success)
			DbgOut("Failed to get current thread handle");

		if (!init_signals()) {
			return false;
		}
		if (!init_system_path()) {
			return false;
		}
		if (!init_hook_info()) {
			return false;
		}
		if (!init_mutexes()) {
			return false;
		}

		/* this prevents the library from being automatically unloaded
		 * by the next FreeLibrary call */
		GetModuleFileNameW(hinst, name, MAX_PATH);
		LoadLibraryW(name);

		capture_thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)main_capture_thread, (LPVOID)cur_thread, 0, 0);
		if (!capture_thread) {
			CloseHandle(cur_thread);
			return false;
		}

	} else if (reason == DLL_PROCESS_DETACH) {
		if (capture_thread) {
			stop_loop = true;
			WaitForSingleObject(capture_thread, 300);
			CloseHandle(capture_thread);
		}

		free_hook();
	}

	(void)unused1;
	return true;
}

__declspec(dllexport) LRESULT CALLBACK dummy_debug_proc(int code, WPARAM wparam, LPARAM lparam)
{
	static bool hooking = true;
	MSG *msg = (MSG *)lparam;

	if (hooking && msg->message == (WM_USER + 432)) {
		HMODULE user32 = GetModuleHandleW(L"USER32");
		BOOL(WINAPI * unhook_windows_hook_ex)(HHOOK) = NULL;

		unhook_windows_hook_ex = (BOOL(WINAPI *)(HHOOK))ms_get_obfuscated_func(user32, "VojeleY`bdgxvM`hhDz", 0x7F55F80C9EE3A213ULL);

		if (unhook_windows_hook_ex)
			unhook_windows_hook_ex((HHOOK)msg->lParam);
		hooking = false;
	}

	return CallNextHookEx(0, code, wparam, lparam);
}
