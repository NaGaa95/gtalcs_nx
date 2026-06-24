/* jni_fake.h -- fake JNI environment for the GTA LCS GTAJNIlib/HAL layer
 *
 * The native game calls into a thin Java HAL (com.rockstargames.hal.and*) over
 * JNI for UI, audio, secure storage, the Social Club gate and a small user-file
 * channel; bulk data is loaded natively, not through Java. We emulate just
 * enough JavaVM/JNIEnv for those calls to resolve (no-op or mapped to Switch
 * equivalents) and drive the native callbacks the Java side would have fired
 * back from the main loop.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#ifndef __JNI_FAKE_H__
#define __JNI_FAKE_H__

#include <stdint.h>

// passed to JNI_OnLoad / NVThreadInit and to every JNI entry point
extern void *fake_vm;  // JavaVM *
extern void *fake_env; // JNIEnv *

// set when the game asks to quit (CommonAPI.QuitApp)
extern volatile int jni_quit_requested;

// set when the loading screen hides: with no Social Club SDK we kick off the
// frontend ourselves from the main loop (see RequestFrontEndStartUp in main.c)
extern volatile int jni_frontend_ready;

void jni_init(void);

// per-frame video pump: fires the VIDEO_FINISHED callback once a movie started
// via the andVideo callback path has ended (call once per main-loop iteration)
void jni_video_tick(void);

// constructors for fake Java objects to pass into the game's JNI entry points
void *jni_make_string(const char *utf);
void *jni_make_string_array(int n, const char **strs);
void *jni_make_int_array(int n, const int *vals);
void *jni_make_object(const char *label);

// ---------------------------------------------------------------------------
// deferred native callbacks: HAL methods that on Android would call back into
// libGame.so. We queue the equivalent native entry point and let the main loop
// (which owns the game thread + JNIEnv) invoke it.
// ---------------------------------------------------------------------------

typedef enum {
  JNI_CB_RS_FINISH_GATE = 1,        // RockstarJNIlib.FinishGate()
  JNI_CB_RS_START_BEFORE_LOAD,      // RockstarJNIlib.StartGameBeforeLoad()
  JNI_CB_RS_START_GAME,             // RockstarJNIlib.StartGame()
  JNI_CB_RS_GAME_LOADED,            // RockstarJNIlib.GameLoaded()
  JNI_CB_RS_STATE_CHANGED,          // RockstarJNIlib.HandleStateChanged(signedIn)
  JNI_CB_VIDEO_FINISHED,            // andVideo.VideoFinishedCB()
  JNI_CB_PLAYLIST_FINISH_INIT,      // CommonAPI.HandlePlaylistFinishInit(success)
} JniCallbackType;

typedef struct {
  JniCallbackType type;
  int arg0; // signedIn / success flag where applicable
} JniCallback;

// returns 0 when the queue is empty
int jni_pop_callback(JniCallback *out);

// native main-thread runnables posted through andThread.runOnMainThread /
// runOnBackgroundThread. The two ints are the native runnable handle + arg;
// the main loop invokes andThread.runNativeRunnable(handle, arg) for each.
typedef struct {
  int handle;
  int arg;
} JniRunnable;

// returns 0 when the queue is empty
int jni_pop_runnable(JniRunnable *out);

#endif
