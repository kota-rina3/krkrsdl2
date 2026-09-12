//---------------------------------------------------------------------------
/*
	SDL2 / ffmpeg based video overlay backend for kirikiri SDL2.
*/
//---------------------------------------------------------------------------
#include "tjsCommHead.h"
#include "SDL2VideoOverlay.h"
#include "MsgIntf.h"
#include "DebugIntf.h"
#include "StorageImpl.h"
#include <SDL.h>
#include <deque>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <algorithm>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTVPSDL2VideoOverlay
//---------------------------------------------------------------------------
class tTVPSDL2VideoOverlay : public iTVPVideoOverlay
{
	// ---- reference counting
	std::atomic<long> RefCount;

	// ---- engine event queue (not owned)
	NativeEventQueueImplement *Queue;

	// ---- ffmpeg objects
	AVFormatContext *FmtCtx;
	int VideoStreamIndex;
	int AudioStreamIndex;
	int SelectedAudioStream;
	AVCodecContext *VCodecCtx;
	AVCodecContext *ACodecCtx;
	SwsContext *SwsCtx;
	SwrContext *SwrCtx;

	// ---- media information
	long Width, Height;
	double Fps;
	tjs_int64 TotalTimeMs;
	int NumFrames;

	// ---- vomLayer double buffer (owned by tTJSNI_VideoOverlay)
	BYTE *Buffer[2];
	long BufferSize;
	int FrontBuffer;
	bool BufferSet;

	// ---- playback state
	std::mutex StateMutex;
	std::condition_variable StateCond;
	std::thread *Worker;
	std::atomic<bool> QuitFlag;
	std::atomic<int> Status;	// tTVPVideoStatus
	bool LoopMode;
	int StopFrame;
	bool StopFrameSet;
	double PlayRate;
	long AudioVolume;	// 0 .. 100000
	std::atomic<int> CurrentFrame;
	std::atomic<bool> CompleteFired;

	// ---- clock (media position in ms)
	double ClockBaseMs;		// media time at ClockBaseTicks
	Uint32 ClockBaseTicks;	// SDL_GetTicks at that moment
	bool ClockValid;

	// ---- packet queues
	std::deque<AVPacket*> VideoPackets;
	std::deque<AVPacket*> AudioPackets;

	// ---- audio output
	SDL_AudioDeviceID AudioDevice;
	bool AudioDeviceOpen;
	int AudioFreq;
	int AudioChannels;
	std::vector<uint8_t> AudioRing;
	size_t AudioRingRead;
	size_t AudioRingWrite;
	size_t AudioRingUsed;
	double AudioBytesPerMs;
	bool AudioEndOfStream;

	// ---- pending decoded video frame (fetched ahead, presented when due)
	AVFrame *PendingFrame;
	double PendingPtsMs;
	int PendingIndex;

	// ---- pending events for GetEvent()
	struct OverlayEvent
	{
		long Evc;
		LONG_PTR P1, P2;
	};
	std::mutex EventMutex;
	std::deque<OverlayEvent> EventQueue;

	// ---- misc
	long Balance;	// stored, not used
	HWND Window, DrainWindow;
	RECT DestRect;
	bool Visible;

private:
	void PostGraphNotify();
	void PushEvent(long evc, LONG_PTR p1, LONG_PTR p2);
	bool DemuxStep();
	bool DecodeVideoFrame(AVFrame *frame, int *outFrame, double *outPtsMs);
	bool DecodeAudio();
	void PresentFrame(AVFrame *frame, int frameIndex, double ptsMs);
	double GetClockMs();
	bool AudioEndedDrained();
	void ResetClock(double mediaMs);
	void WorkerProc();
	void SeekToMs(double ms);
	void ClearPacketQueues();
	void ResetAudioRing();
	void OpenAudioDevice();
	void CloseAudioDevice();
	static void AudioCallbackTrampoline(void *userdata, Uint8 *stream, int len);
	void AudioCallback(Uint8 *stream, int len);

public:
	tTVPSDL2VideoOverlay(NativeEventQueueImplement *queue, const ttstr &localname);
	virtual ~tTVPSDL2VideoOverlay();
	void ShutdownPlayer();

	// ---- iTVPVideoOverlay
	virtual void __stdcall AddRef() override
		{ RefCount++; }
	virtual void __stdcall Release() override
		{ if(--RefCount == 0) delete this; }

