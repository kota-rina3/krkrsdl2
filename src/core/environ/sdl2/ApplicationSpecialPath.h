/* SPDX-License-Identifier: MIT */
/* Copyright (c) Kirikiri SDL2 Developers */

#ifndef __APPLICATION_SPECIAL_PATH_H__
#define __APPLICATION_SPECIAL_PATH_H__

#ifdef _WIN32
#include <shlobj.h>
#endif
#include "FilePathUtil.h"
#include "StorageIntf.h"
#include "CharacterSet.h"
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>


class ApplicationSpecialPath {
public:
#ifdef _WIN32
	static tjs_string GetSpecialFolderPath(int csidl) {
		tjs_char path[MAX_PATH+1];
		if(!SHGetSpecialFolderPath(NULL, path, csidl, false))
			return tjs_string();
		return tjs_string(path);
	}
	static inline tjs_string GetPersonalPath() {
		tjs_string path = GetSpecialFolderPath(CSIDL_PERSONAL);
		if( path.empty() ) path = GetSpecialFolderPath(CSIDL_APPDATA);

		if(path != TJS_W("")) {
			return path;
		}
		return TJS_W("");
	}
	static inline tjs_string GetAppDataPath() {
		tjs_string path = GetSpecialFolderPath(CSIDL_APPDATA);
		if(path != TJS_W("") ) {
			return path;
		}
		return TJS_W("");
	}
	static inline tjs_string GetSavedGamesPath() {
		tjs_string result;
		PWSTR ppszPath = NULL;
		HRESULT hr = ::SHGetKnownFolderPath(FOLDERID_SavedGames, 0, NULL, &ppszPath);
		if( hr == S_OK ) {
			result = tjs_string( ppszPath );
			::CoTaskMemFree( ppszPath );
		}
		return result;
	}
#endif
#if 1
	static inline tjs_string ReplaceStringAll( tjs_string src, const tjs_string& target, const tjs_string& dest ) {
		tjs_string::size_type nPos = 0;
		while( (nPos = src.find(target, nPos)) != tjs_string::npos ) {
			src.replace( nPos, target.length(), dest );
		}
		return src;
	}
#endif

#if defined(__linux__)
	/* True when 'utf8dir' is (or can be made into) a writable directory.
	   Used to decide whether the game directory may hold the saves, or
	   whether we have to fall back to the per-user XDG directory (a package
	   installed under /opt or inside a read-only image cannot be written). */
	static bool CanUseAsDataPath( const std::string& utf8dir ) {
		struct stat dir_stat;
		if( stat( utf8dir.c_str(), &dir_stat ) == 0 ) {
			return S_ISDIR( dir_stat.st_mode ) && access( utf8dir.c_str(), W_OK ) == 0;
		}
		/* create the whole directory tree */
		std::string cur;
		cur.reserve( utf8dir.size() );
		for( std::string::size_type i = 0; i < utf8dir.size(); ++i ) {
			cur += utf8dir[i];
			if( utf8dir[i] == '/' ) {
				if( cur.size() > 1 && mkdir( cur.c_str(), 0777 ) != 0 && errno != EEXIST )
					return false;
			}
		}
		if( mkdir( utf8dir.c_str(), 0777 ) != 0 && errno != EEXIST )
			return false;
		return access( utf8dir.c_str(), W_OK ) == 0;
	}
#endif

