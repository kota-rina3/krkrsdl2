//---------------------------------------------------------------------------
/*
	SDL2 / ffmpeg based video overlay backend for kirikiri SDL2.

	This implements the iTVPVideoOverlay protocol used by
	tTJSNI_VideoOverlay, decoding via libavformat/libavcodec, presenting
	frames into the vomLayer double buffer and reporting playback events
	through the engine's NativeEventQueue (SDL custom events).
*/
//---------------------------------------------------------------------------
#ifndef __SDL2_VIDEO_OVERLAY_H__
#define __SDL2_VIDEO_OVERLAY_H__
//---------------------------------------------------------------------------
#include "VideoOverlayCompat.h"
#include "NativeEventQueue.h"
#include "tjsCommHead.h" // ttstr
//---------------------------------------------------------------------------
// creates the SDL2/ffmpeg video overlay player.
// queue  : engine event queue to post WM_GRAPHNOTIFY messages to
// localname  : OS local file path of the movie (ttstr)
// out    : receives the AddRef'ed overlay object; throws tjsError on failure
//---------------------------------------------------------------------------
void TVPGetSDL2VideoOverlayObject(NativeEventQueueImplement *queue,
	const ttstr &localname, iTVPVideoOverlay **out);
//---------------------------------------------------------------------------
#endif // __SDL2_VIDEO_OVERLAY_H__