	virtual void __stdcall SetWindow(HWND window) override
		{ Window = window; }
	virtual void __stdcall SetMessageDrainWindow(HWND window) override
		{ DrainWindow = window; }
	virtual void __stdcall SetRect(RECT *rect) override
		{ if(rect) DestRect = *rect; }
	virtual void __stdcall SetVisible(bool b) override
		{ Visible = b; }
	virtual void __stdcall Play() override;
	virtual void __stdcall Stop() override;
	virtual void __stdcall Pause() override;
	virtual void __stdcall SetPosition(tjs_uint64 tick) override
		{ SeekToMs((double)tick); }
	virtual void __stdcall GetPosition(tjs_uint64 *tick) override
		{ if(tick) *tick = (tjs_uint64)GetClockMs(); }
	virtual void __stdcall GetStatus(tTVPVideoStatus *status) override
		{ if(status) *status = (tTVPVideoStatus)Status.load(); }
	virtual void __stdcall GetEvent(long *evcode, LONG_PTR *param1, LONG_PTR *param2, bool *got) override;
	virtual void __stdcall FreeEventParams(long evcode, LONG_PTR param1, LONG_PTR param2) override
		{ /* nothing to free */ }

	virtual void __stdcall Rewind() override
		{ SeekToMs(0.0); }
	virtual void __stdcall SetFrame( int f ) override
		{ if(Fps > 0.0) SeekToMs((double)f * 1000.0 / Fps); }
	virtual void __stdcall GetFrame( int *f ) override
		{ if(f) *f = CurrentFrame.load(); }
	virtual void __stdcall GetFPS( double *f ) override
		{ if(f) *f = Fps; }
	virtual void __stdcall GetNumberOfFrame( int *f ) override
		{ if(f) *f = NumFrames; }
	virtual void __stdcall GetTotalTime( tjs_int64 *t ) override
		{ if(t) *t = TotalTimeMs; }

	virtual void __stdcall GetVideoSize( long *width, long *height ) override
		{ if(width) *width = Width; if(height) *height = Height; }
	virtual void __stdcall GetFrontBuffer( BYTE **buff ) override
	{
		if(!buff) return;
		if(!BufferSet) { *buff = NULL; return; }
		*buff = Buffer[FrontBuffer];
	}
	virtual void __stdcall SetVideoBuffer( BYTE *buff1, BYTE *buff2, long size ) override
	{
		Buffer[0] = buff1; Buffer[1] = buff2;
		BufferSize = size; FrontBuffer = 0; BufferSet = true;
	}

	virtual void __stdcall SetStopFrame( int frame ) override
		{ StopFrame = frame; StopFrameSet = true; }
	virtual void __stdcall GetStopFrame( int *frame ) override
		{ if(frame) *frame = StopFrameSet ? StopFrame : -1; }
	virtual void __stdcall SetDefaultStopFrame() override
		{ StopFrameSet = false; StopFrame = -1; }

	virtual void __stdcall SetPlayRate( double rate ) override
		{ if(rate > 0.05 && rate < 50.0) PlayRate = rate; }
	virtual void __stdcall GetPlayRate( double *rate ) override
		{ if(rate) *rate = PlayRate; }

	virtual void __stdcall SetAudioBalance( long balance ) override
		{ Balance = balance; }
	virtual void __stdcall GetAudioBalance( long *balance ) override
		{ if(balance) *balance = Balance; }
	virtual void __stdcall SetAudioVolume( long volume ) override
		{ AudioVolume = volume < 0 ? 0 : (volume > 100000 ? 100000 : volume); }
	virtual void __stdcall GetAudioVolume( long *volume ) override
		{ if(volume) *volume = AudioVolume; }

	virtual void __stdcall GetNumberOfAudioStream( unsigned long *streamCount ) override;
	virtual void __stdcall SelectAudioStream( unsigned long num ) override;
	virtual void __stdcall GetEnableAudioStreamNum( long *num ) override
		{ if(num) *num = SelectedAudioStream; }
	virtual void __stdcall DisableAudioStream( void ) override
		{ /* not supported */ }

	virtual void __stdcall GetNumberOfVideoStream( unsigned long *streamCount ) override;
	virtual void __stdcall SelectVideoStream( unsigned long num ) override
		{ /* only the first video stream is supported */ }
	virtual void __stdcall GetEnableVideoStreamNum( long *num ) override
		{ if(num) *num = VideoStreamIndex; }

	virtual void __stdcall SetMixingBitmap( HDC hdc, RECT *dest, float alpha ) override
		{ /* not supported */ }
	virtual void __stdcall ResetMixingBitmap() override
		{ /* not supported */ }

