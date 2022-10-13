#include "wasapi_capture_proxy.h"
#include "wasapi-hook.h"
#include "wasapi_capturer.h"

// Default maximum number of output streams that can be open simultaneously
// for all platforms.

//#define DEBUG_AUDIO_CAPTURE
#ifdef DEBUG_AUDIO_CAPTURE
FILE *fp_out_ = nullptr;
#endif

namespace Util {
// write the decoded PCM to disk
void write_to_file(std::vector<uint8_t *> buffer_vec, FILE *fp, uint32_t bytes, bool has_sub)
{
	if (bytes > 0) {
		fwrite(buffer_vec[0], 1, bytes, fp);
		if (has_sub)
			fwrite(buffer_vec[1], 1, bytes, fp);
	}
	fflush(fp);
}

void run_cpuid(uint32_t eax, uint32_t ecx, uint32_t *abcd)
{
#if defined(_MSC_VER)
	__cpuidex((int *)abcd, eax, ecx);
#else
	uint32_t ebx, edx;
#if defined(__i386__) && defined(__PIC__)
	/* in case of PIC under 32-bit EBX cannot be clobbered */
	__asm__("movl %%ebx, %%edi \n\t cpuid \n\t xchgl %%ebx, %%edi"
		: "=D"(ebx),
#else
	__asm__("cpuid"
		: "+b"(ebx),
#endif
		  "+a"(eax), "+c"(ecx), "=d"(edx));
	abcd[0] = eax;
	abcd[1] = ebx;
	abcd[2] = ecx;
	abcd[3] = edx;
#endif
}

int check_xcr0_ymm()
{
	uint32_t xcr0;
#if defined(_MSC_VER)
	xcr0 = (uint32_t)_xgetbv(0); /* min VS2010 SP1 compiler is required */
#else
	__asm__("xgetbv" : "=a"(xcr0) : "c"(0) : "%edx");
#endif
	return ((xcr0 & 6) == 6); /* checking if xmm and ymm state are enabled in XCR0 */
}

int check_4th_gen_intel_core_features()
{
	uint32_t abcd[4];
	uint32_t fma_movbe_osxsave_mask = ((1 << 12) | (1 << 22) | (1 << 27));
	uint32_t avx2_bmi12_mask = (1 << 5) | (1 << 3) | (1 << 8);

	/* CPUID.(EAX=01H, ECX=0H):ECX.FMA[bit 12]==1   &&
		CPUID.(EAX=01H, ECX=0H):ECX.MOVBE[bit 22]==1 &&
		CPUID.(EAX=01H, ECX=0H):ECX.OSXSAVE[bit 27]==1 */
	run_cpuid(1, 0, abcd);
	if ((abcd[2] & fma_movbe_osxsave_mask) != fma_movbe_osxsave_mask)
		return 0;

	if (!check_xcr0_ymm())
		return 0;

	/*  CPUID.(EAX=07H, ECX=0H):EBX.AVX2[bit 5]==1  &&
		CPUID.(EAX=07H, ECX=0H):EBX.BMI1[bit 3]==1  &&
		CPUID.(EAX=07H, ECX=0H):EBX.BMI2[bit 8]==1  */
	run_cpuid(7, 0, abcd);
	if ((abcd[1] & avx2_bmi12_mask) != avx2_bmi12_mask)
		return 0;

	/* CPUID.(EAX=80000001H):ECX.LZCNT[bit 5]==1 */
	run_cpuid(0x80000001, 0, abcd);
	if ((abcd[2] & (1 << 5)) == 0)
		return 0;

	return 1;
}

static int can_use_intel_core_4th_gen_features()
{
	static int the_4th_gen_features_available = -1;
	/* test is performed once */
	if (the_4th_gen_features_available < 0)
		the_4th_gen_features_available = check_4th_gen_intel_core_features();

	return the_4th_gen_features_available;
}
}

WASCaptureProxy::WASCaptureProxy(void)
	: _obj(NULL),
	  _max_num_output_streams(kDefaultMaxOutputStreams),
	  _buffer_count(kDefaultMaxOutputStreams),
	  _nbytes_per_buffer(0),
	  _avx2_support(FALSE),
	  _has_sub_buffer(FALSE),
	  _first_render_client(nullptr),
	  _num_output_streams(1),
	  _caputred_cnt(0)
{
#ifdef DEBUG_AUDIO_CAPTURE
	time_t base_time = time(nullptr);
	struct tm base_date_local;

	localtime_s(&base_date_local, &base_time);

	fp_out_ = fopen("E:\\capture.pcm", "wb");
#endif

	_avx2_support = Util::can_use_intel_core_4th_gen_features() > 0 ? TRUE : FALSE;
	initialize();
}

WASCaptureProxy::~WASCaptureProxy(void)
{
#ifdef DEBUG_AUDIO_CAPTURE
	if (fp_out_)
		fclose(fp_out_);
#endif
	for (std::vector<uint8_t *>::iterator it = _audio_data.begin(); it != _audio_data.end(); ++it) {
		_aligned_free(*it);
	}
	_nbytes_per_buffer = 0;
}

HRESULT WASCaptureProxy::initialize(void)
{
	HRESULT hr = E_FAIL;
	if (_nbytes_per_buffer == 0) {
		size_t alignment = 32;
		if (!_avx2_support)
			alignment = 16;

		_nbytes_per_buffer = kDefaultBytesPerBuffer; // params.GetBytesPerBuffer();
		for (int i = 0; i < _buffer_count; i++) {
			uint8_t *ptr = (uint8_t *)_aligned_malloc(sizeof(uint8_t) * _nbytes_per_buffer, alignment);
			if (ptr) {
				_audio_data.push_back(ptr);
				if (((unsigned long long)ptr % alignment) == 0) {
					hlog("%s()_%d : captured_audio_data_ pointer=%p,  is aligned on =%d", __FUNCTION__, __LINE__, ptr, alignment);
					hr = S_OK;
				} else {
					hlog("%s()_%d : captured_audio_data_ pointer=%p,  is not aligned on =%d", __FUNCTION__, __LINE__, ptr, alignment);
					hr = S_FALSE;
				}
			} else {
				hlog("%s()_%d : Error allocation aligned memory.", __FUNCTION__, __LINE__);
				hr = E_OUTOFMEMORY;
			}
		}
	}
	return hr;
}
void WASCaptureProxy::reset_data(void)
{
	_first_render_client = NULL;
	_num_output_streams = 0;
	_has_sub_buffer = FALSE;
}

// To-Do: Do we need to increase buffer size?
//        _aligned_realloc causes access violation
HRESULT WASCaptureProxy::reset(int32_t bytes_per_buffer)
{
	HRESULT hr = S_OK;
	size_t alignment = 32;
	if (!_avx2_support)
		alignment = 16;

	if (bytes_per_buffer <= kDefaultBytesPerBuffer || bytes_per_buffer >= kDefaultMaxBytesPerBuffer)
		return E_FAIL;

	for (int i = 0; i < _buffer_count; i++) {
		uint8_t *ptr = _audio_data[i];
		ptr = (uint8_t *)_aligned_realloc(ptr, sizeof(uint8_t) * bytes_per_buffer, alignment);
		if (ptr) {
			_audio_data[i] = ptr;
			if (((unsigned long long)ptr % alignment) == 0) {
				hlog("%s()_%d : captured_audio_data_ pointer=%p,  is aligned on =%d", __FUNCTION__, __LINE__, ptr, alignment);
				hr = S_OK;
			} else {
				hlog("%s()_%d : captured_audio_data_ pointer=%p,  is not aligned on =%d", __FUNCTION__, __LINE__, ptr, alignment);
				hr = S_FALSE;
			}
		} else {
			hlog("%s()_%d : Error reallocation aligned memory.", __FUNCTION__, __LINE__);
			hr = E_OUTOFMEMORY;
		}
	}
	_nbytes_per_buffer = bytes_per_buffer;
	return hr;
}

void WASCaptureProxy::capture_audio(IAudioRenderClient *audio_render_client, uint32_t num_filled_bytes, int32_t block_align, bool slient)
{
	uint8_t *audio_data = nullptr;
	std::map<IAudioRenderClient *, audio_data_pool_t *>::iterator iter = _render_clients.find(audio_render_client);
	if (iter != _render_clients.end()) {
		WASCaptureProxy::audio_data_pool_t *pool = iter->second;
		if (pool) {
			audio_data = pool->data.front();
		}
	}

	uint32_t capture_audio_size = 0;
	if (!audio_data)
		return;

	if (!_nbytes_per_buffer) {
		hlog("%s()_%d :captured_bytes_per_buffer_", __FUNCTION__, __LINE__);
		return;
	}

	std::wostringstream wos;
	BOOL is_single_stream = FALSE;
	BOOL mix_done = FALSE;

	capture_audio_size = block_align * num_filled_bytes;

	if (capture_audio_size > _nbytes_per_buffer) {
		wos << "[" << GetCurrentThreadId() << "] capture_audio: "
		    << "num_filled_byte=" << capture_audio_size << " captured_bytes_per_buffer_=" << _nbytes_per_buffer << std::endl;
		hlog("%s()_%d : %s", __FUNCTION__, __LINE__, wos.str().c_str());
		if (!WASCaptureProxy::reset(capture_audio_size))
			return;
	}
	std::unique_lock<std::mutex> lock(_lock);
	//cv_.wait(lock, [this] { return processed_; });

	// ouput stream count increased, but the stream not yet started.
	// then we go on capturing audio data like single stream.
	if (_num_output_streams == 1) {
		is_single_stream = TRUE;
		_caputred_cnt = 1;
	} else {
		// increse cpatured output stream counter
		++_caputred_cnt;
	}

	if (_caputred_cnt == 1) {
		_first_render_client = audio_render_client;
		memset(_audio_data[0], 0, _nbytes_per_buffer);
		if (audio_data != nullptr && !slient)
			memcpy(_audio_data[0], audio_data, capture_audio_size);
	} else if (_caputred_cnt == 2) {
		// If other render thread comes, mix the audio data and wite to file.
		if (_first_render_client != audio_render_client) {
			mix_done = TRUE;
			wos << "[" << GetCurrentThreadId() << "] capture_audio: "
			    << "[first render=0x" << _first_render_client << "] _caputred_cnt=" << _caputred_cnt << " mix_done_=" << mix_done << std::endl;
			if (!slient)
				mix_audio(_audio_data[0], audio_data, capture_audio_size);
		}
		// If same render thread comes, write the audio data to sub buffer
		else {
			_has_sub_buffer = TRUE;
			wos << "[" << GetCurrentThreadId() << "] capture_audio: "
			    << "[first render=0x" << _first_render_client << "] _caputred_cnt=" << _caputred_cnt << " has_sub_buffer_=" << _has_sub_buffer
			    << std::endl;
			memset(_audio_data[1], 0, _nbytes_per_buffer);
			if (!slient)
				memcpy(_audio_data[1], audio_data, capture_audio_size);
		}
	}
	// g_caputred_cnt > 2 and couter was not reset
	else if (_first_render_client != audio_render_client) {
		if (_caputred_cnt == 3) {
			wos << "[" << GetCurrentThreadId() << "] capture_audio: "
			    << "[first render=0x" << _first_render_client << "] _caputred_cnt=" << _caputred_cnt << " mix_done_=" << mix_done << std::endl;
			if (!slient)
				mix_audio(_audio_data[0], audio_data, capture_audio_size);
		} else if (_caputred_cnt == 4) {
			mix_done = TRUE;
			wos << "[" << GetCurrentThreadId() << "] capture_audio: "
			    << "[first render=0x" << _first_render_client << "] _caputred_cnt=" << _caputred_cnt << " mix_done_=" << mix_done << std::endl;
			if (!slient)
				mix_audio(_audio_data[1], audio_data, capture_audio_size);
		}
	} else {
		// This audio packet is dropped
		mix_done = TRUE;
	}
	// If output stream is just one, then nothing to mix
	// Stopping render thread happens earlier than decresing output stream count.
	// Output stream count can be decresed when the stream is stopped.
	// Start render thread happens later than increasing output stream count.
	// Output stream count can be incresed when the stream is not yet started, so buffer is needed in that case.
	if (is_single_stream || mix_done) {
		/*
		wos << "[" << GetCurrentThreadId() << "] capture_audio: "
			<< "[first render=0x" << _first_render_client << "] _caputred_cnt=" << _caputred_cnt << "(file write)"
			<< " (is_single_stream=" << is_single_stream << " mix_done=" << mix_done << ")"
			<< std::endl;
		*/
#ifdef DEBUG_AUDIO_CAPTURE
		static int count = 0;
		hlog("###################### %d %ld", count++, capture_audio_size);
		// write the decoded PCM to disk
		_has_sub_buffer = 0;
		Util::write_to_file(_audio_data, fp_out_, capture_audio_size, _has_sub_buffer);
#endif

		if (_obj) {
			/*_obj->on_receive(_audio_data[0], capture_audio_size);
			if (_has_sub_buffer)
				_obj->on_receive(_audio_data[1], capture_audio_size);*/
		}

		// reset count
		_caputred_cnt = 0;
		_has_sub_buffer = FALSE;
	}
	//processed_ = TRUE;
}

void WASCaptureProxy::mix_audio(uint8_t *buffer_dest, uint8_t *buffer_src, size_t totoal_frames)
{
	if (_avx2_support)
		mix_audio_avx2(buffer_dest, buffer_src, totoal_frames);
	else
		mix_audio_sse2(buffer_dest, buffer_src, totoal_frames);
}

void WASCaptureProxy::mix_audio_sse2(uint8_t *buffer_dest, uint8_t *buffer_src, size_t totoal_frames)
{
	size_t frames_left = totoal_frames;
	uint8_t *dest_temp = buffer_dest;
	uint8_t *src_temp = buffer_src;

	size_t aligned_uint8s = totoal_frames & 0xFFFFFFF0;

	__m128i max_val = _mm_set1_epi8(UINT8_MAX);

	for (size_t i = 0; i < aligned_uint8s; i += 16) {
		__m128i *pos = (__m128i *)(dest_temp + i);
		__m128i *src = (__m128i *)(src_temp + i);
		__m128i mix;
		mix = _mm_add_epi8(_mm_loadu_si128(pos), _mm_loadu_si128(src));
		mix = _mm_min_epu8(mix, max_val);

		_mm_store_si128(pos, mix);
	}
	frames_left &= 0x1F;
	dest_temp += aligned_uint8s;
	src_temp += aligned_uint8s;

	if (frames_left) {
		for (size_t i = 0; i < frames_left; i++) {
			uint8_t val = dest_temp[i] + src_temp[i];

			if (val < 0)
				val = 0;
			else if (val > UINT8_MAX)
				val = UINT8_MAX;

			dest_temp[i] = val;
		}
	}
}

void WASCaptureProxy::mix_audio_avx2(uint8_t *buffer_dest, uint8_t *buffer_src, size_t totoal_frames)
{
	size_t frames_left = totoal_frames;
	uint8_t *dest_temp = buffer_dest;
	uint8_t *src_temp = buffer_src;

	size_t aligned_uint8s = totoal_frames & 0xFFFFFFE0;

	__m256i max_val = _mm256_set1_epi8(UINT8_MAX);

	for (size_t i = 0; i < aligned_uint8s; i += 32) {
		__m256i *pos = (__m256i *)(dest_temp + i);
		__m256i *src = (__m256i *)(src_temp + i);
		__m256i mix;
		mix = _mm256_add_epi8(_mm256_loadu_si256(pos), _mm256_loadu_si256(src));
		mix = _mm256_min_epu8(mix, max_val);

		_mm256_store_si256(pos, mix);
	}
	frames_left &= 0x1F;
	dest_temp += aligned_uint8s;
	src_temp += aligned_uint8s;

	if (frames_left) {
		for (size_t i = 0; i < frames_left; i++) {
			uint8_t val = dest_temp[i] + src_temp[i];

			if (val < 0)
				val = 0;
			else if (val > UINT8_MAX)
				val = UINT8_MAX;

			dest_temp[i] = val;
		}
	}
}

void WASCaptureProxy::push_audio_data(IAudioRenderClient *key, BYTE **ppdata)
{
	WASCaptureProxy::audio_data_pool_t *pool = nullptr;
	std::map<IAudioRenderClient *, audio_data_pool_t *>::iterator iter = _render_clients.find(key);
	if (iter == _render_clients.end()) {
		pool = new WASCaptureProxy::audio_data_pool_t();
		pool->render = key;
		_render_clients.insert(std::make_pair(key, pool));
	} else {
		pool = iter->second;
	}
	pool->data.push(*ppdata);
}

void WASCaptureProxy::pop_audio_data(IAudioRenderClient *key)
{
	std::map<IAudioRenderClient *, audio_data_pool_t *>::iterator iter = _render_clients.find(key);
	if (iter != _render_clients.end()) {
		WASCaptureProxy::audio_data_pool_t *pool = iter->second;
		if (pool)
			pool->data.pop();
	}
}

void WASCaptureProxy::output_stream_added(IAudioRenderClient *key)
{
	++_num_output_streams;

	//LOGGER::make_trace_log(ELASC, "%s()_%d : [%u] output_stream_added: render=0x%I64u, num_output_streams=%d", __FUNCTION__, __LINE__, ::GetCurrentThreadId(), key, _num_output_streams);

	if (_num_output_streams > _max_num_output_streams) {
		//LOGGER::make_trace_log(ELASC, "%s()_%d : [%u] set _num_output_streams=%d, num_output_streams=%d", __FUNCTION__, __LINE__, ::GetCurrentThreadId(), _max_num_output_streams, _num_output_streams);
		_num_output_streams = _max_num_output_streams;
	}
}

void WASCaptureProxy::set_audio_capture_proxy_receiver(WASCaptureData *obj)
{
	_obj = obj;
}

void WASCaptureProxy::on_audioclient_stopped(IAudioClient *audio_client, IAudioRenderClient *render_client, BOOL already_stopped)
{
	if (already_stopped) {
		BOOL b_found = FALSE;
		IAudioRenderClient *key = nullptr;
		WASCaptureProxy::audio_data_pool_t *pool = nullptr;
		for (std::map<IAudioRenderClient *, audio_data_pool_t *>::iterator it = _render_clients.begin(); it != _render_clients.end(); ++it) {
			key = it->first;
			pool = it->second;
			if (pool->render == render_client) {
				pool->render = nullptr;
				while (!pool->data.empty())
					pool->data.pop();
				b_found = TRUE;
				break;
			}
		}
		if (b_found) {
			_render_clients.erase(key);
			if (pool)
				delete pool;
		}
	}

	_obj->on_stop(audio_client, render_client);
}

void WASCaptureProxy::on_renderclient_released(void) {}

std::string WASCaptureProxy::AsHumanReadableString(const WAVEFORMATEX *p_format) const
{
	std::ostringstream s;
	s << "format=" << p_format->wFormatTag << " channels=" << p_format->nChannels << " sample_rate=" << p_format->nSamplesPerSec
	  << " bits_per_sample=" << p_format->wBitsPerSample << " block size of data=" << p_format->nBlockAlign
	  << " average bytes per sec=" << p_format->nAvgBytesPerSec;
	return s.str();
}
