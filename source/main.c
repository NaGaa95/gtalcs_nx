/* main.c
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * GTA: Liberty City Stories 2.4.379 uses the older Rockstar Android engine
 * (com.rockstargames.gtalcs.GTAJNIlib + com.rockstargames.hal.and* HAL), not
 * the oswrapper framework of CTW / Max Payne. We drive the GTAJNIlib_* JNI
 * entry points through a fake JNI environment, replacing the Android GameView.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>
#include <GLES2/gl2.h>

#include "config.h"
#include "util.h"
#include "error.h"
#include "so_util.h"
#include "hooks.h"
#include "imports.h"
#include "jni_fake.h"
#include "movie_player.h"

static void *heap_so_base = NULL;
static size_t heap_so_limit = 0;

so_module donor_mod; // libopenal.so, C++ runtime donor (see config.h)
so_module game_mod;  // libGame.so

// memory-split sizes (MB), filled in by __libnx_initheap, printed from main().
size_t g_mem_total_mb = 0, g_mem_newlib_mb = 0, g_mem_so_mb = 0;

// nv GPU pool size: all GPU bos (textures+buffers) come from this transfer-memory
// region. The libnx default (~256MB) OOMs during world-load textures; libnx
// carves this from the newlib heap, trading CPU heap for GPU headroom.
u32 __nx_nv_transfermem_size = 0x60000000; // 1.5 GB GPU memory pool

extern void worldstream_probe(void);   // hooks/game.c (streaming watchdog)
volatile int g_hide_saves = 0;         // libc_shim.c: New Game hides save slots
int g_hide_saves_frames = 0;           // auto-clear countdown

// provide replacement heap init function to separate newlib heap from the .so
void __libnx_initheap(void) {
  void *addr;
  size_t size = 0, fake_heap_size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  // The newlib heap backs both the game's malloc and mesa's GPU bos, so give it
  // the bulk of RAM; the .so region only holds the ~15MB of mapped libraries.
  extern char *fake_heap_start;
  extern char *fake_heap_end;
  size_t so_region = (size_t)MEMORY_SO_MB * 1024 * 1024;
  if (so_region > size / 4)            // never grab more than 25% from newlib
    so_region = size / 4;
  fake_heap_size  = size - so_region;
  fake_heap_start = (char *)addr;
  fake_heap_end   = (char *)addr + fake_heap_size;

  heap_so_base = (char *)addr + fake_heap_size;
  heap_so_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base, 0x1000); // align to page size
  heap_so_limit = (char *)addr + size - (char *)heap_so_base;

  g_mem_total_mb  = size >> 20;
  g_mem_newlib_mb = fake_heap_size >> 20;
  g_mem_so_mb     = so_region >> 20;
}

static void check_data(void) {
  struct stat st;
  if (stat(SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nCheck your data files.", SO_NAME);
  if (stat(CXX_DONOR_SO_NAME, &st) < 0)
    fatal_error("Could not find\n%s.\nCheck your data files.", CXX_DONOR_SO_NAME);
  // WAD accepted in ./assets/ or the game dir root (AAsset shim tries both)
  if (stat("assets/data_main.wad", &st) < 0 && stat("data_main.wad", &st) < 0)
    fatal_error("Could not find\ndata_main.wad.\n"
                "Put it in /switch/gtalcs/assets/\n(or the gtalcs folder itself).");
}

static void check_syscalls(void) {
  if (!envIsSyscallHinted(0x77))
    fatal_error("svcMapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x78))
    fatal_error("svcUnmapProcessCodeMemory is unavailable.");
  if (!envIsSyscallHinted(0x73))
    fatal_error("svcSetProcessMemoryPermission is unavailable.");
  if (envGetOwnProcessHandle() == INVALID_HANDLE)
    fatal_error("Own process handle is unavailable.");
}

static void set_screen_size(int w, int h) {
  if (w <= 0 || h <= 0 || w > 1920 || h > 1080) {
    // auto; pick resolution based on docked mode
    if (appletGetOperationMode() == AppletOperationMode_Console) {
      screen_width = 1920;
      screen_height = 1080;
    } else {
      screen_width = 1280;
      screen_height = 720;
    }
  } else {
    screen_width = w;
    screen_height = h;
  }
  debugPrintf("screen mode: %dx%d\n", screen_width, screen_height);
}

// gamepad: input is pushed through the GTAJNIlib JNI entry points.
// onJoyButtonDown/Up take a button index 0..15 (engine bounds-checks #0xf and
// indexes a button-state array); these indices map Switch buttons to engine actions.

#define GPAD_BUTTON_A 0
#define GPAD_BUTTON_B 1
#define GPAD_BUTTON_X 2
#define GPAD_BUTTON_Y 3
#define GPAD_BUTTON_START 4
#define GPAD_BUTTON_SELECT 5
#define GPAD_BUTTON_L1 6
#define GPAD_BUTTON_R1 7
#define GPAD_BUTTON_L2 8
#define GPAD_BUTTON_R2 9
#define GPAD_BUTTON_DPAD_LEFT 10
#define GPAD_BUTTON_DPAD_RIGHT 11
#define GPAD_BUTTON_DPAD_UP 12
#define GPAD_BUTTON_DPAD_DOWN 13
#define GPAD_BUTTON_THUMBL 14
#define GPAD_BUTTON_THUMBR 15

// joypad axis indices (setJoyAxis writes axisValues[axis]): lx, ly, rx, ry, lt, rt
#define GPAD_AXIS_LX 0
#define GPAD_AXIS_LY 1
#define GPAD_AXIS_RX 2
#define GPAD_AXIS_RY 3
#define GPAD_AXIS_LT 4
#define GPAD_AXIS_RT 5

typedef struct {
  u64 hid;
  int button;
} PadMap;

// Positional face buttons (Switch B = bottom = engine A confirm).
// Minus is handled via onBackButtonPressed below, not as a button index.
static const PadMap pad_map[] = {
  { HidNpadButton_B, GPAD_BUTTON_B },
  { HidNpadButton_A, GPAD_BUTTON_A },
  { HidNpadButton_Y, GPAD_BUTTON_Y },
  { HidNpadButton_X, GPAD_BUTTON_X },
  { HidNpadButton_L, GPAD_BUTTON_L1 },
  { HidNpadButton_R, GPAD_BUTTON_R1 },
  { HidNpadButton_ZL, GPAD_BUTTON_L2 },
  { HidNpadButton_ZR, GPAD_BUTTON_R2 },
  { HidNpadButton_Up, GPAD_BUTTON_DPAD_UP },
  { HidNpadButton_Down, GPAD_BUTTON_DPAD_DOWN },
  { HidNpadButton_Left, GPAD_BUTTON_DPAD_LEFT },
  { HidNpadButton_Right, GPAD_BUTTON_DPAD_RIGHT },
  { HidNpadButton_StickL, GPAD_BUTTON_THUMBL },
  { HidNpadButton_StickR, GPAD_BUTTON_THUMBR },
  { HidNpadButton_Plus, GPAD_BUTTON_START },
};

// JNI entry points of libGame.so (extern "C"; com.rockstargames.gtalcs.*).

// setup / lifecycle
static void  (* implTestJNI)(void *env, void *thiz);
static void  (* implSetOSVersion)(void *env, void *thiz, int osver);
static void  (* implSetDeviceInfo)(void *env, void *thiz, int perf, void *model, void *board, void *gpu);
static void  (* implSetIsTVDevice)(void *env, void *thiz, int is_tv);
static void  (* implSetHasVibrator)(void *env, void *thiz, int has);
static void  (* implSetNoJoysticks)(void *env, void *thiz, int n);
static void  (* implSetPrivateFilesDir)(void *env, void *thiz, void *path);
static void  (* implSetAssetManager)(void *env, void *thiz, void *mgr);
static void  (* implSetAssetPacksInfo)(void *env, void *thiz, void *names, void *paths);
static void  (* implSetFileInstalled)(void *env, void *thiz, void *name, int installed);
static void  (* implInitTouchSense)(void *env, void *thiz);
static void  (* implInitCloud)(void *env, void *thiz, void *rockstar_id, void *other);
static void  (* implMarkInitialized)(void *env, void *thiz);
static int   (* implIsInitialized)(void *env, void *thiz);
static int   (* implIsOnMainMenuScreen)(void *env, void *thiz);
static void *(* implGetGameBuildType)(void *env, void *thiz);
static void  (* implUpdateRockstarID)(void *env, void *thiz, void *id);

// surface / frame
static void  (* implViewOnSurfaceCreated)(void *env, void *thiz);
static void  (* implViewOnSurfaceChanged)(void *env, void *thiz, void *surface);
static void  (* implViewOnSurfaceDestroyed)(void *env, void *thiz);
static void  (* implViewOnDrawFrame)(void *env, void *thiz, float dt);
static void  (* implViewOnPause)(void *env, void *thiz);
static void  (* implViewOnResume)(void *env, void *thiz);

// input
static void  (* implOnTouchStart)(void *env, void *thiz, int id, float x, float y);
static void  (* implOnTouchMove)(void *env, void *thiz, int id, float x, float y);
static void  (* implOnTouchEnd)(void *env, void *thiz, int id, float x, float y);
static void  (* implOnJoyButtonDown)(void *env, void *thiz, int pad, int keycode);
static void  (* implOnJoyButtonUp)(void *env, void *thiz, int pad, int keycode);
static void  (* implSetJoyAxis)(void *env, void *thiz, int pad, int axis, float value);
static void  (* implSetAccelerometer)(void *env, void *thiz, float x, float y, float z);
static void  (* implOnBackButtonPressed)(void *env, void *thiz);

// rockstar / playlist callbacks (Java->native; driven by jni_fake event queue)
static void  (* implRsGameLoaded)(void *env, void *thiz);
static void  (* implRsStartGame)(void *env, void *thiz);
static void  (* implRsStartGameBeforeLoad)(void *env, void *thiz);
static void  (* implRsFinishGate)(void *env, void *thiz);
static void  (* implRsHandleStateChanged)(void *env, void *thiz, int signed_in);
static int   (* implCommonHandlePlaylistFinishInit)(void *env, void *thiz, int success);

// HAL callbacks
static void  (* implRunNativeRunnable)(void *env, void *thiz, int handle, int arg);
static void  (* implVideoFinishedCB)(void *env, void *thiz);

// CMenuManager singleton + start hooks. RequestFrontEndStartUp is normally
// fired by the Social Club SDK; with no SDK we drive it once loading ends.
static void  (* RequestFrontEndStartUp)(void *menumgr);
static void  (* LoadGameFromGate)(void *menumgr); // the load path's full game-start
static void  (* StartNewGame)(void *menumgr);     // new-game reset (destroys loaded state)
static void **menumgr_global; // *(module + 0x7f92c0)

static void resolve_entry_points(void) {
  #define ENT(var, sym) var = (void *)so_find_addr_rx(&game_mod, sym)
  #define GJ "Java_com_rockstargames_gtalcs_GTAJNIlib_"
  #define RJ "Java_com_rockstargames_gtalcs_RockstarJNIlib_"
  ENT(implTestJNI, GJ "TestJNI");
  ENT(implSetOSVersion, GJ "setOSVersion");
  ENT(implSetDeviceInfo, GJ "setDeviceInfo");
  ENT(implSetIsTVDevice, GJ "setIsTVDevice");
  ENT(implSetHasVibrator, GJ "setHasVibrator");
  ENT(implSetNoJoysticks, GJ "setNoJoysticks");
  ENT(implSetPrivateFilesDir, GJ "setPrivateFilesDir");
  ENT(implSetAssetManager, GJ "setAssetManager");
  ENT(implSetAssetPacksInfo, GJ "setAssetPacksInfo");
  ENT(implSetFileInstalled, GJ "setFileInstalled");
  ENT(implInitCloud, GJ "initCloud");
  ENT(implMarkInitialized, GJ "markInitialized");
  ENT(implIsInitialized, GJ "isInitialized");
  ENT(implIsOnMainMenuScreen, GJ "isOnMainMenuScreen");
  ENT(implGetGameBuildType, GJ "getGameBuildType");
  ENT(implUpdateRockstarID, GJ "updateRockstarID");
  ENT(implViewOnSurfaceCreated, GJ "viewOnSurfaceCreated");
  ENT(implViewOnSurfaceChanged, GJ "viewOnSurfaceChanged");
  ENT(implViewOnSurfaceDestroyed, GJ "viewOnSurfaceDestroyed");
  ENT(implViewOnDrawFrame, GJ "viewOnDrawFrame");
  ENT(implViewOnPause, GJ "viewOnPause");
  ENT(implViewOnResume, GJ "viewOnResume");
  ENT(implOnTouchStart, GJ "onTouchStart");
  ENT(implOnTouchMove, GJ "onTouchMove");
  ENT(implOnTouchEnd, GJ "onTouchEnd");
  ENT(implOnJoyButtonDown, GJ "onJoyButtonDown");
  ENT(implOnJoyButtonUp, GJ "onJoyButtonUp");
  ENT(implSetJoyAxis, GJ "setJoyAxis");
  ENT(implSetAccelerometer, GJ "setAccelerometer");
  ENT(implOnBackButtonPressed, GJ "onBackButtonPressed");
  ENT(implInitTouchSense, "Java_com_rockstargames_gtalcs_GTAActivity_initTouchSense");
  ENT(implRsGameLoaded, RJ "GameLoaded");
  ENT(implRsStartGame, RJ "StartGame");
  ENT(implRsStartGameBeforeLoad, RJ "StartGameBeforeLoad");
  ENT(implRsFinishGate, RJ "FinishGate");
  ENT(implRsHandleStateChanged, RJ "HandleStateChanged");
  ENT(implCommonHandlePlaylistFinishInit, "Java_com_rockstargames_gtalcs_CommonAPI_HandlePlaylistFinishInit");
  ENT(implRunNativeRunnable, "Java_com_rockstargames_hal_andThread_runNativeRunnable");
  ENT(implVideoFinishedCB, "Java_com_rockstargames_hal_andVideo_VideoFinishedCB");
  #undef ENT
  RequestFrontEndStartUp = (void *)so_try_find_addr_rx(&game_mod, "_ZN12CMenuManager22RequestFrontEndStartUpEv");
  LoadGameFromGate = (void *)so_try_find_addr_rx(&game_mod, "_ZN12CMenuManager16LoadGameFromGateEv");
  StartNewGame = (void *)so_try_find_addr_rx(&game_mod, "_ZN12CMenuManager12StartNewGameEv");
  menumgr_global = (void **)((char *)game_mod.load_virtbase + 0x7f9000 + 704);
  #undef GJ
  #undef RJ
  (void)implIsInitialized;
  (void)implIsOnMainMenuScreen;
  (void)implGetGameBuildType;
  (void)implUpdateRockstarID;
  (void)implInitTouchSense;
}

// ---------------------------------------------------------------------------
// deferred Java->native callbacks queued by the fake HAL (jni_fake.c)
// ---------------------------------------------------------------------------

static void dispatch_callbacks(void) {
  JniCallback cb;
  int n = 0;
  while (n++ < 32 && jni_pop_callback(&cb)) {
    switch (cb.type) {
      case JNI_CB_RS_FINISH_GATE:
        debugPrintf("cb: RockstarJNIlib.FinishGate\n");
        implRsFinishGate(fake_env, NULL);
        break;
      case JNI_CB_RS_START_BEFORE_LOAD:
        debugPrintf("cb: RockstarJNIlib.StartGameBeforeLoad\n");
        implRsStartGameBeforeLoad(fake_env, NULL);
        break;
      case JNI_CB_RS_START_GAME:
        debugPrintf("cb: RockstarJNIlib.StartGame\n");
        // New Game: hide save files (g_hide_saves) for the duration so the engine
        // finds no save and builds fresh; restored once in-game (state 9) or on
        // timeout. StartNewGame + LoadGameFromGate drive the menu->GameStart advance.
        g_hide_saves = 1;
        g_hide_saves_frames = 1800;           // ~30s safety auto-restore
        if (StartNewGame && LoadGameFromGate && menumgr_global && *menumgr_global) {
          void *mm = *menumgr_global;
          StartNewGame(mm);
          LoadGameFromGate(mm);
          ((volatile uint8_t *)mm)[21] = 0;   // 0 = new game, not load
          debugPrintf("cb: New Game -> saves hidden + advance (start fresh)\n");
        } else {
          implRsStartGame(fake_env, NULL);    // fallback
        }
        break;
      case JNI_CB_RS_GAME_LOADED:
        debugPrintf("cb: RockstarJNIlib.GameLoaded\n");
        implRsGameLoaded(fake_env, NULL);
        break;
      case JNI_CB_RS_STATE_CHANGED:
        debugPrintf("cb: RockstarJNIlib.HandleStateChanged(%d)\n", cb.arg0);
        implRsHandleStateChanged(fake_env, NULL, cb.arg0);
        break;
      case JNI_CB_VIDEO_FINISHED:
        debugPrintf("cb: andVideo.VideoFinishedCB\n");
        implVideoFinishedCB(fake_env, NULL);
        break;
      case JNI_CB_PLAYLIST_FINISH_INIT:
        debugPrintf("cb: CommonAPI.HandlePlaylistFinishInit(%d)\n", cb.arg0);
        implCommonHandlePlaylistFinishInit(fake_env, NULL, cb.arg0);
        break;
      default:
        break;
    }
  }
}

static void dispatch_runnables(void) {
  JniRunnable r;
  int n = 0;
  // run queued runnables on the main thread (background ones run here too, just
  // not concurrently).
  while (n++ < 32 && jni_pop_runnable(&r)) {
    debugPrintf("runnable: handle=%d arg=%d\n", r.handle, r.arg);
    implRunNativeRunnable(fake_env, NULL, r.handle, r.arg);
  }
}

#define MAX_TOUCHES 4

typedef struct {
  int active;
  u32 finger_id;
  float x, y; // normalized
} TouchSlot;

static TouchSlot touch_prev[MAX_TOUCHES];

static int touch_slot_find(u32 finger_id) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (touch_prev[i].active && touch_prev[i].finger_id == finger_id)
      return i;
  return -1;
}

static int touch_slot_alloc(void) {
  for (int i = 0; i < MAX_TOUCHES; i++)
    if (!touch_prev[i].active)
      return i;
  return -1;
}

static void update_touch(void) {
  HidTouchScreenState state = { 0 };
  if (!hidGetTouchScreenStates(&state, 1))
    return;

  // libnx reports touch in the panel's native 1280x720 space; normalize to 0..1
  const float inv_w = 1.0f / 1280.0f;
  const float inv_h = 1.0f / 720.0f;

  int seen[MAX_TOUCHES] = { 0 };
  for (int i = 0; i < state.count; i++) {
    const HidTouchState *t = &state.touches[i];
    const float x = (float)t->x * inv_w;
    const float y = (float)t->y * inv_h;
    int slot = touch_slot_find(t->finger_id);
    if (slot < 0) {
      slot = touch_slot_alloc();
      if (slot < 0)
        continue;
      touch_prev[slot].active = 1;
      touch_prev[slot].finger_id = t->finger_id;
      implOnTouchStart(fake_env, NULL, slot, x, y);
    } else if (x != touch_prev[slot].x || y != touch_prev[slot].y) {
      implOnTouchMove(fake_env, NULL, slot, x, y);
    }
    touch_prev[slot].x = x;
    touch_prev[slot].y = y;
    seen[slot] = 1;
  }

  for (int slot = 0; slot < MAX_TOUCHES; slot++) {
    if (touch_prev[slot].active && !seen[slot]) {
      implOnTouchEnd(fake_env, NULL, slot, touch_prev[slot].x, touch_prev[slot].y);
      touch_prev[slot].active = 0;
    }
  }
}

static PadState pad;
static u64 pad_prev = 0;

static void update_gamepad(void) {
  padUpdate(&pad);
  const u64 down = padGetButtons(&pad);
  const u64 changed = down ^ pad_prev;

  for (unsigned int i = 0; i < sizeof(pad_map) / sizeof(*pad_map); i++) {
    if (changed & pad_map[i].hid) {
      if (down & pad_map[i].hid) {
        implOnJoyButtonDown(fake_env, NULL, 0, pad_map[i].button);
        movie_skip(); // the game ignores input while waiting for a movie
      } else {
        implOnJoyButtonUp(fake_env, NULL, 0, pad_map[i].button);
      }
    }
  }
  // Minus -> the engine's back/pause handler, on the press edge
  if ((changed & HidNpadButton_Minus) && (down & HidNpadButton_Minus)) {
    implOnBackButtonPressed(fake_env, NULL);
    movie_skip();
  }
  pad_prev = down;

  const float scale = 1.f / 32767.0f;
  const HidAnalogStickState ls = padGetStickPos(&pad, 0);
  const HidAnalogStickState rs = padGetStickPos(&pad, 1);
  // Android Y axes point down (engine applies its own deadzone of 0.125)
  const float axes[6] = {
    (float)ls.x * scale, (float)ls.y * -scale,
    (float)rs.x * scale, (float)rs.y * -scale,
    (down & HidNpadButton_ZL) ? 1.0f : 0.0f,
    (down & HidNpadButton_ZR) ? 1.0f : 0.0f,
  };

  static float prev_axes[6];
  static const int axis_idx[6] = {
    GPAD_AXIS_LX, GPAD_AXIS_LY, GPAD_AXIS_RX, GPAD_AXIS_RY, GPAD_AXIS_LT, GPAD_AXIS_RT
  };
  for (int i = 0; i < 6; i++) {
    if (axes[i] != prev_axes[i]) {
      prev_axes[i] = axes[i];
      implSetJoyAxis(fake_env, NULL, 0, axis_idx[i], axes[i]);
    }
  }
}

int main(void) {
  // run the CPU at full clock for boot; dropped back to normal once the menu
  // has rendered a few frames (see the main loop)
  cpu_boost(1);

  // Force all GL onto the calling thread by disabling mesa's internal threading:
  // a worker thread running GL commands races nouveau's small-buffer allocator
  // (nouveau_mm) against our glFinish drain and corrupts its pool. Set before GL init.
  setenv("MESA_GLTHREAD", "false", 1);
  setenv("mesa_glthread", "false", 1);
  setenv("GALLIUM_THREAD", "0", 1);

  // read config, writing defaults if it's missing
  if (read_config(CONFIG_NAME) < 0)
    write_config(CONFIG_NAME);

  check_syscalls();
  check_data();

  // Social Club / cloud netcode uses BSD sockets; bring the stack up. Offline
  // play must still work if this fails, so log but don't abort.
  Result sock_rc = socketInitializeDefault();
  if (R_FAILED(sock_rc))
    debugPrintf("socketInitializeDefault failed: %08x (networking disabled)\n", sock_rc);

  set_screen_size(config.screen_width, config.screen_height);

  debugPrintf("mem: total=%zu MB | newlib(game+mesa+GPU)=%zu MB | .so region=%zu MB\n",
              g_mem_total_mb, g_mem_newlib_mb, g_mem_so_mb);
  debugPrintf("nv GPU transfermem pool = %u MB\n",
              (unsigned)(__nx_nv_transfermem_size >> 20));
  debugPrintf(" lib base = %p\n", heap_so_base);
  debugPrintf("  lib max = %u KB\n", heap_so_limit / 1024);

  // load the C++ runtime donor first, then the game; the game's std::/__cxa_
  // imports resolve into the donor's statically-linked libc++
  if (so_load(&donor_mod, CXX_DONOR_SO_NAME, heap_so_base, heap_so_limit) < 0)
    fatal_error("Could not load\n%s.", CXX_DONOR_SO_NAME);

  void *game_base = (void *)ALIGN_MEM((uintptr_t)heap_so_base + donor_mod.load_size, 0x1000);
  const size_t game_limit = heap_so_limit - ((uintptr_t)game_base - (uintptr_t)heap_so_base);
  if (so_load(&game_mod, SO_NAME, game_base, game_limit) < 0)
    fatal_error("Could not load\n%s.", SO_NAME);

  update_imports();

  so_relocate(&donor_mod);
  so_relocate(&game_mod);
  so_resolve(&donor_mod, dynlib_functions, dynlib_numfunctions, 1);
  so_resolve(&game_mod, dynlib_functions, dynlib_numfunctions, 1);

  patch_game();
  movie_wad_init();   // resolve cFile syms now (symtab is freed by so_flush_caches)

  // can't set it in the initializer because it's not constant
  stderr_fake = stderr;

  resolve_entry_points();
  int (* JNI_OnLoad)(void *vm, void *reserved) = (void *)so_find_addr_rx(&game_mod, "JNI_OnLoad");
  void (* NVThreadInit)(void *vm) = (void *)so_find_addr_rx(&game_mod, "_Z12NVThreadInitP7_JavaVM");

  so_finalize(&donor_mod);
  so_finalize(&game_mod);
  so_flush_caches(&donor_mod);
  so_flush_caches(&game_mod);

  // the donor's initializers (libc++ statics) must run before the game's
  so_execute_init_array(&donor_mod);
  so_execute_init_array(&game_mod);

  so_free_temp(&donor_mod);
  so_free_temp(&game_mod);

  jni_init();
  NVThreadInit(fake_vm);

  debugPrintf("calling JNI_OnLoad\n");
  JNI_OnLoad(fake_vm, NULL);

  // ---- setup sequence (replaces the Android GTAActivity/GameThread setup) ----
  void *asset_mgr = jni_make_object("AssetManager");
  void *surface = jni_make_object("Surface");

  debugPrintf("setup: TestJNI / device / paths\n");
  implTestJNI(fake_env, NULL);
  implSetOSVersion(fake_env, NULL, 30);
  // (perfHint, model, board, gpu) -- high perf hint avoids the lowest detail tier
  implSetDeviceInfo(fake_env, NULL, 2,
      jni_make_string("Switch"),
      jni_make_string("nx"),
      jni_make_string("NVIDIA Tegra X1"));
  implSetIsTVDevice(fake_env, NULL, 1);
  implSetHasVibrator(fake_env, NULL, 0);
  // setNoJoysticks = "set NUMBER of joysticks", not a boolean: stored as
  // joypadsConnected, which gates the analog-stick read path (count<1 -> stick
  // dead in-game). 1 == one controller present.
  implSetNoJoysticks(fake_env, NULL, 1);

  // "." anchors the engine's file roots to the game directory (cwd)
  implSetPrivateFilesDir(fake_env, NULL, jni_make_string("."));
  implSetAssetManager(fake_env, NULL, asset_mgr);

  // Register asset packs. data_main.wad holds models/textures; audio lives in a
  // separate data_music.wad. The engine only mounts packs declared installed,
  // and missing music WAD -> retries AUDIO/MUSIC/*.mp3 forever -> black-screen
  // hang, so only declare data_music when its WAD is actually present.
  struct stat mwst;
  const int have_music = (stat("data_music.wad", &mwst) == 0 ||
                          stat("assets/data_music.wad", &mwst) == 0);
  if (have_music) {
    const char *pack_names[] = { "data_main", "data_music" };
    const char *pack_paths[] = { ".", "." };
    implSetAssetPacksInfo(fake_env, NULL,
        jni_make_string_array(2, pack_names),
        jni_make_string_array(2, pack_paths));
    implSetFileInstalled(fake_env, NULL, jni_make_string("data_main"), 1);
    implSetFileInstalled(fake_env, NULL, jni_make_string("data_music"), 1);
    debugPrintf("packs: data_main + data_music (music WAD found -> audio enabled)\n");
  } else {
    const char *pack_names[] = { "data_main" };
    const char *pack_paths[] = { "." };
    implSetAssetPacksInfo(fake_env, NULL,
        jni_make_string_array(1, pack_names),
        jni_make_string_array(1, pack_paths));
    implSetFileInstalled(fake_env, NULL, jni_make_string("data_main"), 1);
    debugPrintf("packs: data_main only (data_music.wad NOT found -> audio absent)\n");
  }

  implInitTouchSense(fake_env, NULL);
  implInitCloud(fake_env, NULL, jni_make_string(""), jni_make_string(""));
  implMarkInitialized(fake_env, NULL);

  // surface bring-up: viewOnSurfaceChanged does the full EGL setup and reads the
  // render size from ANativeWindow_getWidth/getHeight (our shim reports screen_*)
  debugPrintf("surface: viewOnSurfaceCreated / viewOnSurfaceChanged\n");
  implViewOnSurfaceCreated(fake_env, NULL);
  implViewOnSurfaceChanged(fake_env, NULL, surface);
  implViewOnResume(fake_env, NULL);

  padConfigureInput(8, HidNpadStyleSet_NpadStandard);
  padInitializeAny(&pad);
  hidInitializeTouchScreen();

  const u64 tick_freq = armGetSystemTickFreq();
  u64 last_tick = armGetSystemTick();
  int boot_frames = 0;
  u64 frame_count = 0;

  int frontend_started = 0;
  int frontend_delay = 0;
  int last_app_state = -99;
  int frontend_shown = 0;

  while (appletMainLoop() && !jni_quit_requested) {
    frame_count++;
    dispatch_callbacks();
    dispatch_runnables();

    // Engine boot is a state machine at *(*(module+0x7fd8b8)), states 0..9:
    // 7 = main menu, 8 = GameStart (CGame::Initialise builds the world + gameplay
    // pools), 9 = in-game. It stalls at state 1 because two gates the absent Social
    // Club SDK normally sets never fire; set both and let it boot through state 8:
    //    state 1 -> byte at module+0xa435b0  (playlist-init-done)
    //    state 2 -> CMenuManager[+40]         (the "GameLoaded" flag)
    if (!frontend_started && jni_frontend_ready) {
      if (++frontend_delay >= 20) {
        *(volatile uint8_t *)((char *)game_mod.load_virtbase + 0xa435b0) = 1;
        void *mm = menumgr_global ? *menumgr_global : NULL;
        if (mm)
          ((volatile uint8_t *)mm)[40] = 1;
        debugPrintf("BOOT: unblocked boot gates (playlist + GameLoaded); menuMgr=%p\n", mm);
        frontend_started = 1;
      }
    }

    // Watch the boot state machine; at the main-menu state (7), kick the frontend
    // (RequestFrontEndStartUp has no normal caller without the SC SDK).
    if (frontend_started) {
      uint32_t **scpp = (uint32_t **)((char *)game_mod.load_virtbase + 0x7fd8b8);
      uint32_t *scstate = *scpp;
      int st = scstate ? (int)scstate[0] : -1;
      // In-game (state 9) "Load Game": CMenuManager::Process gates the load at
      // +0x2b on a "gate-before-load requested" flag that ShowRockstarGateBeforeLoad
      // sets on the first load and never clears, so later loads freeze. Clear it
      // while in-game; state 7 (main menu) is left alone so its reshow guard works.
      if (st == 9 && menumgr_global && *menumgr_global)
        ((volatile uint8_t *)*menumgr_global)[0x2b] = 0;
      if (st != last_app_state) {
        debugPrintf("BOOT: app state %d -> %d (frame %llu)\n",
            last_app_state, st, (unsigned long long)frame_count);
        last_app_state = st;
        if (st == 9 && g_hide_saves) {
          g_hide_saves = 0;   // in-game now: restore saves so save/load works again
          debugPrintf("New Game: in-game -> saves restored\n");
        }
        if (st == 7 && !frontend_shown) {
          void *mm = menumgr_global ? *menumgr_global : NULL;
          if (RequestFrontEndStartUp && mm) {
            RequestFrontEndStartUp(mm);
            debugPrintf("BOOT: state 7 (menu) -> RequestFrontEndStartUp(%p)\n", mm);
          }
          frontend_shown = 1;
        }
      }
    }

    // safety auto-restore for the New Game save-hide, in case state 9 is missed
    if (g_hide_saves && g_hide_saves_frames > 0 && --g_hide_saves_frames == 0) {
      g_hide_saves = 0;
      debugPrintf("New Game: save-hide timed out -> saves restored\n");
    }

    update_gamepad();
    update_touch();

    const u64 now = armGetSystemTick();
    float dt = (float)(now - last_tick) / (float)tick_freq;
    last_tick = now;
    if (dt <= 0.0f || dt > 0.5f)
      dt = 1.0f / 60.0f;

    if (frame_count <= 3)
      debugPrintf("frame %llu: -> viewOnDrawFrame\n", (unsigned long long)frame_count);
    implViewOnDrawFrame(fake_env, NULL, dt);
    if (frame_count <= 3 || frame_count % 120 == 0) {
      const GLenum e = glGetError();
      debugPrintf("frame %llu: drawn, glerr=0x%04x\n", (unsigned long long)frame_count, e);
      if (frame_count == 1) {
        const GLubyte *ver = glGetString(GL_VERSION);
        const GLubyte *ren = glGetString(GL_RENDERER);
        debugPrintf("GL_VERSION=%s\nGL_RENDERER=%s\n",
            ver ? (const char *)ver : "(null)", ren ? (const char *)ren : "(null)");
        GLint vp[4] = { 0 };
        glGetIntegerv(GL_VIEWPORT, vp);
        debugPrintf("GL_VIEWPORT = %d,%d,%d,%d\n", vp[0], vp[1], vp[2], vp[3]);
      }
    }

    // keeps movie playback going when the game stops rendering during it
    movie_main_loop_tick();
    jni_video_tick();   // fire VIDEO_FINISHED once a callback-path movie ends
    worldstream_probe(); // re-poke the UMD streamer if a request stalls

    if (boot_frames < 10) {
      if (++boot_frames == 10)
        cpu_boost(0);
    }
  }

  // log why the loop ended (clean exits are almost always one of these two)
  debugPrintf("BOOT: main loop exited after %llu frames (reason: %s)\n",
      (unsigned long long)frame_count,
      jni_quit_requested ? "the game requested quit (CommonAPI.QuitApp)"
                         : "appletMainLoop() returned false (applet closed / backgrounded)");

  debugPrintf("shutting down\n");
  implViewOnPause(fake_env, NULL);
  implViewOnSurfaceDestroyed(fake_env, NULL);

  movie_stop();
  deinit_openal();
  socketExit();

  extern void NX_NORETURN __libnx_exit(int rc);
  __libnx_exit(0);

  return 0;
}
