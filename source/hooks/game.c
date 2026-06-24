/* game.c -- hooks and patches for everything other than AL and GL
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * GTA: Liberty City Stories (2.4.379) uses the OLDER Rockstar Android engine
 * (GTAJNIlib / com.rockstargames.hal.and* HAL, shared with GTA III/VC/SA
 * mobile), NOT the oswrapper GameNative framework of CTW/Max Payne. The thread
 * and TLS plumbing is the same NVThread layer, so those hooks carry over with
 * the same mangled names; the OS_Thread* / OS_ScreenGet* lifecycle CTW hooked
 * is absent here (inlined) -- LCS spawns every engine thread through
 * NVThreadSpawnJNIThread and takes the screen size from the ANativeWindow.
 * Mangled names below were verified against this exact libGame.so build.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <threads.h>
#include <pthread.h>
#include <switch.h>

#include "../util.h"
#include "../so_util.h"
#include "../hooks.h"
#include "../jni_fake.h"

extern so_module game_mod; // defined in main.c

// the game spawns its loader/sound/stream threads through this NVThread
// wrapper; each new thread needs a fake TLS block installed in TPIDR_EL0 for
// the stack-guard cookie reads (see init_fake_tls below)
typedef struct {
  void *(*func)(void *);
  void *arg;
  uint8_t tls[0x100];
} NVThreadStart;

static uint8_t main_fake_tls[0x100];

static void init_fake_tls(uint8_t *tls) {
  memset(tls, 0, 0x100);
  armSetTlsRw(tls);
}

static int nv_thread_trampoline(void *arg) {
  NVThreadStart *start = arg;
  void *(*func)(void *) = start->func;
  void *user_arg = start->arg;
  init_fake_tls(start->tls);
  void *rc = func(user_arg);
  // start->tls stays installed in TPIDR_EL0 through thread teardown, so the
  // block is leaked on purpose (0x100+ bytes per spawned thread)
  return (int)(intptr_t)rc;
}

// _Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_
// NVThreadSpawnJNIThread(long*, pthread_attr_t const*, char const*,
//                        void* (*)(void*), void*)
int NVThreadSpawnJNIThread(long *tid, const void *attr, const char *name, void *(*fn)(void *), void *arg) {
  (void)attr;
  debugPrintf("NVThreadSpawnJNIThread: %s\n", name ? name : "(unnamed)");
  NVThreadStart *start = calloc(1, sizeof(*start));
  if (!start)
    return -1;
  start->func = fn;
  start->arg = arg;
  thrd_t thrd;
  if (thrd_create(&thrd, nv_thread_trampoline, start) != thrd_success) {
    free(start);
    return -1;
  }
  if (tid)
    *tid = (long)thrd;
  return 0;
}

// always hand out our fake JNIEnv; the real one TLS-caches an env pointer
// that we can't provide
void *NVThreadGetCurrentJNIEnv(void) {
  return fake_env;
}

// ---------------------------------------------------------------------------
// streaming thread fix
//
// cUmdStream::UmdThread (the data-streaming worker) can wake on an EMPTY request
// queue and process the circular-list SENTINEL as a real request, reading a
// "device" pointer past the end of the object (heap garbage) and faulting. The
// original `if (head == 0)` empty-check never catches the sentinel (non-NULL).
// Benign on Android, faults here.
//
// Reimplemented faithfully with the one missing check: if the dequeued head IS
// the sentinel, treat the queue as empty and wait again. Offsets from the
// disasm of this build:
//   cUmdStream:  +0x50 queue mutex (pthread_mutex_t* via our fake),
//                +5272 list sentinel (next@+0, prev@+8), +5288 head
//   cUmdRequest: +24 buffer, +32 device, +40 offset, +44 size, +48 result,
//                +60 result-copy, +72 completion callback
// ---------------------------------------------------------------------------

static int  (*sce_ClearEventFlag)(unsigned int id, unsigned int bits);
static int  (*sce_SetEventFlag)(unsigned int id, unsigned int bits);
static int  (*sce_WaitEventFlag)(unsigned int id, unsigned int pattern, unsigned int mode, unsigned int *outbits, void *timeout);
static int  (*cfile_read)(void *cfile, void *buf, unsigned int size);
static volatile unsigned int *umd_eventflag_id; // engine global, module +0x875e20
static unsigned umd_req_total;
void *g_umd_instance = NULL;   // the cUmdStream singleton, captured at thread start

#define UMD_MUTEX(s)    (*(pthread_mutex_t **)((char *)(s) + 0x50))
#define UMD_SENTINEL(s) ((void *)((char *)(s) + 5272))
#define UMD_HEADP(s)    ((void **)((char *)(s) + 5288))

static void UmdThread_fixed(void *self) {
  g_umd_instance = self;   // for the main-loop stuck-request kick
  void *const sentinel = UMD_SENTINEL(self);
  void **const headp = UMD_HEADP(self);
  const unsigned int flagid = *umd_eventflag_id;

  goto wait_for_work; // first iteration skips the clear/idle-signal

  for (;;) {
clear_top:
    sce_ClearEventFlag(flagid, 0xfffffffeu); // clear bit0 (request-available)
unlock_and_wait:
    pthread_mutex_unlock(UMD_MUTEX(self));
    sce_SetEventFlag(flagid, 2);             // set bit1 (consumer idle)
wait_for_work:
    sce_WaitEventFlag(flagid, 1, 1, 0, 0);   // wait for bit0
    { static unsigned w = 0;
      if (++w <= 48 || (w & 0x3f) == 0)
        debugPrintf("UmdThread woke #%u\n", w); }

    void *head = *headp;
    if (head == NULL) {
      pthread_mutex_lock(UMD_MUTEX(self));
      head = *(void **)sentinel;             // queue.next
      if (head == sentinel)                  // <-- THE FIX: empty queue
        head = NULL;
      *headp = head;
      pthread_mutex_unlock(UMD_MUTEX(self));
      if (head == NULL)
        goto check_empty;
    }

    // ---- process the request ----
    pthread_mutex_lock(UMD_MUTEX(self));
    head = *headp;
    const int size = *(int *)((char *)head + 44);
    int result = 0;
    if (size != 0) {
      void *device = *(void **)((char *)head + 32);
      umd_req_total++;
      if (umd_req_total <= 16 || (umd_req_total & 0x3f) == 0)
        debugPrintf("UMD[%u]: dev=%p size=%d off=%d\n", umd_req_total, device,
            size, *(int *)((char *)head + 40));
      const long offset = *(int *)((char *)head + 40); // sign-extended
      void *cfile = *(void **)((char *)device + 16);
      void *seekobj = *(void **)cfile;                 // cfile->[0]
      void **vt = *(void ***)seekobj;                  // its vtable
      ((void (*)(void *, long))vt[4])(seekobj, offset);
      void *buffer = *(void **)((char *)(*headp) + 24);
      result = cfile_read(cfile, buffer, (unsigned int)size);
      if (umd_req_total <= 16 || (umd_req_total & 0x3f) == 0)
        debugPrintf("UMD[%u] read done r=%d\n", umd_req_total, result);
    }
    head = *headp;
    *(int *)((char *)head + 48) = result;
    *(int *)((char *)head + 44) = 0;
    *(int *)((char *)head + 60) = *(int *)((char *)head + 48);
    // unlink the node from the list
    void *next = ((void **)head)[0];
    void *prev = ((void **)head)[1];
    ((void **)next)[1] = prev;
    ((void **)prev)[0] = next;
    ((void **)head)[0] = NULL;
    // completion callback (run with the lock released)
    void *cb = *(void **)((char *)head + 72);
    if (cb) {
      pthread_mutex_unlock(UMD_MUTEX(self));
      head = *headp;
      ((void (*)(void *))(*(void **)((char *)head + 72)))(head);
      pthread_mutex_lock(UMD_MUTEX(self));
    }
    {
      void *qn = *(void **)sentinel; // queue.next
      *headp = NULL;
      if (qn != sentinel)
        goto unlock_and_wait;
      else
        goto clear_top;
    }

check_empty:
    pthread_mutex_lock(UMD_MUTEX(self));
    if (*(void **)sentinel != sentinel)
      goto unlock_and_wait;
    else
      goto clear_top;
  }
}

static void patch_umd_stream(void) {
  uintptr_t umd = so_try_find_addr_rx(&game_mod, "_ZN10cUmdStream9UmdThreadEv");
  if (!umd)
    return; // streaming thread not present in this build; nothing to fix
  sce_ClearEventFlag = (void *)so_try_find_addr_rx(&game_mod, "_Z23sceKernelClearEventFlagjj");
  sce_SetEventFlag   = (void *)so_try_find_addr_rx(&game_mod, "_Z21sceKernelSetEventFlagjj");
  sce_WaitEventFlag  = (void *)so_try_find_addr_rx(&game_mod, "_Z22sceKernelWaitEventFlagjjjPjS_");
  cfile_read         = (void *)so_try_find_addr_rx(&game_mod, "_ZN5cFile4readEPvj");
  umd_eventflag_id   = (volatile unsigned int *)((char *)game_mod.load_virtbase + 0x875e20);
  if (!sce_ClearEventFlag || !sce_SetEventFlag || !sce_WaitEventFlag || !cfile_read) {
    debugPrintf("UMD: missing helper symbol, leaving streaming thread unpatched\n");
    return;
  }
  hook_arm64(so_find_addr(&game_mod, "_ZN10cUmdStream9UmdThreadEv"), (uintptr_t)UmdThread_fixed);
  debugPrintf("UMD: streaming thread reimplemented (empty-queue fix)\n");
}

// ---------------------------------------------------------------------------
// StreamThread::ThreadMain -- the main asset streamer. Two hazards on this port,
// both reimplemented away:
//  * empty ring after a semaphore wake -> the original processes request[-1];
//  * a queued request with a NULL device pointer (+0x20) from a failed texture
//    open -> the original dereferences NULL.
// We skip both instead of faulting. Globals/offsets read from the disasm:
//   module +0x9ec3c0 : ring control  [head:int @0, tail:int @4, size:int @8]
//   module +0x9ec3b0 : -> request array (stride 0x28)
//   module +0x9ec3b8 : -> ring buffer (int request indices)
//   module +0x9ec3d0 : -> Platform::Semaphore (the stream-request semaphore)
//   request: +0 sector, +4 size, +8 buffer, +17 needs-signal, +18 mark,
//            +20 result, +24 completion sem, +32 device (vtbl: [4]=seek,[2]=read)
// ---------------------------------------------------------------------------

static void (*plat_sem_down)(void *sem);
static void (*plat_sem_up)(void *sem);
static volatile int32_t *st_ring;   // module+0x9ec3c0
static void **st_reqarr_pp;         // module+0x9ec3b0
static int32_t **st_ringbuf_pp;     // module+0x9ec3b8
static void **st_sem_pp;            // module+0x9ec3d0
static int st_null_warned;
static unsigned st_req_total, st_req_null;

static void StreamThread_fixed(void *self) {
  (void)self;
  volatile int32_t *ring = st_ring;     // [0]=head [1]=tail [2]=size
  unsigned char *prev_mark = NULL;

  for (;;) {
    if (prev_mark) { *prev_mark = 0; prev_mark = NULL; }

    plat_sem_down(*st_sem_pp);
    { static unsigned w = 0;
      if (++w <= 48 || (w & 0x3f) == 0)
        debugPrintf("StreamThread woke #%u head=%d tail=%d size=%d\n", w,
                    ring[0], ring[1], ring[2]); }

    // The semaphore is posted once per request, but on this port the post can
    // become visible BEFORE the matching ring write (tail++) lands. Waking to an
    // empty ring means the request is mid-enqueue: wait briefly instead of
    // discarding the token. Discarding it (the old bare `continue`) stranded the
    // request and deadlocked the whole game at scene/skip load. A genuinely
    // spurious post just times out and is absorbed by the `continue` below, so
    // the token balance holds either way.
    if (ring[0] == ring[1]) {
      for (int spins = 0; ring[0] == ring[1] && spins < 100; spins++)
        svcSleepThread(100000ULL);       // 0.1ms x up to 100 = ~10ms grace
    }
    __sync_synchronize();                // acquire: see the producer's req writes
    const int head = ring[0];
    if (head == ring[1])
      continue;                          // truly empty -> re-wait on the semaphore

    const int index = (*st_ringbuf_pp)[head];
    unsigned char *req = (unsigned char *)*st_reqarr_pp + (int64_t)index * 0x28;
    req[18] = 1;
    prev_mark = req + 18;

    if (*(int32_t *)(req + 20) == 0) {
      void *device = *(void **)(req + 32);
      st_req_total++;
      if (!device) st_req_null++;
      if (st_req_total <= 16 || (st_req_total & 0x7f) == 0)
        debugPrintf("STREAM[%u]: dev=%p sec=%d nsec=%d (nulls=%u)\n",
            st_req_total, device, *(int32_t *)(req + 0), *(int32_t *)(req + 4), st_req_null);
      if (device) {
        void **vt = *(void ***)device;
        const int32_t sector = *(int32_t *)(req + 0);
        ((void (*)(void *, long))vt[4])(device, (long)(int32_t)((uint32_t)sector << 11));
        void *buf = *(void **)(req + 8);
        const int32_t nsec = *(int32_t *)(req + 4);
        const int r = ((int (*)(void *, void *, int))vt[2])(
            device, buf, (int)((uint32_t)nsec << 11));
        if (st_req_total <= 16 || (st_req_total & 0x3f) == 0)
          debugPrintf("STREAM[%u] read done r=%d\n", st_req_total, r);
        *(int32_t *)(req + 20) = (r == 0) ? 254 : 0;
      } else {
        if (!st_null_warned) { st_null_warned = 1; debugPrintf("STREAM: NULL-device request skipped (texture not found)\n"); }
        *(int32_t *)(req + 20) = 254;     // mark failed instead of faulting
      }
    }

    if (ring[0] != ring[1]) {
      const int size = ring[2];
      if (size > 0)
        ring[0] = (ring[0] + 1) % size;
    }
    *(int32_t *)(req + 4) = 0;
    if (req[17] != 0)
      plat_sem_up(*(void **)(req + 24));
  }
}

static void patch_stream_thread(void) {
  uintptr_t st = so_try_find_addr_rx(&game_mod, "_ZN12StreamThread10ThreadMainEv");
  if (!st)
    return;
  plat_sem_down  = (void *)so_try_find_addr_rx(&game_mod, "_ZN8Platform9Semaphore4DownEv");
  plat_sem_up    = (void *)so_try_find_addr_rx(&game_mod, "_ZN8Platform9Semaphore2UpEv");
  st_ring        = (volatile int32_t *)((char *)game_mod.load_virtbase + 0x9ec3c0);
  st_reqarr_pp   = (void **)((char *)game_mod.load_virtbase + 0x9ec3b0);
  st_ringbuf_pp  = (int32_t **)((char *)game_mod.load_virtbase + 0x9ec3b8);
  st_sem_pp      = (void **)((char *)game_mod.load_virtbase + 0x9ec3d0);
  if (!plat_sem_down || !plat_sem_up) {
    debugPrintf("STREAM: missing Platform::Semaphore symbol, not patched\n");
    return;
  }
  hook_arm64(so_find_addr(&game_mod, "_ZN12StreamThread10ThreadMainEv"), (uintptr_t)StreamThread_fixed);
  debugPrintf("STREAM: asset streamer reimplemented (empty-ring + NULL-device fix)\n");
}

// ---------------------------------------------------------------------------
// cWorldStream::PollStreaming(bool) @ 0x39c7d8 -- the blocking "wait for the
// scene to finish streaming" loop run from CStreaming::LoadScene (gameplay entry
// + cutscenes). On this port a request occasionally never reports completion, so
// the event flag this loop polls never gets its bits and the game hangs on a
// black screen (the dominant gameplay-entry freeze). Reimplemented from the
// disasm, plus a ~4s watchdog: on timeout, log the pending state and run the
// engine's own "done" path so the game proceeds; the rest streams in async.
// 'this' offsets: +472/+944 pending counts, +520 event-flag id, +495 blocking
// flag, +484 flag, +200/+208 a swapped ptr pair, all per the disasm.
// ---------------------------------------------------------------------------
static int  (*sce_PollEventFlag)(unsigned int id, unsigned int pattern, unsigned int mode, unsigned int *outbits);
static void (*lgl_sleep)(int ms);
static void (*ws_swap_in)(void *self);

void *g_ws_instance = NULL;   // the cWorldStream singleton, captured at first poll

// log the stuck in-flight cUmdRequest (+536) then run the engine's "done" reset
// so streaming proceeds. dev@32 NULL = its file open failed; result@48 nonzero =
// finished but the callback never ran; cb@72 0 = no callback set.
static void worldstream_unstick(unsigned char *ws, unsigned int flagid, const char *why) {
  static int nt = 0;
  if (nt++ < 24) {
    unsigned char *rq = (unsigned char *)(uintptr_t)(*(uint64_t *)(ws + 536));
    uint64_t dev = rq ? *(uint64_t *)(rq + 32) : 0, cb = rq ? *(uint64_t *)(rq + 72) : 0;
    int rsz = rq ? *(int *)(rq + 44) : 0, rres = rq ? *(int *)(rq + 48) : 0;
    debugPrintf("PollStreaming: %s | +472=%u +495=%u flag=%u req=%llx dev=%llx size=%d result=%d cb=%llx -- forcing proceed\n",
                why, *(uint32_t *)(ws + 472), ws[495], flagid, (unsigned long long)(uintptr_t)rq,
                (unsigned long long)dev, rsz, rres, (unsigned long long)cb);
  }
  sce_ClearEventFlag(flagid, 0);
  *(uint64_t *)(ws + 208) = *(uint64_t *)(ws + 200);
  *(uint32_t *)(ws + 944) = 0;
  *(uint32_t *)(ws + 472) = 0;
  *(uint32_t *)(ws + 484) = 0;
}

static void PollStreaming_fixed(void *self, int blocking) {
  unsigned char *ws = self;
  const unsigned int flagid = *(uint32_t *)(ws + 520);
  volatile uint32_t *p472 = (volatile uint32_t *)(ws + 472);
  volatile uint32_t *p944 = (volatile uint32_t *)(ws + 944);
  unsigned int out = 0;
  int ret;

  if (!g_ws_instance)
    g_ws_instance = self;   // capture the singleton for worldstream_probe()

  if (blocking & 1) {
    int spins = 0;
    for (;;) {
      if (*p472 == 0 && *p944 == 0)
        sce_SetEventFlag(flagid, 2);                 // mark "idle"
      ret = sce_PollEventFlag(flagid, 3, 8, &out);
      if (ret != -1)
        break;                                       // a completion arrived
      lgl_sleep(1);
      if (++spins >= 4000) {                          // ~4s blocking hang
        worldstream_unstick(ws, flagid, "TIMEOUT(block)");
        return;
      }
    }
  } else {
    ret = sce_PollEventFlag(flagid, 3, 8, &out);
    if (ret == -1) {
      // Non-blocking per-frame pump. The continuous streamer loads one chunk at
      // a time and won't request the next while one is pending, so a stuck
      // request (no blocking wait to time out) halts ALL world streaming. If one
      // stays pending ~5s, unstick so cWorldStream::Update advances.
      static int stuckf = 0;
      if (*p472 == 0 && *p944 == 0) { stuckf = 0; return; }
      if (++stuckf < 300) return;                     // ~5s at 60fps
      stuckf = 0;
      worldstream_unstick(ws, flagid, "STUCK(cont)");
      return;
    }
  }

  // shared "process" path (PollStreaming+0x9c)
  sce_ClearEventFlag(flagid, 0);
  if (ws[495] != 0 || ret != 0 || (out & 2)) {
    *(uint64_t *)(ws + 208) = *(uint64_t *)(ws + 200);
    *p944 = 0; *p472 = 0;
    *(uint32_t *)(ws + 484) = 0;
  } else {
    ws_swap_in(self);
  }
}

// UMD streaming watchdog, called every frame from the main loop. cWorldStream
// streams world geometry one request at a time and won't advance until the
// in-flight one (+536) completes; a single lost UmdThread wakeup (producer sets
// the "work available" bit just as the consumer clears it) deadlocks the whole
// world streamer (only the spawn area loads). If the in-flight request lingers
// more than a few frames, re-poke the UMD event-flag bit0 so UmdThread re-scans
// its queue. Self-limiting: the poke stops the instant +536 clears.
void worldstream_probe(void) {
  if (!g_ws_instance || !sce_SetEventFlag || !umd_eventflag_id)
    return;
  uint64_t req = *(uint64_t *)((char *)g_ws_instance + 536);
  static uint64_t last = 0;
  static int same = 0;
  if (req && req == last) {
    if (++same >= 8)
      sce_SetEventFlag(*umd_eventflag_id, 1);   // bit0 = request-available
  } else {
    same = 0;
  }
  last = req;
}

static void patch_world_stream(void) {
  uintptr_t ps = so_try_find_addr_rx(&game_mod, "_ZN12cWorldStream13PollStreamingEb");
  if (!ps)
    return;
  sce_PollEventFlag = (void *)so_try_find_addr_rx(&game_mod, "_Z22sceKernelPollEventFlagjjjPj");
  lgl_sleep         = (void *)so_try_find_addr_rx(&game_mod, "_Z8lglSleepi");
  ws_swap_in        = (void *)so_try_find_addr_rx(&game_mod, "_ZN12cWorldStream21SwapInStreamingBufferEv");
  // sce_SetEventFlag / sce_ClearEventFlag are resolved by patch_umd_stream()
  if (!sce_PollEventFlag || !lgl_sleep || !ws_swap_in || !sce_SetEventFlag || !sce_ClearEventFlag) {
    debugPrintf("WORLDSTREAM: missing helper symbol, not patched\n");
    return;
  }
  hook_arm64(so_find_addr(&game_mod, "_ZN12cWorldStream13PollStreamingEb"), (uintptr_t)PollStreaming_fixed);
  debugPrintf("WORLDSTREAM: PollStreaming reimplemented (4s timeout unstick)\n");
}

// Social Club is faked, but SocialServices::IsNetworkReachable reads a flag the
// engine leaves "reachable", so the pause-menu Load takes the CLOUD-save path
// and loops forever on a cloud op that never completes offline. The JNI
// andHttp.IsNetworkReachable already reports no network; make the native one
// agree so the load falls back to the local GenericLoad (the path boot Resume
// uses, which works).
static int social_return_false(void) { return 0; }

void patch_game(void) {
  // route JNIEnv access and thread spawning through the fake environment
  // (identical mangling to CTW / Max Payne -- the NVThread layer is shared)
  hook_arm64(so_find_addr(&game_mod, "_Z24NVThreadGetCurrentJNIEnvv"), (uintptr_t)NVThreadGetCurrentJNIEnv);
  hook_arm64(so_find_addr(&game_mod, "_Z22NVThreadSpawnJNIThreadPlPK14pthread_attr_tPKcPFPvS5_ES5_"), (uintptr_t)NVThreadSpawnJNIThread);

  // LCS does NOT export OS_ThreadLaunch/Wait/Close/IsRunning or OS_ScreenGet*
  // (CTW hooked those). Every engine thread comes through the NVThread wrapper
  // above, and the render size is taken from ANativeWindow_getWidth/getHeight
  // (our libc_shim reports screen_width/height there), so nothing else to hook.

  // fix the streaming-thread empty-queue crash (see UmdThread_fixed)
  patch_umd_stream();
  // fix StreamThread::ThreadMain (empty ring + NULL-device requests)
  patch_stream_thread();
  // break the LoadScene "wait for streaming" hang (4s timeout -> proceed)
  patch_world_stream();

  // force the native "network reachable" flag false so the in-game pause-menu
  // Load uses the local GenericLoad instead of looping in the dead cloud path
  if (so_try_find_addr_rx(&game_mod, "_ZN14SocialServices18IsNetworkReachableEv")) {
    hook_arm64(so_find_addr(&game_mod, "_ZN14SocialServices18IsNetworkReachableEv"), (uintptr_t)social_return_false);
    debugPrintf("SOCIAL: IsNetworkReachable forced false (offline -> local save/load)\n");
  }

  // CFerry::UpdateFerrys reads [pool+40], but on a new game the sim ticks before
  // CFerry::Init allocates the pool, so the NULL pool ptr faults. Guard the
  // entry: at +0xc insert `cbz x0, ret`, at +0x10 make the branch unconditional
  // so a valid pool still runs. Self-heals once Init sets the pool a frame later.
  {
    const uintptr_t fy = so_try_find_addr_rx(&game_mod, "_ZN6CFerry12UpdateFerrysEv");
    if (fy) {
      uint32_t *p = (uint32_t *)(fy - (uintptr_t)game_mod.load_virtbase
                                 + (uintptr_t)game_mod.load_base);
      if (p[3] == 0x3940a009u && p[4] == 0x34000049u) {
        p[3] = 0xb4000040u;   // cbz x0, +8  (pool NULL -> ret)
        p[4] = 0x14000002u;   // b   +8      (pool ok   -> do the update)
      } else {
        debugPrintf("FERRY: unexpected insns %08x %08x\n", p[3], p[4]);
      }
    }
  }

  // The AArch64 stack guard reads its cookie at an offset off TPIDR_EL0, which
  // reads back 0 on the Switch (libnx keeps the real TLS in TPIDRRO_EL0) and
  // would fault. Point TPIDR_EL0 at a static buffer here (and in every spawned
  // thread's trampoline).
  init_fake_tls(main_fake_tls);
}
