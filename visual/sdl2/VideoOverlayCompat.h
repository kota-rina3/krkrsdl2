/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

/* VideoOverlayCompat.h - Win32 type shims for the non-Windows video overlay
 *
 * VideoOvlImpl.h/.cpp are derived from the Win32 sources of Kirikiri Z and
 * therefore mention a handful of Win32 types (BYTE / HWND / RECT / LONG_PTR /
 * ...). Windows builds get the real ones from <windows.h>. The SDL2 and
 * Linux/FFmpeg backends never touch a Win32 object - the movie is rendered into
 * layer buffers or an SDL texture - so the plain stand-ins below are enough to
 * compile the shared interface.
 */

#ifndef KRKRSDL2_VIDEO_OVERLAY_COMPAT_H
#define KRKRSDL2_VIDEO_OVERLAY_COMPAT_H

#if defined(_WIN32)

#include <windows.h>

#else /* !_WIN32 */

#include <cstddef>
#include <cstdint>

/* StorageImpl.h may already map these names onto the tjs types; only add the
 * ones that are still missing (a #define is visible to #ifdef, a typedef is
 * not - repeating an identical typedef is fine either way). */
#ifndef BYTE
typedef unsigned char BYTE;
#endif
#ifndef DWORD
typedef unsigned int DWORD;
#endif
#ifndef LONG
typedef long LONG;
#endif
#ifndef UINT
typedef unsigned int UINT;
#endif
#ifndef BOOL
typedef int BOOL;
#endif

/* Same stand-in as the one DrawDevice.h uses for the non-Windows HWND. */
typedef void *HWND;
typedef void *HDC;

#ifndef LONG_PTR
# if defined(_WIN64)
typedef long long LONG_PTR;
# else
typedef long LONG_PTR;
# endif
#endif

/* No non-Windows header in this tree defines RECT yet. */
struct RECT
{
	long left;
	long top;
	long right;
	long bottom;
};

/* iTVPVideoOverlay is the Win32 krmovie interface. The SDL2/FFmpeg backends use
 * their own players, but tTJSNI_VideoOverlay keeps a pointer member of this
 * type, so the name has to exist on every platform. */
class iTVPVideoOverlay;

#endif /* !_WIN32 */

#endif /* KRKRSDL2_VIDEO_OVERLAY_COMPAT_H */
