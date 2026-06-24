/* config.h -- global configuration and config file handling
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

// MB reserved for the .so load region (only holds the mapped libGame.so/
// libopenal.so, ~15MB). The newlib heap gets (total RAM - this) and backs all
// dynamic allocation: the game's malloc and mesa's GPU bos. Sizing it larger
// starved the GPU and OOM'd world-load textures (glerr=0x505).
#define MEMORY_SO_MB 256

// The APK ships no libc++_shared.so; the C++ runtime resolves from its own
// libopenal.so, which statically links libc++ with default visibility. We load
// that as a second module purely as the C++ runtime donor. The game's OpenAL
// imports still bind to native openal-soft (import table beats module exports).
#define SO_NAME "libGame.so"
#define CXX_DONOR_SO_NAME "libopenal.so"
#define CONFIG_NAME "config.txt"
#define LOG_NAME "debug.log"
// backing store for the engine's get/setAppLocalValue key/value pairs
#define APPSTATE_NAME "appstate.txt"

// Define to write debug.log and nxlink stdout. Off for release (debugPrintf
// becomes a no-op).
// #define DEBUG_LOG 1

// actual screen size
extern int screen_width;
extern int screen_height;

typedef struct {
  int screen_width;
  int screen_height;
  int trilinear_filter;
  int show_fps;
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
