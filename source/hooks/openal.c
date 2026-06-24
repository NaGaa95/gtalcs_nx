/* openal.c -- OpenAL hooks
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * 2.1.131 update: libGame.so now imports OpenAL from libopenal.so instead
 * of statically linking it, so the whole API is served via the import table
 * in imports.c. Only the two creation hooks live here.
 */

#include <stdio.h>
#include <stdlib.h>

#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include "../util.h"
#include "../hooks.h"

static ALCcontext *al_ctx = NULL;
static ALCdevice *al_dev = NULL;

ALCcontext *alcCreateContextHook(ALCdevice *dev, const ALCint *unused) {
  // override the engine's 22050hz request with 44100hz
  const ALCint attr[] = { ALC_FREQUENCY, 44100, 0 };
  al_ctx = alcCreateContext(dev, attr); // capture for deinit
  return al_ctx;
}

ALCdevice *alcOpenDeviceHook(const char *name) {
  al_dev = alcOpenDevice(name); // capture for deinit
  return al_dev;
}

void deinit_openal(void) {
  if (al_dev) {
    if (al_ctx) {
      alcMakeContextCurrent(NULL);
      alcDestroyContext(al_ctx);
    }
    alcCloseDevice(al_dev);
  }
}
