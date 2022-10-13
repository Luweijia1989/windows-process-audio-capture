#ifndef _WASAPI_AUDIO_CAPTURE_PROXY_H_
#define _WASAPI_AUDIO_CAPTURE_PROXY_H_

#include <atomic>
#include <map>
#include <queue>
#include <mutex>

#include <Audioclient.h>
#include <mmdeviceapi.h>

class WASCaptureData;
class WASCaptureProxy {
public:
	static const int32_t kDefaultMaxOutputStreams = 128;
	static const int32_t kDefaultBytesPerBuffer = 8192;
	static const int32_t kDefaultMaxBytesPerBuffer = 20480;

public:
	typedef struct _audio_data_pool_t {
		IAudioRenderClient *render;
		std::queue<uint8_t *> data;
		//CRITICAL_SECTION		lock;
		_audio_data_pool_t(void) : render(nullptr) {}

		~_audio_data_pool_t(void) {}
	} audio_data_pool_t;

	WASCaptureProxy(void);
	virtual ~WASCaptureProxy(void);

	HRESULT initialize(void);
	void reset_data(void);
	HRESULT reset(int32_t bytes_per_buffer);

	void capture_audio(IAudioRenderClient *audio_render_client, uint32_t num_filled_bytes, int32_t block_align, bool slient);
	void mix_audio(uint8_t *buffer_dest, uint8_t *buffer_src, size_t totoal_frames);
	void mix_audio_sse2(uint8_t *buffer_dest, uint8_t *buffer_src, size_t totoal_frames);
	void mix_audio_avx2(uint8_t *buffer_dest, uint8_t *buffer_src, size_t totoal_frames);
	void push_audio_data(IAudioRenderClient *audio_render_client, BYTE **ppData);
	void pop_audio_data(IAudioRenderClient *audio_render_client);
	int output_stream_count() const { return _num_output_streams; }
	void on_audioclient_stopped(IAudioClient *audio_client, IAudioRenderClient *render_client, BOOL already_stopped);
	void on_renderclient_released();
	std::string AsHumanReadableString(const WAVEFORMATEX *format) const;
	void output_stream_added(IAudioRenderClient *audio_render_client);

	void set_audio_capture_proxy_receiver(WASCaptureData *obj);

protected:
	void SetMaxOutputStreamsAllowed(int32_t max) { _max_num_output_streams = max; }

private:
#ifdef DEBUG_AUDIO_CAPTURE
	// PCM Audio file
	FILE *fp_out_;
#endif
	WASCaptureData *_obj;

	// captured audio data buffer
	std::vector<uint8_t *> _audio_data;
	uint32_t _nbytes_per_buffer;
	int32_t _buffer_count;
	BOOL _avx2_support;
	BOOL _has_sub_buffer;

	std::mutex _lock;
	IAudioRenderClient *_first_render_client;

	// Max number of open output streams, modified by
	// SetMaxOutputStreamsAllowed().
	int32_t _max_num_output_streams;

	// Number of currently open output streams.
	std::atomic<int32_t> _num_output_streams;
	std::map<IAudioRenderClient *, audio_data_pool_t *> _render_clients;

	int32_t _caputred_cnt;
};

#endif