	static inline tjs_string GetConfigFileName( const tjs_string& exename ) {
		return ChangeFileExt(exename, TJS_W(".cf"));
	}
	static tjs_string GetDataPathDirectory( tjs_string datapath, const tjs_string& exename ) {
#ifdef _WIN32
		if(datapath == TJS_W("") ) datapath = tjs_string(TJS_W("$(exepath)\\savedata"));

		tjs_string exepath = ExcludeTrailingSlash(ExtractFileDir(exename));
		tjs_string personalpath = ExcludeTrailingSlash(GetPersonalPath());
		tjs_string appdatapath = ExcludeTrailingSlash(GetAppDataPath());
		tjs_string savedgamespath = ExcludeTrailingSlash(GetSavedGamesPath());
		if(personalpath == TJS_W("")) personalpath = exepath;
		if(appdatapath == TJS_W("")) appdatapath = exepath;
		if(savedgamespath == TJS_W("")) savedgamespath = exepath;

		datapath = ReplaceStringAll(datapath, TJS_W("$(exepath)"), exepath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(personalpath)"), personalpath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(appdatapath)"), appdatapath);
		datapath = ReplaceStringAll(datapath, TJS_W("$(vistapath)"), appdatapath );
		datapath = ReplaceStringAll(datapath, TJS_W("$(savedgamespath)"), savedgamespath);
		return IncludeTrailingBackslash(ExpandUNCFileName(datapath));
#else
		if (datapath != TJS_W(""))
		{
			return datapath;
		}
#if defined(__linux__)
/* Namespace for the per-user XDG fallback below. Defaults to the same name
   SDL's preference path fallback uses further down; packagers can override it
   with -DKRKRSDL2_LINUX_APP_ID=... */
#ifndef KRKRSDL2_LINUX_APP_ID
#define KRKRSDL2_LINUX_APP_ID "krkrsdl2"
#endif
		/* Linux default: keep the saves inside the game directory, right next
		   to the executable (<exe-dir>/savedata). This mirrors the Windows
		   default ("$(exepath)\savedata") and makes the port portable: the
		   whole game folder can be copied around with its saves intact.

		   Precedence:
		     1. "-datapath=..." on the command line (handled by the caller,
		        it wins over everything here),
		     2. the KRKR_LINUX_SAVE_DIR environment variable,
		     3. the game directory, when it is writable,
		     4. the XDG data directory (fallback for read-only game dirs,
		        e.g. a package installed under /opt or a read-only image). */
		{
			const char *override_dir = getenv("KRKR_LINUX_SAVE_DIR");
			if (override_dir && *override_dir)
			{
				std::string override_utf8 = override_dir;
				tjs_string override_path;
				if (TVPUtf8ToUtf16(override_path, override_utf8))
				{
					return IncludeTrailingSlash(override_path);
				}
			}

			tjs_string exedir = ExcludeTrailingSlash(ExtractFileDir(exename));
			if (!exedir.empty())
			{
				tjs_string game_dir = IncludeTrailingSlash(exedir + TJS_W("/savedata"));
				std::string game_dir_utf8;
				if (TVPUtf16ToUtf8(game_dir_utf8, game_dir) &&
					CanUseAsDataPath(game_dir_utf8))
				{
					return game_dir;
				}
			}
		}
		/* XDG fallback: keeps saves outside a read-only application payload.
		   The XDG data directory is the freedesktop standard location, and
		   linglong maps the host's ~/.local/share into the container, so
		   saves survive application updates and stay editable by the user. */
		{
			const char *xdg_data_home = getenv("XDG_DATA_HOME");
			std::string save_base;
			if (xdg_data_home && *xdg_data_home)
			{
				save_base = xdg_data_home;
			}
			else
			{
				const char *home = getenv("HOME");
				if (home && *home)
				{
					save_base = std::string(home) + "/.local/share";
				}
			}
			if (!save_base.empty())
			{
				if (save_base[save_base.length() - 1] != '/')
				{
					save_base += '/';
				}
				/* <app-id>/savedata, mirroring the layout the other platforms
				 * use (iOS keeps its files in Documents/<bundle-id>/savedata):
				 * the game's own data lives in a subdirectory of the
				 * application folder instead of being mixed with unrelated
				 * per-user state. */
				save_base += KRKRSDL2_LINUX_APP_ID;
				save_base += "/savedata";
			}
			tjs_string save_path;
			if (!save_base.empty() && TVPUtf8ToUtf16(save_path, save_base))
			{
				if (save_path.length() > 0 && save_path[save_path.length() - 1] != TJS_W('/'))
				{
					save_path += TJS_W('/');
				}
				return save_path;
			}
		}
		/* Fallbacks: SDL's preference path, then the executable directory for
		   portable development builds. */
		{
			char *pref_path = SDL_GetPrefPath(NULL, "krkrsdl2");
			std::string pref_path_utf8;
			if (pref_path)
			{
				pref_path_utf8 = pref_path;
				SDL_free(pref_path);
				tjs_string pref_path_utf16;
				TVPUtf8ToUtf16(pref_path_utf16, pref_path_utf8);
				return pref_path_utf16;
			}
			ttstr nativeDataPath = ttstr(TVPGetAppPath().AsStdString());
			TVPGetLocalName(nativeDataPath);
			nativeDataPath += TJS_W("/savedata/");
			return nativeDataPath.AsStdString();
		}
#elif defined(__vita__)
		return TJS_W("savedata0:/savedata/");
#else
		char *pref_path = SDL_GetPrefPath(NULL, "krkrsdl2");
		std::string pref_path_utf8;
		if (pref_path)
		{
			pref_path_utf8 = pref_path;
			SDL_free(pref_path);
			tjs_string pref_path_utf16;
			TVPUtf8ToUtf16(pref_path_utf16, pref_path_utf8);
			return pref_path_utf16;
		}
		ttstr nativeDataPath = ttstr(TVPGetAppPath().AsStdString());
#ifndef __EMSCRIPTEN__
		if (!nativeDataPath.IsEmpty())
		{
			TVPGetLocalName(nativeDataPath);
		}
#endif
		nativeDataPath += TJS_W("/savedata/");
		return nativeDataPath.AsStdString();
#endif
#endif
	}
	static tjs_string GetUserConfigFileName( const tjs_string& datapath, const tjs_string& exename ) {
		// exepath, personalpath, appdatapath
		return GetDataPathDirectory(datapath, exename) + ExtractFileName(ChangeFileExt(exename, TJS_W(".cfu")));
	}
};


#endif // __APPLICATION_SPECIAL_PATH_H__