	virtual void __stdcall SetMixingMovieAlpha( float a ) override
		{ /* not supported */ }
	virtual void __stdcall GetMixingMovieAlpha( float *a ) override
		{ if(a) *a = 1.0f; }
	virtual void __stdcall SetMixingMovieBGColor( unsigned long col ) override
		{ /* not supported */ }
	virtual void __stdcall GetMixingMovieBGColor( unsigned long *col ) override
		{ if(col) *col = 0; }

	virtual void __stdcall PresentVideoImage() override
		{ /* not supported */ }

	virtual void __stdcall GetContrastRangeMin( float *v ) override
		{ if(v) *v = 0.0f; }
	virtual void __stdcall GetContrastRangeMax( float *v ) override
		{ if(v) *v = 1.0f; }
	virtual void __stdcall GetContrastDefaultValue( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall GetContrastStepSize( float *v ) override
		{ if(v) *v = 0.1f; }
	virtual void __stdcall GetContrast( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall SetContrast( float v ) override
		{ /* not supported */ }

	virtual void __stdcall GetBrightnessRangeMin( float *v ) override
		{ if(v) *v = 0.0f; }
	virtual void __stdcall GetBrightnessRangeMax( float *v ) override
		{ if(v) *v = 1.0f; }
	virtual void __stdcall GetBrightnessDefaultValue( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall GetBrightnessStepSize( float *v ) override
		{ if(v) *v = 0.1f; }
	virtual void __stdcall GetBrightness( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall SetBrightness( float v ) override
		{ /* not supported */ }

	virtual void __stdcall GetHueRangeMin( float *v ) override
		{ if(v) *v = 0.0f; }
	virtual void __stdcall GetHueRangeMax( float *v ) override
		{ if(v) *v = 1.0f; }
	virtual void __stdcall GetHueDefaultValue( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall GetHueStepSize( float *v ) override
		{ if(v) *v = 0.1f; }
	virtual void __stdcall GetHue( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall SetHue( float v ) override
		{ /* not supported */ }

	virtual void __stdcall GetSaturationRangeMin( float *v ) override
		{ if(v) *v = 0.0f; }
	virtual void __stdcall GetSaturationRangeMax( float *v ) override
		{ if(v) *v = 1.0f; }
	virtual void __stdcall GetSaturationDefaultValue( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall GetSaturationStepSize( float *v ) override
		{ if(v) *v = 0.1f; }
	virtual void __stdcall GetSaturation( float *v ) override
		{ if(v) *v = 0.5f; }
	virtual void __stdcall SetSaturation( float v ) override
		{ /* not supported */ }
};
//---------------------------------------------------------------------------
// factory
//---------------------------------------------------------------------------
void TVPGetSDL2VideoOverlayObject(NativeEventQueueImplement *queue,
	const ttstr &localname, iTVPVideoOverlay **out)
{
	if(out == NULL) return;
	*out = new tTVPSDL2VideoOverlay(queue, localname);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// constructor / destructor
//---------------------------------------------------------------------------
tTVPSDL2VideoOverlay::tTVPSDL2VideoOverlay(NativeEventQueueImplement *queue,
	const ttstr &localname)
	: RefCount(1), Queue(queue),
	FmtCtx(NULL), VideoStreamIndex(-1), AudioStreamIndex(-1), SelectedAudioStream(-1),
	VCodecCtx(NULL), ACodecCtx(NULL), SwsCtx(NULL), SwrCtx(NULL),
	Width(0), Height(0), Fps(0.0), TotalTimeMs(0), NumFrames(0),
	Buffer{NULL, NULL}, BufferSize(0), FrontBuffer(0), BufferSet(false),
	QuitFlag(false), Status(vsStopped), LoopMode(false),
	StopFrame(-1), StopFrameSet(false), PlayRate(1.0), AudioVolume(100000),
	CurrentFrame(0), CompleteFired(false),
	PendingFrame(NULL), PendingPtsMs(0.0), PendingIndex(0),
	ClockBaseMs(0.0), ClockBaseTicks(0), ClockValid(false),
	AudioDevice(0), AudioDeviceOpen(false), AudioFreq(48000), AudioChannels(2),
	AudioRingRead(0), AudioRingWrite(0), AudioRingUsed(0), AudioBytesPerMs(0.0),
	AudioEndOfStream(false), Balance(0),
	Window(NULL), DrainWindow(NULL), Visible(false)
{
	std::string name = localname.AsNarrowStdString();
	int err = avformat_open_input(&FmtCtx, name.c_str(), NULL, NULL);
	if(err < 0)
	{
		char errbuf[256];
		av_strerror(err, errbuf, sizeof(errbuf));
		TVPThrowExceptionMessage(TJS_W("cannot open movie file: %1 (%2)"),
			ttstr(name.c_str()) + TJS_W(" (") + ttstr(errbuf) + TJS_W(")"));
	}
	if(avformat_find_stream_info(FmtCtx, NULL) < 0)
		TVPThrowExceptionMessage(TJS_W("cannot find stream information in movie"));

	// ---- find streams
	const AVCodec *vcodec = NULL, *acodec = NULL;
	VideoStreamIndex = av_find_best_stream(FmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, &vcodec, 0);
	AudioStreamIndex = av_find_best_stream(FmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &acodec, 0);

	if(VideoStreamIndex < 0 && AudioStreamIndex < 0)
		TVPThrowExceptionMessage(TJS_W("no audio/video stream in movie"));

	// ---- open video codec
	if(VideoStreamIndex >= 0)
	{
		AVStream *st = FmtCtx->streams[VideoStreamIndex];
		VCodecCtx = avcodec_alloc_context3(vcodec);
		avcodec_parameters_to_context(VCodecCtx, st->codecpar);
		VCodecCtx->thread_count = 0;	// auto
		if(avcodec_open2(VCodecCtx, vcodec, NULL) < 0)
			TVPThrowExceptionMessage(TJS_W("cannot open video codec"));

		Width = VCodecCtx->width;
		Height = VCodecCtx->height;
		AVRational fr = st->avg_frame_rate;
		if(fr.num <= 0 || fr.den <= 0) fr = st->r_frame_rate;
		if(fr.num > 0 && fr.den > 0) Fps = av_q2d(fr);
		if(Fps <= 0.0 || Fps > 1000.0) Fps = 30.0;

		if(FmtCtx->duration > 0)
			TotalTimeMs = (tjs_int64)(FmtCtx->duration * 1000.0 / AV_TIME_BASE);
		NumFrames = (int)(Fps * (TotalTimeMs / 1000.0) + 0.5);
		if(NumFrames <= 0) NumFrames = st->nb_frames > 0 ? (int)st->nb_frames : 0;
	}

	// ---- open audio codec
	if(AudioStreamIndex >= 0)
	{
		AVStream *st = FmtCtx->streams[AudioStreamIndex];
		ACodecCtx = avcodec_alloc_context3(acodec);
		avcodec_parameters_to_context(ACodecCtx, st->codecpar);
		if(avcodec_open2(ACodecCtx, acodec, NULL) < 0)
		{
			// audio is optional
			avcodec_free_context(&ACodecCtx);
			ACodecCtx = NULL;
			AudioStreamIndex = -1;
		}
		SelectedAudioStream = AudioStreamIndex;
	}

	// ---- swscale: decode to BGRA, bottom-up (krkr bitmap layout)
	if(VideoStreamIndex >= 0)
	{
		SwsCtx = sws_getContext(Width, Height, VCodecCtx->pix_fmt,
			Width, Height, AV_PIX_FMT_BGRA,
			SWS_BICUBIC, NULL, NULL, NULL);
		if(SwsCtx == NULL)
			TVPThrowExceptionMessage(TJS_W("cannot create video scaler"));
	}

	// ---- audio output configuration (opened on Play)
	AudioBytesPerMs = (double)(AudioFreq * AudioChannels * 2) / 1000.0;
	AudioRing.resize((size_t)(AudioBytesPerMs * 2000));	// 2 second ring
}
//---------------------------------------------------------------------------
tTVPSDL2VideoOverlay::~tTVPSDL2VideoOverlay()
{
	ShutdownPlayer();

	if(SwsCtx) { sws_freeContext(SwsCtx); SwsCtx = NULL; }
	if(SwrCtx) { swr_free(&SwrCtx); SwrCtx = NULL; }
	if(VCodecCtx) avcodec_free_context(&VCodecCtx);
	if(ACodecCtx) avcodec_free_context(&ACodecCtx);
	if(FmtCtx) avformat_close_input(&FmtCtx);
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::ShutdownPlayer()
{
	QuitFlag = true;
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		StateCond.notify_all();
	}
	if(Worker && Worker->joinable()) Worker->join();
	delete Worker; Worker = NULL;
	CloseAudioDevice();
	ClearPacketQueues();
	ResetAudioRing();
	if(PendingFrame)
	{
		av_frame_free(&PendingFrame);
		PendingFrame = NULL;
	}
	Status = vsStopped;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// events
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::PushEvent(long evc, LONG_PTR p1, LONG_PTR p2)
{
	{
		std::lock_guard<std::mutex> lk(EventMutex);
		OverlayEvent ev;
		ev.Evc = evc; ev.P1 = p1; ev.P2 = p2;
		EventQueue.push_back(ev);
	}
	PostGraphNotify();
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::PostGraphNotify()
{
	if(Queue)
	{
		NativeEvent ev(WM_GRAPHNOTIFY);
		Queue->PostEvent(ev);
	}
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::GetEvent(long *evcode, LONG_PTR *param1,
		LONG_PTR *param2, bool *got)
{
	if(!evcode || !param1 || !param2 || !got) return;
	*got = false;
	std::lock_guard<std::mutex> lk(EventMutex);
	if(EventQueue.empty()) return;
	OverlayEvent ev = EventQueue.front();
	EventQueue.pop_front();
	*evcode = ev.Evc; *param1 = ev.P1; *param2 = ev.P2;
	*got = true;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// clock
//---------------------------------------------------------------------------
double tTVPSDL2VideoOverlay::GetClockMs()
{
	std::lock_guard<std::mutex> lk(StateMutex);
	if(!ClockValid) return ClockBaseMs;
	double elapsed = (double)(SDL_GetTicks() - ClockBaseTicks) * PlayRate;
	return ClockBaseMs + elapsed;
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::ResetClock(double mediaMs)
{
	// caller must hold StateMutex
	ClockBaseMs = mediaMs;
	ClockBaseTicks = SDL_GetTicks();
	ClockValid = true;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// packet queues
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::ClearPacketQueues()
{
	for(auto &p : VideoPackets) av_packet_free(&p);
	VideoPackets.clear();
	for(auto &p : AudioPackets) av_packet_free(&p);
	AudioPackets.clear();
}
//---------------------------------------------------------------------------
bool tTVPSDL2VideoOverlay::DemuxStep()
{
	AVPacket *pkt = av_packet_alloc();
	int err;
	while((err = av_read_frame(FmtCtx, pkt)) >= 0)
	{
		if(pkt->stream_index == VideoStreamIndex)
		{
			VideoPackets.push_back(pkt);
			return true;
		}
		else if(pkt->stream_index == AudioStreamIndex)
		{
			AudioPackets.push_back(pkt);
			return true;
		}
		av_packet_free(&pkt);
		pkt = av_packet_alloc();
	}
	if(pkt) av_packet_free(&pkt);
	if(err < 0 && err != AVERROR_EOF && err != AVERROR(EAGAIN))
	{
		char errbuf[256];
		av_strerror(err, errbuf, sizeof(errbuf));
		TVPAddLog(ttstr(TJS_W("movie demux error: ")) + ttstr(errbuf));
	}
	return false;	// EOF or error
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// audio
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::ResetAudioRing()
{
	// caller must hold StateMutex (or be in single-threaded context)
	AudioRingRead = AudioRingWrite = AudioRingUsed = 0;
	AudioEndOfStream = false;
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::OpenAudioDevice()
{
	if(AudioDeviceOpen || AudioStreamIndex < 0) return;

	SDL_AudioSpec desired, obtained;
	SDL_memset(&desired, 0, sizeof(desired));
	SDL_memset(&obtained, 0, sizeof(obtained));
	desired.freq = AudioFreq;
	desired.format = AUDIO_S16SYS;
	desired.channels = AudioChannels;
	desired.samples = 2048;
	desired.callback = tTVPSDL2VideoOverlay::AudioCallbackTrampoline;
	desired.userdata = this;

	AudioDevice = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
	if(AudioDevice == 0) return;
	AudioFreq = obtained.freq;
	AudioChannels = obtained.channels;
	AudioBytesPerMs = (double)(AudioFreq * AudioChannels * 2) / 1000.0;
	AudioRing.resize((size_t)(AudioBytesPerMs * 2000));
	AudioRingRead = AudioRingWrite = AudioRingUsed = 0;
	AudioDeviceOpen = true;

	// ---- swresample: source codec layout -> S16 stereo @ AudioFreq
	if(ACodecCtx)
	{
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
		AVChannelLayout out_layout = AV_CHANNEL_LAYOUT_STEREO;
		swr_alloc_set_opts2(&SwrCtx,
			&out_layout, AV_SAMPLE_FMT_S16, AudioFreq,
			&ACodecCtx->ch_layout, ACodecCtx->sample_fmt, ACodecCtx->sample_rate,
			0, NULL);
#else
		int64_t out_layout = AV_CH_LAYOUT_STEREO;
		SwrCtx = swr_alloc_set_opts(SwrCtx,
			out_layout, AV_SAMPLE_FMT_S16, AudioFreq,
			ACodecCtx->channel_layout ? (int64_t)ACodecCtx->channel_layout :
				av_get_default_channel_layout(ACodecCtx->channels),
			ACodecCtx->sample_fmt, ACodecCtx->sample_rate,
			0, NULL);
#endif
		if(SwrCtx) swr_init(SwrCtx);
	}
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::CloseAudioDevice()
{
	if(AudioDeviceOpen)
	{
		SDL_CloseAudioDevice(AudioDevice);
		AudioDevice = 0;
		AudioDeviceOpen = false;
	}
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::AudioCallbackTrampoline(void *userdata, Uint8 *stream, int len)
{
	static_cast<tTVPSDL2VideoOverlay*>(userdata)->AudioCallback(stream, len);
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::AudioCallback(Uint8 *stream, int len)
{
	std::lock_guard<std::mutex> lk(StateMutex);
	size_t todo = (size_t)len;
	if(AudioRingUsed < todo)
	{
		if(AudioRingUsed > 0)
		{
			for(size_t i = 0; i < AudioRingUsed; i++)
				stream[i] = AudioRing[AudioRingRead + i];
			AudioRingRead = (AudioRingRead + AudioRingUsed) % AudioRing.size();
			AudioRingUsed = 0;
		}
		SDL_memset(stream + AudioRingUsed, 0, len - (int)AudioRingUsed);
		return;
	}
	for(size_t i = 0; i < todo; i++)
		stream[i] = AudioRing[AudioRingRead + i];
	AudioRingRead = (AudioRingRead + todo) % AudioRing.size();
	AudioRingUsed -= todo;
}
//---------------------------------------------------------------------------
bool tTVPSDL2VideoOverlay::DecodeAudio()
{
	// decode one audio frame into the ring buffer; false when no more data now
	if(!ACodecCtx || AudioStreamIndex < 0) return false;
	if(SwrCtx == NULL) return false;

	std::deque<AVPacket*> local;
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		if(AudioPackets.empty()) return false;
		local.swap(AudioPackets);
	}

	bool gotAny = false;
	AVFrame *frame = av_frame_alloc();
	for(auto it = local.begin(); it != local.end(); ++it)
	{
		AVPacket *pkt = *it;
		if(avcodec_send_packet(ACodecCtx, pkt) >= 0)
		{
			while(true)
			{
				int err = avcodec_receive_frame(ACodecCtx, frame);
				if(err < 0) break;
				// convert
				int outBytesPerSample = 2;
#if LIBAVUTIL_VERSION_INT >= AV_VERSION_INT(57, 28, 100)
				int outChannels = 2;
				int maxOut = (int)AudioRing.size();
				uint8_t *outBuf = new uint8_t[maxOut];
				uint8_t *outPtr[1] = { outBuf };
				int got = swr_convert(SwrCtx, outPtr, maxOut / (outBytesPerSample * outChannels),
					(const uint8_t**)frame->extended_data, frame->nb_samples);
				if(got > 0)
				{
					size_t bytes = (size_t)got * outBytesPerSample * outChannels;
					std::lock_guard<std::mutex> lk(StateMutex);
					for(size_t i = 0; i < bytes; i++)
					{
						AudioRing[AudioRingWrite] = outBuf[i];
						AudioRingWrite = (AudioRingWrite + 1) % AudioRing.size();
					}
					AudioRingUsed += bytes;
					gotAny = true;
				}
				delete[] outBuf;
#else
				int maxOut = (int)AudioRing.size();
				uint8_t *outBuf = new uint8_t[maxOut];
				uint8_t *outPtr[1] = { outBuf };
				int got = swr_convert(SwrCtx, outPtr, maxOut / (outBytesPerSample * 2),
					(const uint8_t**)frame->extended_data, frame->nb_samples);
				if(got > 0)
				{
					size_t bytes = (size_t)got * outBytesPerSample * 2;
					std::lock_guard<std::mutex> lk(StateMutex);
					for(size_t i = 0; i < bytes; i++)
					{
						AudioRing[AudioRingWrite] = outBuf[i];
						AudioRingWrite = (AudioRingWrite + 1) % AudioRing.size();
					}
					AudioRingUsed += bytes;
					gotAny = true;
				}
				delete[] outBuf;
#endif
			}
		}
		av_packet_free(&pkt);
	}
	av_frame_free(&frame);
	return gotAny;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// video
//---------------------------------------------------------------------------
bool tTVPSDL2VideoOverlay::DecodeVideoFrame(AVFrame *frame, int *outFrame, double *outPtsMs)
{
	if(!VCodecCtx || VideoStreamIndex < 0) return false;

	AVStream *st = FmtCtx->streams[VideoStreamIndex];
	AVPacket *pkt = NULL;
	for(;;)
	{
		// pull a decoded frame if available
		int err = avcodec_receive_frame(VCodecCtx, frame);
		if(err >= 0)
		{
			double pts = frame->best_effort_timestamp * av_q2d(st->time_base) * 1000.0;
			if(outPtsMs) *outPtsMs = pts;
			if(outFrame)
			{
				int f = (int)(pts * Fps / 1000.0 + 0.5);
				*outFrame = f;
			}
			return true;
		}
		if(err == AVERROR_EOF) return false;

		// need more input: demux until a video packet arrives
		if(!DemuxStep())
		{
			// flush decoder
			avcodec_send_packet(VCodecCtx, NULL);
			continue;
		}
		{
			std::lock_guard<std::mutex> lk(StateMutex);
			if(VideoPackets.empty()) continue;
			pkt = VideoPackets.front();
			VideoPackets.pop_front();
		}
		avcodec_send_packet(VCodecCtx, pkt);	// refcounted; free our ref either way
		av_packet_free(&pkt);
	}
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::PresentFrame(AVFrame *frame, int frameIndex, double ptsMs)
{
	if(!BufferSet || !SwsCtx) return;

	int back = 1 - FrontBuffer;
	uint8_t *dstBase = Buffer[back];
	if(dstBase == NULL) return;
	long stride = Width * 4;
	// krkr bitmaps are bottom-up: GetScanLine(height-1) is the buffer start
	uint8_t *dst = dstBase + (Height - 1) * (long)stride;
	uint8_t *dstData[4] = { dst, NULL, NULL, NULL };
	int dstLinesize[4] = { -(int)stride, 0, 0, 0 };
	sws_scale(SwsCtx, frame->data, frame->linesize, 0, Height,
		dstData, dstLinesize);

	FrontBuffer = back;
	CurrentFrame = frameIndex;
	PushEvent(EC_UPDATE, (LONG_PTR)frameIndex, 0);
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
bool tTVPSDL2VideoOverlay::AudioEndedDrained()
{
	if(AudioStreamIndex < 0) return true;
	std::lock_guard<std::mutex> lk(StateMutex);
	return AudioEndOfStream && AudioRingUsed == 0;
}
//---------------------------------------------------------------------------
// seek
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::SeekToMs(double ms)
{
	std::lock_guard<std::mutex> lk(StateMutex);

	bool wasPlaying = (Status.load() == vsPlaying);

	// clear pending events (stale EC_UPDATE/EC_COMPLETE)
	{
		std::lock_guard<std::mutex> elk(EventMutex);
		EventQueue.clear();
	}
	ClearPacketQueues();
	ResetAudioRing();
	if(PendingFrame)
	{
		av_frame_free(&PendingFrame);
		PendingFrame = NULL;
	}
	CompleteFired = false;

	if(VCodecCtx) avcodec_flush_buffers(VCodecCtx);
	if(ACodecCtx) avcodec_flush_buffers(ACodecCtx);

	if(FmtCtx)
	{
		int64_t target = (int64_t)(ms * AV_TIME_BASE / 1000.0);
		avformat_seek_file(FmtCtx, -1, INT64_MIN, target, INT64_MAX, 0);
	}

	CurrentFrame = (int)(ms * Fps / 1000.0 + 0.5);
	ResetClock(ms);
	StateCond.notify_all();
	(void)wasPlaying;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// worker thread
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::WorkerProc()
{
	while(!QuitFlag)
	{
		{
			std::unique_lock<std::mutex> lk(StateMutex);
			if(QuitFlag) return;
			if(Status.load() != vsPlaying)
			{
				StateCond.wait(lk, [this]{ return QuitFlag.load() || Status.load() == vsPlaying; });
				if(QuitFlag) return;
				// resync clock at resume
				ResetClock(ClockBaseMs);
			}
		}

		// ---- keep the audio ring buffer filled
		if(AudioStreamIndex >= 0 && !AudioEndOfStream)
		{
			bool decoded = false;
			{
				std::lock_guard<std::mutex> lk(StateMutex);
				if(!AudioPackets.empty()) decoded = true;
			}
			if(decoded)
			{
				DecodeAudio();
			}
			else if(!DemuxStep())
			{
				// demux hit EOF and nothing left to decode for now;
				// mark EOS only once the ring has drained
				std::lock_guard<std::mutex> lk(StateMutex);
				AudioEndOfStream = true;
			}
		}

		// ---- video frame pacing
		if(VideoStreamIndex >= 0 && BufferSet)
		{
			// fetch the next frame when none is pending
			if(!PendingFrame)
			{
				PendingFrame = av_frame_alloc();
				int fidx = 0; double pts = 0.0;
				if(DecodeVideoFrame(PendingFrame, &fidx, &pts))
				{
					PendingPtsMs = pts;
					PendingIndex = fidx;
				}
				else
				{
					av_frame_free(&PendingFrame);
					PendingFrame = NULL;
					// no more video frames: wait for audio to end or EOS
					if(AudioEndedDrained())
					{
						if(!CompleteFired.exchange(true))
							PushEvent(EC_COMPLETE, 0, 0);
						Status = vsEnded;
						SDL_Delay(30);
					}
					else SDL_Delay(5);
					continue;
				}
			}

			double clock = GetClockMs();
			if(PendingPtsMs <= clock + 15.0)	// 15ms scheduling slack
			{
				PresentFrame(PendingFrame, PendingIndex, PendingPtsMs);
				av_frame_free(&PendingFrame);
				PendingFrame = NULL;

				// ---- stop frame support
				if(StopFrameSet && CurrentFrame.load() >= StopFrame)
				{
					if(!CompleteFired.exchange(true))
						PushEvent(EC_COMPLETE, 0, 0);
					Status = vsEnded;
					continue;
				}
			}
			else
			{
				// sleep until due (bounded)
				double due = PendingPtsMs - GetClockMs();
				if(due > 2.0) SDL_Delay((Uint32)std::min(due - 1.0, 40.0));
				else SDL_Delay(1);
			}
		}
		else
		{
			// audio only
			if(AudioStreamIndex < 0)
			{
				if(!CompleteFired.exchange(true))
					PushEvent(EC_COMPLETE, 0, 0);
				Status = vsEnded;
				SDL_Delay(30);
			}
			else if(AudioEndedDrained())
			{
				if(!CompleteFired.exchange(true))
					PushEvent(EC_COMPLETE, 0, 0);
				Status = vsEnded;
				SDL_Delay(30);
			}
			else SDL_Delay(10);
		}
	}
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// playback control
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::Play()
{
	if(Status.load() == vsPlaying) return;
	QuitFlag = false;

	// (re)start from the beginning after full completion
	if(Status.load() == vsEnded)
		SeekToMs(0.0);

	OpenAudioDevice();
	if(AudioDeviceOpen)
		SDL_PauseAudioDevice(AudioDevice, 0);

	{
		std::lock_guard<std::mutex> lk(StateMutex);
		ResetClock(ClockBaseMs);	// resync timeline to "now"
		Status = vsPlaying;
		StateCond.notify_all();
	}

	if(!Worker)
	{
		Worker = new std::thread(&tTVPSDL2VideoOverlay::WorkerProc, this);
	}
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::Stop()
{
	ShutdownPlayer();
	SeekToMs(0.0);
	Status = vsStopped;
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::Pause()
{
	if(Status.load() == vsPlaying)
	{
		std::lock_guard<std::mutex> lk(StateMutex);
		// freeze clock at the current position
		double now = ClockBaseMs + (double)(SDL_GetTicks() - ClockBaseTicks) * PlayRate;
		ClockBaseMs = now;
		ClockBaseTicks = SDL_GetTicks();
		ClockValid = false;
		Status = vsPaused;
	}
	if(AudioDeviceOpen)
		SDL_PauseAudioDevice(AudioDevice, 1);
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::GetNumberOfAudioStream(unsigned long *streamCount)
{
	if(!streamCount) return;
	unsigned long count = 0;
	for(unsigned i = 0; i < FmtCtx->nb_streams; i++)
		if(FmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
			count++;
	*streamCount = count;
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::SelectAudioStream(unsigned long num)
{
	// switching on the fly is not supported; only remember the selection
	unsigned long count = 0;
	GetNumberOfAudioStream(&count);
	if(num < count) SelectedAudioStream = (long)num;
}
//---------------------------------------------------------------------------
void tTVPSDL2VideoOverlay::GetNumberOfVideoStream(unsigned long *streamCount)
{
	if(!streamCount) return;
	unsigned long count = 0;
	for(unsigned i = 0; i < FmtCtx->nb_streams; i++)
		if(FmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
			count++;
	*streamCount = count;
}
//---------------------------------------------------------------------------
