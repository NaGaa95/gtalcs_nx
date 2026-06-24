/* jni_fake.c -- fake JNI environment for the GTA LCS GTAJNIlib/HAL layer
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <switch.h>

#include "util.h"
#include "jni_fake.h"
#include "movie_player.h"

#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006

typedef uint64_t juint;

// ---------------------------------------------------------------------------
// fake object model
// ---------------------------------------------------------------------------

enum {
  TAG_OBJECT = 0x4f424a31, // 'OBJ1'
  TAG_STRING = 0x53545231, // 'STR1'
  TAG_OBJARR = 0x4f415231, // 'OAR1'
  TAG_PRIARR = 0x50415231, // 'PAR1'
  TAG_ID     = 0x4d494431, // 'MID1'
};

typedef struct {
  uint32_t tag;
  char label[96];
} FakeObject;

typedef struct {
  uint32_t tag;
  char *utf;
} FakeString;

typedef struct {
  uint32_t tag;
  int len;
  void **items;
} FakeObjArray;

typedef struct {
  uint32_t tag;
  int len;
  int elem_size;
  void *data;
} FakePriArray;

// method/field IDs are pointers to these records; calls dispatch by (class, name).
typedef struct {
  uint32_t tag;
  char cls[64];
  char name[80];
  char sig[80];
} FakeID;

volatile int jni_quit_requested = 0;
volatile int jni_frontend_ready = 0;

void *jni_make_object(const char *label) {
  FakeObject *o = calloc(1, sizeof(*o));
  o->tag = TAG_OBJECT;
  strncpy(o->label, label, sizeof(o->label) - 1);
  o->label[sizeof(o->label) - 1] = '\0';   // strncpy may not NUL-terminate
  return o;
}

void *jni_make_string(const char *utf) {
  FakeString *s = calloc(1, sizeof(*s));
  s->tag = TAG_STRING;
  s->utf = strdup(utf ? utf : "");
  return s;
}

void *jni_make_string_array(int n, const char **strs) {
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = n;
  a->items = calloc(n ? n : 1, sizeof(void *));
  for (int i = 0; i < n; i++)
    a->items[i] = jni_make_string(strs[i]);
  return a;
}

void *jni_make_int_array(int n, const int *vals) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = n;
  a->elem_size = sizeof(int);
  a->data = calloc(n ? n : 1, sizeof(int));
  if (vals)
    memcpy(a->data, vals, n * sizeof(int));
  return a;
}

static const char *obj_str(void *jstr) {
  FakeString *s = jstr;
  if (s && s->tag == TAG_STRING)
    return s->utf;
  return "";
}

// is the FakeID's class basename equal to `base`? (ignores the package path)
static int cls_is(const FakeID *id, const char *base) {
  const char *slash = strrchr(id->cls, '/');
  const char *name = slash ? slash + 1 : id->cls;
  return strcmp(name, base) == 0;
}

// ---------------------------------------------------------------------------
// deferred native-callback queue (the Java->native direction)
// ---------------------------------------------------------------------------

#define CB_QUEUE_LEN 32
static JniCallback cb_queue[CB_QUEUE_LEN];
static int cb_head = 0, cb_tail = 0;

static void push_cb(JniCallbackType type, int arg0) {
  const int next = (cb_tail + 1) % CB_QUEUE_LEN;
  if (next == cb_head) {
    debugPrintf("JNI: callback queue full, dropping %d\n", type);
    return;
  }
  cb_queue[cb_tail].type = type;
  cb_queue[cb_tail].arg0 = arg0;
  cb_tail = next;
}

int jni_pop_callback(JniCallback *out) {
  if (cb_head == cb_tail)
    return 0;
  if (out)
    *out = cb_queue[cb_head];
  cb_head = (cb_head + 1) % CB_QUEUE_LEN;
  return 1;
}

// ---------------------------------------------------------------------------
// native runnable queue (andThread.runOnMainThread / runOnBackgroundThread)
// ---------------------------------------------------------------------------

#define RUN_QUEUE_LEN 64
static JniRunnable run_queue[RUN_QUEUE_LEN];
static int run_head = 0, run_tail = 0;

static void push_runnable(int handle, int arg) {
  const int next = (run_tail + 1) % RUN_QUEUE_LEN;
  if (next == run_head) {
    debugPrintf("JNI: runnable queue full, dropping handle %d\n", handle);
    return;
  }
  run_queue[run_tail].handle = handle;
  run_queue[run_tail].arg = arg;
  run_tail = next;
}

int jni_pop_runnable(JniRunnable *out) {
  if (run_head == run_tail)
    return 0;
  if (out)
    *out = run_queue[run_head];
  run_head = (run_head + 1) % RUN_QUEUE_LEN;
  return 1;
}

// ---------------------------------------------------------------------------
// andSecureData key/value store, persisted as tab-separated lines.
// Backs GetString/SetString/GetInt/SetInt/GetBool/SetBool/GetFloat/SetFloat
// (everything is stored as a string and parsed on read).
// ---------------------------------------------------------------------------

#define KV_MAX 128

typedef struct {
  char key[64];
  char val[256];
} KvPair;

static KvPair kv_store[KV_MAX];
static int kv_count = 0;

static const char *kv_get(const char *key) {
  for (int i = 0; i < kv_count; i++)
    if (!strcmp(kv_store[i].key, key))
      return kv_store[i].val;
  return NULL;
}

static void kv_save(void) {
  FILE *f = fopen(APPSTATE_NAME, "w");
  if (!f)
    return;
  for (int i = 0; i < kv_count; i++)
    fprintf(f, "%s\t%s\n", kv_store[i].key, kv_store[i].val);
  fclose(f);
}

static void kv_set(const char *key, const char *val) {
  for (int i = 0; i < kv_count; i++) {
    if (!strcmp(kv_store[i].key, key)) {
      strlcpy(kv_store[i].val, val, sizeof(kv_store[i].val));
      kv_save();
      return;
    }
  }
  if (kv_count >= KV_MAX) {
    debugPrintf("JNI: secure-data store full, dropping %s\n", key);
    return;
  }
  strlcpy(kv_store[kv_count].key, key, sizeof(kv_store[kv_count].key));
  strlcpy(kv_store[kv_count].val, val, sizeof(kv_store[kv_count].val));
  kv_count++;
  kv_save();
}

static void kv_load(void) {
  FILE *f = fopen(APPSTATE_NAME, "r");
  if (!f)
    return;
  char line[360];
  while (kv_count < KV_MAX && fgets(line, sizeof(line), f)) {
    char *tab = strchr(line, '\t');
    if (!tab)
      continue;
    *tab = 0;
    char *val = tab + 1;
    val[strcspn(val, "\r\n")] = 0;
    strlcpy(kv_store[kv_count].key, line, sizeof(kv_store[kv_count].key));
    strlcpy(kv_store[kv_count].val, val, sizeof(kv_store[kv_count].val));
    kv_count++;
  }
  fclose(f);
}

// ---------------------------------------------------------------------------
// andFile user-file channel: small config/save blobs backed by native files
// in the game directory. NOT used for bulk assets (those load natively).
// ---------------------------------------------------------------------------

// returns a NUL-terminated heap buffer with the file contents, or NULL.
static char *read_whole_file(const char *path, long *out_len) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (len < 0) { fclose(f); return NULL; }
  char *buf = malloc(len + 1);
  if (!buf) { fclose(f); return NULL; }
  const size_t got = fread(buf, 1, len, f);
  fclose(f);
  buf[got] = 0;
  if (out_len)
    *out_len = (long)got;
  return buf;
}

// method/field ID pool
#define MAX_IDS 512
static FakeID id_pool[MAX_IDS];
static int id_count = 0;

static FakeID *get_id(const char *cls, const char *name, const char *sig) {
  for (int i = 0; i < id_count; i++) {
    if (!strcmp(id_pool[i].name, name) && !strcmp(id_pool[i].sig, sig) &&
        !strcmp(id_pool[i].cls, cls))
      return &id_pool[i];
  }
  if (id_count >= MAX_IDS) {
    debugPrintf("JNI: id pool exhausted!\n");
    return &id_pool[0];
  }
  FakeID *id = &id_pool[id_count++];
  id->tag = TAG_ID;
  snprintf(id->cls, sizeof(id->cls), "%s", cls);
  snprintf(id->name, sizeof(id->name), "%s", name);
  snprintf(id->sig, sizeof(id->sig), "%s", sig);
  return id;
}

// label of the FakeObject that FindClass produced for this jclass
static const char *class_label(void *cls) {
  FakeObject *o = cls;
  if (o && o->tag == TAG_OBJECT)
    return o->label;
  return "";
}

// va_list arg helpers (JNI variadic: jfloat is promoted to double, small ints
// to int, jobject is a pointer)
static const char *next_str(va_list va) { return obj_str(va_arg(va, void *)); }
static int next_int(va_list va) { return va_arg(va, int); }
static float next_float(va_list va) { return (float)va_arg(va, double); }

// ---------------------------------------------------------------------------
// HAL dispatch, keyed by (class, method)
// ---------------------------------------------------------------------------

// --- video: drive the ffmpeg movie player from the engine's PlayVideo calls ---
// Two paths map onto movie_play(): CommonAPI.PlayVideo polls isVideoPlaying,
// andVideo.PlayVideoFile waits for the VIDEO_FINISHED callback. A missing movie
// skips cleanly so the game never stalls.
static int g_video_finish_pending;

static int jni_start_movie(const char *vid) {
  if (!vid || !vid[0])
    return 0;
  if (movie_play(vid, 1))                 // 1 = skippable (movie_skip on input)
    return 1;
  char alt[300];                          // also try under an assets/ prefix
  snprintf(alt, sizeof alt, "assets/%s", vid[0] == '/' ? vid + 1 : vid);
  if (movie_play(alt, 1))
    return 1;
  // engine asks for Movies/LCS_INTRO_*.m4v; the only shipped movie is the Android
  // resource res/raw/intro.m4v, so fall back to it (or a bare intro.m4v)
  if (movie_play("res/raw/intro.m4v", 1))
    return 1;
  return movie_play("intro.m4v", 1);
}

// called once per main-loop iteration from main.c: fires VIDEO_FINISHED once a
// movie that the callback-based path (andVideo) started has finished.
void jni_video_tick(void) {
  if (g_video_finish_pending && !movie_is_playing()) {
    g_video_finish_pending = 0;
    push_cb(JNI_CB_VIDEO_FINISHED, 0);
  }
}

static void hal_void(const FakeID *id, va_list va) {
  const char *name = id->name;

  // --- com.rockstargames.gtalcs.CommonAPI (loading UI, misc) ---
  if (cls_is(id, "CommonAPI")) {
    if (!strcmp(name, "QuitApp")) {
      debugPrintf("JNI: CommonAPI.QuitApp -> request quit\n");
      jni_quit_requested = 1;
      return;
    }
    if (!strcmp(name, "PlaylistFinishInit")) {
      // native side waits for HandlePlaylistFinishInit(success)
      const int success = next_int(va);
      debugPrintf("JNI: CommonAPI.PlaylistFinishInit(%d)\n", success);
      push_cb(JNI_CB_PLAYLIST_FINISH_INIT, success);
      return;
    }
    if (!strcmp(name, "ShowLoadingScreen")) {
      cpu_boost(1);   // overclock the CPU while a loading screen is up
      return;
    }
    if (!strcmp(name, "HideLoadingScreen")) {
      // loading done: drop the boost and signal the main loop to start the
      // frontend (the Social Club SDK would normally drive this)
      cpu_boost(0);
      jni_frontend_ready = 1;
      return;
    }
    if (!strcmp(name, "PlayVideo")) {
      const char *vid = next_str(va);
      int ok = jni_start_movie(vid);
      debugPrintf("JNI: CommonAPI.PlayVideo(%s) -> %s\n", vid ? vid : "(null)",
                  ok ? "playing" : "not found, skipping");
      return; // the game waits via isVideoPlaying (state 4); no callback needed
    }
    // ShowLoadingScreen/HideLoadingScreen/ShowLoadingBar(F)/setKeepScreenOn/
    // setInUI/RemoveFrameRateLock/GoToUrl/PlaylistPlay/PlaylistPause -> no-op
    debugPrintf("JNI: CommonAPI.%s ignored\n", name);
    return;
  }

  // --- com.rockstargames.gtalcs.RockstarJNIlib (Social Club gate / sign-in) ---
  // No Java UI, so every gate is auto-accepted by firing the native callback the
  // Java SDK would have. New Game and Load take DISTINCT native paths and must
  // stay split: ShowGate -> StartGame (fresh game), ShowGateBeforeLoad ->
  // StartGameBeforeLoad (loads the save slot). Answering both with the load path
  // made New Game load the existing save instead of starting fresh.
  if (cls_is(id, "RockstarJNIlib")) {
    if (!strcmp(name, "ShowGate")) {
      debugPrintf("JNI: RockstarJNIlib.ShowGate -> StartGame (new game)\n");
      push_cb(JNI_CB_RS_START_GAME, 0);
      return;
    }
    if (!strcmp(name, "ShowGateBeforeLoad")) {
      debugPrintf("JNI: RockstarJNIlib.ShowGateBeforeLoad -> StartGameBeforeLoad (load)\n");
      push_cb(JNI_CB_RS_START_BEFORE_LOAD, 0);
      return;
    }
    if (!strcmp(name, "CheckGate")) {
      debugPrintf("JNI: RockstarJNIlib.CheckGate -> FinishGate\n");
      push_cb(JNI_CB_RS_FINISH_GATE, 0);
      return;
    }
    if (!strcmp(name, "GameLoad")) {
      debugPrintf("JNI: RockstarJNIlib.GameLoad -> GameLoaded\n");
      push_cb(JNI_CB_RS_GAME_LOADED, 0);
      return;
    }
    if (!strcmp(name, "SignIn") || !strcmp(name, "SignOut")) {
      debugPrintf("JNI: RockstarJNIlib.%s -> offline state change\n", name);
      push_cb(JNI_CB_RS_STATE_CHANGED, 0); // offline / signed-out
      return;
    }
    // ShowPrompt / SetLocalePriority / UpdateRockstarID -> no-op
    debugPrintf("JNI: RockstarJNIlib.%s ignored\n", name);
    return;
  }

  // --- com.rockstargames.hal.andSecureData (typed KV store) ---
  if (cls_is(id, "andSecureData")) {
    if (!strcmp(name, "SetString")) {
      const char *key = next_str(va);
      const char *val = next_str(va);
      kv_set(key, val);
      return;
    }
    if (!strcmp(name, "SetInt")) {
      const char *key = next_str(va);
      char buf[24];
      snprintf(buf, sizeof(buf), "%d", next_int(va));
      kv_set(key, buf);
      return;
    }
    if (!strcmp(name, "SetBool")) {
      const char *key = next_str(va);
      kv_set(key, next_int(va) ? "1" : "0");
      return;
    }
    if (!strcmp(name, "SetFloat")) {
      const char *key = next_str(va);
      char buf[32];
      snprintf(buf, sizeof(buf), "%g", next_float(va));
      kv_set(key, buf);
      return;
    }
    debugPrintf("JNI: andSecureData.%s ignored\n", name);
    return;
  }

  // --- com.rockstargames.hal.andFile (write a small user file natively) ---
  if (cls_is(id, "andFile")) {
    if (!strcmp(name, "writeUserFile")) {
      const char *fn = next_str(va);
      const char *data = next_str(va);
      FILE *f = fopen(fn, "wb");
      if (f) { fwrite(data, 1, strlen(data), f); fclose(f); }
      debugPrintf("JNI: andFile.writeUserFile(%s) -> %s\n", fn, f ? "ok" : "FAIL");
      return;
    }
    debugPrintf("JNI: andFile.%s (void) ignored\n", name);
    return;
  }

  // --- com.rockstargames.hal.andThread ---
  if (cls_is(id, "andThread")) {
    if (!strcmp(name, "runOnMainThread") || !strcmp(name, "runOnBackgroundThread")) {
      const int handle = next_int(va);
      const int arg = next_int(va);
      debugPrintf("JNI: andThread.%s(%d, %d) -> queue runnable\n", name, handle, arg);
      push_runnable(handle, arg);
      return;
    }
    if (!strcmp(name, "sleep")) {
      // (J)V -- a long ms value; promoted as a 64-bit arg
      const long ms = va_arg(va, long);
      svcSleepThread((s64)ms * 1000000LL);
      return;
    }
    debugPrintf("JNI: andThread.%s ignored\n", name);
    return;
  }

  // --- com.rockstargames.hal.andVideo (skip FMV: data is inside the WAD) ---
  if (cls_is(id, "andVideo")) {
    if (!strcmp(name, "PlayVideoFile")) {
      const char *path = next_str(va);
      if (jni_start_movie(path)) {
        debugPrintf("JNI: andVideo.PlayVideoFile(%s) -> playing\n", path);
        g_video_finish_pending = 1;        // fire VIDEO_FINISHED when it ends
      } else {
        debugPrintf("JNI: andVideo.PlayVideoFile(%s) -> not found, finish now\n", path);
        push_cb(JNI_CB_VIDEO_FINISHED, 0); // skip cleanly
      }
      return;
    }
    return;
  }

  // --- com.rockstargames.hal.andAudio (UI sfx) -- stub for now ---
  // --- everything else (andView/andViewManager/andLabel/andButton/...) ---
  debugPrintf("JNI: CallVoidMethod %s.%s ignored\n", id->cls, name);
}

static juint hal_int(const FakeID *id, va_list va) {
  const char *name = id->name;

  if (cls_is(id, "andSecureData")) {
    if (!strcmp(name, "GetInt")) {
      const char *key = next_str(va);
      const char *v = kv_get(key);
      return v ? (juint)(unsigned)atoi(v) : 0;
    }
  }
  if (cls_is(id, "andAudio")) {
    if (!strcmp(name, "PlayAudioFile"))
      return 0; // handle 0 == "not playing"
  }
  debugPrintf("JNI: CallIntMethod %s.%s -> 0\n", id->cls, name);
  return 0;
}

static juint hal_bool(const FakeID *id, va_list va) {
  const char *name = id->name;

  if (cls_is(id, "CommonAPI")) {
    if (!strcmp(name, "isVideoPlaying"))
      return movie_is_playing();
    // PlaylistPlaying / PlaylistHasSongs / GooglePlaylistAvailable -> false
    return 0;
  }
  if (cls_is(id, "andSecureData")) {
    if (!strcmp(name, "GetBool")) {
      const char *key = next_str(va);
      const char *v = kv_get(key);
      return v ? (atoi(v) != 0) : 0;
    }
  }
  if (cls_is(id, "andAudio")) {
    if (!strcmp(name, "IsPlaying"))
      return 0;
  }
  if (cls_is(id, "RockstarJNIlib")) {
    // Social Club is faked, so the gate is always "passed"; returning false made
    // the in-game load/new-game menu refuse to act (looked like a freeze).
    if (!strcmp(name, "CheckGate"))
      return 1;
  }
  debugPrintf("JNI: CallBooleanMethod %s.%s -> false\n", id->cls, name);
  return 0;
}

static float hal_float(const FakeID *id, va_list va) {
  if (cls_is(id, "andSecureData") && !strcmp(id->name, "GetFloat")) {
    const char *key = next_str(va);
    const char *v = kv_get(key);
    return v ? (float)atof(v) : 0.0f;
  }
  debugPrintf("JNI: CallFloatMethod %s.%s -> 0\n", id->cls, id->name);
  return 0.0f;
}

static void *hal_object(const FakeID *id, va_list va) {
  const char *name = id->name;

  if (cls_is(id, "CommonAPI")) {
    if (!strcmp(name, "GetDeviceLanguage"))
      return jni_make_string("en");
    if (!strcmp(name, "GetVersionName"))
      return jni_make_string("2.4");
  }

  if (cls_is(id, "andSecureData")) {
    if (!strcmp(name, "GetString")) {
      const char *key = next_str(va);
      const char *v = kv_get(key);
      return jni_make_string(v ? v : "");
    }
  }

  if (cls_is(id, "andFile")) {
    if (!strcmp(name, "readUserFile")) {
      const char *fn = next_str(va);
      char *data = read_whole_file(fn, NULL);
      void *r = data ? jni_make_string(data) : NULL;
      free(data);
      debugPrintf("JNI: andFile.readUserFile(%s) -> %s\n", fn, r ? "ok" : "null");
      return r;
    }
    if (!strcmp(name, "getFile")) {
      // (dir, name, ext)[B -- best effort: read `name` from the game dir as a
      // byte[]; NULL on miss (first boot has no saves, the engine handles it)
      (void)next_str(va);                 // dir / category (unused)
      const char *fn = next_str(va);
      long len = 0;
      char *data = read_whole_file(fn, &len);
      if (!data)
        return NULL;
      FakePriArray *a = calloc(1, sizeof(*a));
      a->tag = TAG_PRIARR;
      a->len = (int)len;
      a->elem_size = 1;
      a->data = data; // hand ownership to the array
      return a;
    }
  }

  // toImage / addSubview returns / NewObject-style getters -> a fresh fake obj
  debugPrintf("JNI: CallObjectMethod %s.%s -> fake object\n", id->cls, name);
  return jni_make_object("halobject");
}

// ---------------------------------------------------------------------------
// DeviceInfo-style jfield reads. Rarely hit (LCS pushes device info through
// setDeviceInfo natively); sane defaults.
// ---------------------------------------------------------------------------

static juint get_boolean_field(const char *name) {
  if (!strcmp(name, "isTvDevice"))     return 1;
  if (!strcmp(name, "hasTouchScreen")) return 1;
  if (!strcmp(name, "hasVibrator"))    return 0;
  return 0;
}

static juint get_int_field(const char *name) {
  if (!strcmp(name, "osVersion"))    return 30;
  if (!strcmp(name, "cpuFrequency")) return 1785;
  return 0;
}

static void *get_object_field(const char *name) {
  if (!strcmp(name, "manufacturer")) return jni_make_string("Nintendo");
  if (!strcmp(name, "model"))        return jni_make_string("Switch");
  if (!strcmp(name, "hardware"))     return jni_make_string("nx");
  if (!strcmp(name, "product"))      return jni_make_string("switch");
  return NULL;
}

// ---------------------------------------------------------------------------
// JNIEnv function table
// ---------------------------------------------------------------------------

static juint j_GetVersion(void *env) { (void)env; return JNI_VERSION_1_6; }

static void *j_FindClass(void *env, const char *name) {
  (void)env;
  debugPrintf("JNI: FindClass(%s)\n", name);
  return jni_make_object(name);
}

static void *j_GetMethodID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  debugPrintf("JNI: GetMethodID(%s::%s %s)\n", class_label(cls), name, sig);
  return get_id(class_label(cls), name, sig);
}

static void *j_GetFieldID(void *env, void *cls, const char *name, const char *sig) {
  (void)env;
  return get_id(class_label(cls), name, sig);
}

static void *j_GetObjectClass(void *env, void *obj) {
  (void)env; (void)obj;
  return jni_make_object("class");
}

static void *j_NewGlobalRef(void *env, void *obj) { (void)env; return obj; }
static void *j_NewLocalRef(void *env, void *obj) { (void)env; return obj; }
static juint j_ret0_2(void *env, void *a) { (void)env; (void)a; return 0; }
static juint j_ret0_3(void *env, void *a, void *b) { (void)env; (void)a; (void)b; return 0; }

// --- Call<type>Method ---

static juint j_CallBooleanMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_bool(id, va);
}
static juint j_CallBooleanMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_bool(id, va);
  va_end(va);
  return r;
}

static juint j_CallIntMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_int(id, va);
}
static juint j_CallIntMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_int(id, va);
  va_end(va);
  return r;
}

static void *j_CallObjectMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_object(id, va);
}
static void *j_CallObjectMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = hal_object(id, va);
  va_end(va);
  return r;
}

static void j_CallVoidMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  hal_void(id, va);
}
static void j_CallVoidMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  hal_void(id, va);
  va_end(va);
}

static float j_CallFloatMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj;
  return hal_float(id, va);
}
static float j_CallFloatMethod(void *env, void *obj, FakeID *id, ...) {
  va_list va; va_start(va, id);
  float r = hal_float(id, va);
  va_end(va);
  return r;
}

static juint j_CallLongMethodV(void *env, void *obj, FakeID *id, va_list va) {
  (void)env; (void)obj; (void)va;
  debugPrintf("JNI: CallLongMethod %s.%s -> 0\n", id->cls, id->name);
  return 0;
}

// static variants share the dispatchers (the receiver doesn't matter)
static void *j_CallStaticObjectMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_object(id, va);
}
static void *j_CallStaticObjectMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = hal_object(id, va);
  va_end(va);
  return r;
}
static juint j_CallStaticBooleanMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_bool(id, va);
}
static juint j_CallStaticBooleanMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_bool(id, va);
  va_end(va);
  return r;
}
static juint j_CallStaticIntMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_int(id, va);
}
static juint j_CallStaticIntMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  juint r = hal_int(id, va);
  va_end(va);
  return r;
}
static void j_CallStaticVoidMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  hal_void(id, va);
}
static void j_CallStaticVoidMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  hal_void(id, va);
  va_end(va);
}
static float j_CallStaticFloatMethodV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)cls;
  return hal_float(id, va);
}
static float j_CallStaticFloatMethod(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  float r = hal_float(id, va);
  va_end(va);
  return r;
}

static void *j_NewObjectV(void *env, void *cls, FakeID *id, va_list va) {
  (void)env; (void)id; (void)va;
  return jni_make_object(class_label(cls));
}
static void *j_NewObject(void *env, void *cls, FakeID *id, ...) {
  va_list va; va_start(va, id);
  void *r = j_NewObjectV(env, cls, id, va);
  va_end(va);
  return r;
}

// --- fields ---

static void *j_GetObjectField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_object_field(id->name);
}
static juint j_GetBooleanField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_boolean_field(id->name);
}
static juint j_GetIntField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj;
  return get_int_field(id->name);
}
static juint j_GetLongField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj; (void)id;
  return 0;
}
static float j_GetFloatField(void *env, void *obj, FakeID *id) {
  (void)env; (void)obj; (void)id;
  return 0.0f;
}

// --- strings ---

static void *j_NewStringUTF(void *env, const char *utf) {
  (void)env;
  return jni_make_string(utf);
}

static const char *j_GetStringUTFChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  return obj_str(jstr);
}

static void j_ReleaseStringUTFChars(void *env, void *jstr, const char *utf) {
  (void)env; (void)jstr; (void)utf;
}

static juint j_GetStringUTFLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static juint j_GetStringLength(void *env, void *jstr) {
  (void)env;
  return strlen(obj_str(jstr));
}

static const uint16_t *j_GetStringChars(void *env, void *jstr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 1;
  const char *utf = obj_str(jstr);
  const int len = strlen(utf);
  uint16_t *out = calloc(len + 1, sizeof(uint16_t));
  for (int i = 0; i < len; i++)
    out[i] = (uint8_t)utf[i];
  return out;
}

static void j_ReleaseStringChars(void *env, void *jstr, const uint16_t *chars) {
  (void)env; (void)jstr;
  free((void *)chars);
}

// --- arrays ---

static juint j_GetArrayLength(void *env, void *arr) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && (a->tag == TAG_OBJARR || a->tag == TAG_PRIARR))
    return a->len;
  return 0;
}

static void *j_GetObjectArrayElement(void *env, void *arr, int idx) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    return a->items[idx];
  return jni_make_string("");
}

static void j_SetObjectArrayElement(void *env, void *arr, int idx, void *val) {
  (void)env;
  FakeObjArray *a = arr;
  if (a && a->tag == TAG_OBJARR && idx >= 0 && idx < a->len)
    a->items[idx] = val;
}

static void *j_NewObjectArray(void *env, int len, void *cls, void *init) {
  (void)env; (void)cls;
  FakeObjArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_OBJARR;
  a->len = len;
  a->items = calloc(len ? len : 1, sizeof(void *));
  for (int i = 0; i < len; i++)
    a->items[i] = init;
  return a;
}

static void *new_pri_array(int len, int elem_size) {
  FakePriArray *a = calloc(1, sizeof(*a));
  a->tag = TAG_PRIARR;
  a->len = len;
  a->elem_size = elem_size;
  a->data = calloc(len ? len : 1, elem_size);
  return a;
}

static void *j_NewByteArray(void *env, int len)  { (void)env; return new_pri_array(len, 1); }
static void *j_NewIntArray(void *env, int len)   { (void)env; return new_pri_array(len, 4); }
static void *j_NewFloatArray(void *env, int len) { (void)env; return new_pri_array(len, 4); }

static void *j_GetPriArrayElements(void *env, void *arr, uint8_t *is_copy) {
  (void)env;
  if (is_copy) *is_copy = 0;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR)
    return a->data;
  return NULL;
}

static void j_ReleasePriArrayElements(void *env, void *arr, void *elems, int mode) {
  (void)env; (void)arr; (void)elems; (void)mode;
}

static void j_GetPriArrayRegion(void *env, void *arr, int start, int len, void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy(buf, (char *)a->data + start * a->elem_size, len * a->elem_size);
}

static void j_SetPriArrayRegion(void *env, void *arr, int start, int len, const void *buf) {
  (void)env;
  FakePriArray *a = arr;
  if (a && a->tag == TAG_PRIARR && start >= 0 && start + len <= a->len)
    memcpy((char *)a->data + start * a->elem_size, buf, len * a->elem_size);
}

// --- misc ---

static juint j_RegisterNatives(void *env, void *cls, void *methods, int n) {
  (void)env; (void)cls; (void)methods;
  debugPrintf("JNI: RegisterNatives(%d methods) ignored\n", n);
  return 0;
}

static juint j_GetJavaVM(void *env, void **vm) {
  (void)env;
  *vm = fake_vm;
  return JNI_OK;
}

static juint j_ExceptionCheck(void *env) { (void)env; return 0; }
static void *j_ExceptionOccurred(void *env) { (void)env; return NULL; }
static void j_ExceptionClearDescribe(void *env) { (void)env; }
static void j_DeleteRef(void *env, void *obj) { (void)env; (void)obj; }
static juint j_PushLocalFrame(void *env, int cap) { (void)env; (void)cap; return 0; }
static void *j_PopLocalFrame(void *env, void *result) { (void)env; return result; }

static juint j_unimplemented(void) {
  debugPrintf("JNI: call to unimplemented function slot\n");
  return 0;
}

// ---------------------------------------------------------------------------
// table assembly (indices per the JNI specification)
// ---------------------------------------------------------------------------

static void *env_table[233];
static void **env_table_ptr = env_table;
void *fake_env = &env_table_ptr;

static juint vm_DestroyJavaVM(void *vm) { (void)vm; return JNI_OK; }
static juint vm_AttachCurrentThread(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  if (env) *env = fake_env;
  return JNI_OK;
}
static juint vm_DetachCurrentThread(void *vm) { (void)vm; return JNI_OK; }
static juint vm_GetEnv(void *vm, void **env, int version) {
  (void)vm; (void)version;
  if (env) *env = fake_env;
  return JNI_OK;
}

static void *vm_table[8];
static void **vm_table_ptr = vm_table;
void *fake_vm = &vm_table_ptr;

void jni_init(void) {
  kv_load();

  for (int i = 0; i < 233; i++)
    env_table[i] = (void *)j_unimplemented;

  env_table[4]  = (void *)j_GetVersion;
  env_table[6]  = (void *)j_FindClass;
  env_table[15] = (void *)j_ExceptionOccurred;
  env_table[16] = (void *)j_ExceptionClearDescribe; // ExceptionDescribe
  env_table[17] = (void *)j_ExceptionClearDescribe; // ExceptionClear
  env_table[19] = (void *)j_PushLocalFrame;
  env_table[20] = (void *)j_PopLocalFrame;
  env_table[21] = (void *)j_NewGlobalRef;
  env_table[22] = (void *)j_DeleteRef;  // DeleteGlobalRef
  env_table[23] = (void *)j_DeleteRef;  // DeleteLocalRef
  env_table[24] = (void *)j_ret0_3;     // IsSameObject
  env_table[25] = (void *)j_NewLocalRef;
  env_table[26] = (void *)j_ret0_2;     // EnsureLocalCapacity
  env_table[28] = (void *)j_NewObject;
  env_table[29] = (void *)j_NewObjectV;
  env_table[31] = (void *)j_GetObjectClass;
  env_table[33] = (void *)j_GetMethodID;
  env_table[34] = (void *)j_CallObjectMethod;
  env_table[35] = (void *)j_CallObjectMethodV;
  env_table[37] = (void *)j_CallBooleanMethod;
  env_table[38] = (void *)j_CallBooleanMethodV;
  env_table[49] = (void *)j_CallIntMethod;
  env_table[50] = (void *)j_CallIntMethodV;
  env_table[53] = (void *)j_CallLongMethodV;
  env_table[55] = (void *)j_CallFloatMethod;
  env_table[56] = (void *)j_CallFloatMethodV;
  env_table[61] = (void *)j_CallVoidMethod;
  env_table[62] = (void *)j_CallVoidMethodV;
  env_table[94] = (void *)j_GetFieldID;
  env_table[95] = (void *)j_GetObjectField;
  env_table[96] = (void *)j_GetBooleanField;
  env_table[100] = (void *)j_GetIntField;
  env_table[101] = (void *)j_GetLongField;
  env_table[102] = (void *)j_GetFloatField;
  env_table[113] = (void *)j_GetMethodID;               // GetStaticMethodID
  env_table[114] = (void *)j_CallStaticObjectMethod;
  env_table[115] = (void *)j_CallStaticObjectMethodV;
  env_table[117] = (void *)j_CallStaticBooleanMethod;
  env_table[118] = (void *)j_CallStaticBooleanMethodV;
  env_table[129] = (void *)j_CallStaticIntMethod;
  env_table[130] = (void *)j_CallStaticIntMethodV;
  env_table[135] = (void *)j_CallStaticFloatMethod;   // s0 must be set for float returns
  env_table[136] = (void *)j_CallStaticFloatMethodV;
  env_table[141] = (void *)j_CallStaticVoidMethod;
  env_table[142] = (void *)j_CallStaticVoidMethodV;
  env_table[144] = (void *)j_GetFieldID;                // GetStaticFieldID
  env_table[145] = (void *)j_GetObjectField;            // GetStaticObjectField
  env_table[146] = (void *)j_GetBooleanField;           // GetStaticBooleanField
  env_table[150] = (void *)j_GetIntField;               // GetStaticIntField
  env_table[164] = (void *)j_GetStringLength;
  env_table[165] = (void *)j_GetStringChars;
  env_table[166] = (void *)j_ReleaseStringChars;
  env_table[167] = (void *)j_NewStringUTF;
  env_table[168] = (void *)j_GetStringUTFLength;
  env_table[169] = (void *)j_GetStringUTFChars;
  env_table[170] = (void *)j_ReleaseStringUTFChars;
  env_table[171] = (void *)j_GetArrayLength;
  env_table[172] = (void *)j_NewObjectArray;
  env_table[173] = (void *)j_GetObjectArrayElement;
  env_table[174] = (void *)j_SetObjectArrayElement;
  env_table[176] = (void *)j_NewByteArray;
  env_table[179] = (void *)j_NewIntArray;
  env_table[181] = (void *)j_NewFloatArray;
  env_table[183] = (void *)j_GetPriArrayElements; // boolean
  env_table[184] = (void *)j_GetPriArrayElements; // byte
  env_table[185] = (void *)j_GetPriArrayElements; // char
  env_table[186] = (void *)j_GetPriArrayElements; // short
  env_table[187] = (void *)j_GetPriArrayElements; // int
  env_table[188] = (void *)j_GetPriArrayElements; // long
  env_table[189] = (void *)j_GetPriArrayElements; // float
  env_table[190] = (void *)j_GetPriArrayElements; // double
  for (int i = 191; i <= 198; i++)
    env_table[i] = (void *)j_ReleasePriArrayElements;
  for (int i = 199; i <= 206; i++)
    env_table[i] = (void *)j_GetPriArrayRegion;
  for (int i = 207; i <= 214; i++)
    env_table[i] = (void *)j_SetPriArrayRegion;
  env_table[215] = (void *)j_RegisterNatives;
  env_table[216] = (void *)j_ret0_2;    // UnregisterNatives
  env_table[217] = (void *)j_ret0_2;    // MonitorEnter
  env_table[218] = (void *)j_ret0_2;    // MonitorExit
  env_table[219] = (void *)j_GetJavaVM;
  env_table[222] = (void *)j_GetPriArrayElements;     // GetPrimitiveArrayCritical
  env_table[223] = (void *)j_ReleasePriArrayElements; // ReleasePrimitiveArrayCritical
  env_table[226] = (void *)j_NewGlobalRef; // NewWeakGlobalRef
  env_table[227] = (void *)j_DeleteRef;    // DeleteWeakGlobalRef
  env_table[228] = (void *)j_ExceptionCheck;

  vm_table[3] = (void *)vm_DestroyJavaVM;
  vm_table[4] = (void *)vm_AttachCurrentThread;
  vm_table[5] = (void *)vm_DetachCurrentThread;
  vm_table[6] = (void *)vm_GetEnv;
  vm_table[7] = (void *)vm_AttachCurrentThread; // AttachCurrentThreadAsDaemon

  debugPrintf("JNI: fake environment initialized (env=%p vm=%p)\n", fake_env, fake_vm);
}
