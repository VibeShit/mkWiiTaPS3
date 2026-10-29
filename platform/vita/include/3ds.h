// PS Vita stand-in for libctru's <3ds.h>: the subset of the 3DS system API
// this game uses, implemented on Vita system calls (platform/vita/src/ctru_*.cpp).
// Types, GPU enums and shader binary structures come from libctru's own
// headers (vendor/libctru) so citro3d's headers and the game see the same
// definitions as on the 3DS.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// libctru's headers rely on <3ds.h> for C linkage, as here.
#include <3ds/types.h>
#include <3ds/result.h>
#include <3ds/gpu/gpu.h>
#include <3ds/gpu/shbin.h>
#include <3ds/gpu/shaderProgram.h>
#include <3ds/gpu/gx.h>
#include <3ds/util/decompress.h>

// ---- Handles, time ---------------------------------------------------------
#define CUR_THREAD_HANDLE 0xFFFF8000
#define CUR_PROCESS_HANDLE 0xFFFF8001
// The 3DS ARM11 tick rate. svcGetSystemTick reports ticks at this rate so the
// game's tick arithmetic (ticks * 1000 / SYSCLOCK_ARM11) stays unchanged.
#define SYSCLOCK_ARM11 268111856ULL
u64 svcGetSystemTick(void);
void svcSleepThread(s64 ns);
Result svcGetThreadPriority(s32* out, Handle handle);
u64 osGetTime(void);
void osSetSpeedupEnable(bool enable);
float osGet3DSliderState(void);

// ---- Synchronization -------------------------------------------------------
// Created lazily on first use, so zero-initialized objects are valid.
typedef struct { volatile int state; int uid; } LightLock;
typedef struct { volatile int state; int uid; int owner; int count; } RecursiveLock;
typedef enum { RESET_ONESHOT = 0, RESET_STICKY = 1, RESET_PULSE = 2 } ResetType;
typedef struct { volatile int state; int uid; int reset; } LightEvent;
void LightLock_Init(LightLock* lock);
void LightLock_Lock(LightLock* lock);
int LightLock_TryLock(LightLock* lock);
void LightLock_Unlock(LightLock* lock);
void RecursiveLock_Init(RecursiveLock* lock);
void RecursiveLock_Lock(RecursiveLock* lock);
void RecursiveLock_Unlock(RecursiveLock* lock);
void LightEvent_Init(LightEvent* event, ResetType reset);
void LightEvent_Clear(LightEvent* event);
void LightEvent_Signal(LightEvent* event);
int LightEvent_TryWait(LightEvent* event);
void LightEvent_Wait(LightEvent* event);
// Non-zero on timeout, zero when signaled.
int LightEvent_WaitTimeout(LightEvent* event, s64 timeout_ns);

// ---- Threads ---------------------------------------------------------------
typedef struct Thread_tag* Thread;
typedef void (*ThreadFunc)(void*);
// prio: 3DS priority (0x18 highest .. 0x3F lowest). core_id: 0/1/2 pin the
// thread to that Vita core; -1/-2 let the scheduler choose.
Thread threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int core_id, bool detached);
Result threadJoin(Thread thread, u64 timeout_ns);
void threadFree(Thread thread);
void threadExit(int rc);

// ---- APT (home menu, model, CPU time) --------------------------------------
typedef enum {
	APTHOOK_ONSUSPEND = 0, APTHOOK_ONRESTORE, APTHOOK_ONSLEEP, APTHOOK_ONWAKEUP, APTHOOK_ONEXIT, APTHOOK_COUNT,
} APT_HookType;
typedef void (*aptHookFn)(APT_HookType hook, void* param);
typedef struct tag_aptHookCookie { struct tag_aptHookCookie* next; aptHookFn callback; void* param; } aptHookCookie;
bool aptMainLoop(void);
void aptHook(aptHookCookie* cookie, aptHookFn callback, void* param);
void aptUnhook(aptHookCookie* cookie);
Result APT_CheckNew3DS(bool* out);
Result APT_GetAppCpuTimeLimit(u32* percent);
Result APT_SetAppCpuTimeLimit(u32 percent);

// ---- CFG -------------------------------------------------------------------
enum { CFG_MODEL_3DS = 0, CFG_MODEL_3DSXL = 1, CFG_MODEL_N3DS = 2, CFG_MODEL_2DS = 3, CFG_MODEL_N3DSXL = 4, CFG_MODEL_N2DSXL = 5 };
Result cfguInit(void);
void cfguExit(void);
Result CFGU_GetSystemModel(u8* model);
Result CFGU_GetModelNintendo2DS(u8* value);
Result CFGU_GetConfigInfoBlk2(u32 size, u32 blkID, void* outData);

// ---- HID (Vita buttons, sticks and front touch) ----------------------------
enum {
	KEY_A = BIT(0), KEY_B = BIT(1), KEY_SELECT = BIT(2), KEY_START = BIT(3),
	KEY_DRIGHT = BIT(4), KEY_DLEFT = BIT(5), KEY_DUP = BIT(6), KEY_DDOWN = BIT(7),
	KEY_R = BIT(8), KEY_L = BIT(9), KEY_X = BIT(10), KEY_Y = BIT(11),
	KEY_ZL = BIT(14), KEY_ZR = BIT(15), KEY_TOUCH = BIT(20),
	KEY_CSTICK_RIGHT = BIT(24), KEY_CSTICK_LEFT = BIT(25), KEY_CSTICK_UP = BIT(26), KEY_CSTICK_DOWN = BIT(27),
	KEY_CPAD_RIGHT = BIT(28), KEY_CPAD_LEFT = BIT(29), KEY_CPAD_UP = BIT(30), KEY_CPAD_DOWN = BIT(31),
	KEY_UP = KEY_DUP | KEY_CPAD_UP, KEY_DOWN = KEY_DDOWN | KEY_CPAD_DOWN,
	KEY_LEFT = KEY_DLEFT | KEY_CPAD_LEFT, KEY_RIGHT = KEY_DRIGHT | KEY_CPAD_RIGHT,
};
typedef struct { u16 px, py; } touchPosition;
typedef struct { s16 dx, dy; } circlePosition;
void hidScanInput(void);
u32 hidKeysHeld(void);
u32 hidKeysDown(void);
u32 hidKeysUp(void);
u32 hidKeysDownRepeat(void);
void hidSetRepeatParameters(u32 delay, u32 interval);
void hidCircleRead(circlePosition* pos);
void hidTouchRead(touchPosition* pos);
// Local multiplayer (not libctru): player n is the n-th connected controller;
// player 0 is the one the calls above read.
#define HID_MAX_PLAYERS 4
unsigned hidPadCount(void);
u32 hidPadKeysHeld(unsigned player);
u32 hidPadKeysDown(unsigned player);
u32 hidPadKeysDownRepeat(unsigned player);
void hidPadCircleRead(unsigned player, circlePosition* pos);
// Splitscreen GPU controls (not citro3d): mirrored views swap the culled
// face; the top screen's target renders at its internal resolution / divisor.
void c3dport_SetCullSwap(bool swap);
void c3dport_SetResolutionDivisor(float divisor);

// ---- Display (the Vita has one screen; vitaGL owns it) ---------------------
typedef enum { GFX_TOP = 0, GFX_BOTTOM = 1 } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT = 1 } gfx3dSide_t;
typedef enum { GSPGPU_EVENT_PSC0 = 0, GSPGPU_EVENT_PSC1, GSPGPU_EVENT_VBlank0, GSPGPU_EVENT_VBlank1, GSPGPU_EVENT_PPF, GSPGPU_EVENT_P3D, GSPGPU_EVENT_DMA } GSPGPU_Event;
void gfxInitDefault(void);
void gfxExit(void);
void gfxSet3D(bool enable);
// Always NULL here: there is no lower LCD to write.
u8* gfxGetFramebuffer(gfxScreen_t screen, gfx3dSide_t side, u16* width, u16* height);
void gfxFlushBuffers(void);
void gfxSwapBuffers(void);
void gfxScreenSwapBuffers(gfxScreen_t scr, bool hasStereo);
void gspWaitForVBlank(void);
void gspWaitForEvent(GSPGPU_Event id, bool nextEvent);
Result GSPGPU_FlushDataCache(const void* adr, u32 size);
Result GSPGPU_InvalidateDataCache(const void* adr, u32 size);
Result DSP_FlushDataCache(const void* address, u32 size);

// ---- Text console (prints to the log; the Vita has no lower screen) --------
typedef struct ConsoleFont { u8* gfx; u16 asciiOffset; u16 numChars; } ConsoleFont;
typedef struct PrintConsole { ConsoleFont font; int cursorX, cursorY; bool consoleInitialised; } PrintConsole;
PrintConsole* consoleInit(gfxScreen_t screen, PrintConsole* console);
PrintConsole* consoleGetDefault(void);
void consoleClear(void);

// ---- Memory ----------------------------------------------------------------
// The Vita GPU reads through vitaGL's own copies, so "LINEAR" memory is
// ordinary heap. linearSpaceFree reports a budget the loaders treat as ample.
void* linearAlloc(size_t size);
void* linearMemAlign(size_t size, size_t alignment);
void* linearRealloc(void* mem, size_t size);
size_t linearGetSize(void* mem);
void linearFree(void* mem);
u32 linearSpaceFree(void);
void* vramAlloc(size_t size);
void vramFree(void* mem);
u32 vramSpaceFree(void);
u32 osConvertVirtToPhys(const void* vaddr);
// There is no separate VRAM: every address counts as already in it, so the
// renderer's LINEAR-to-VRAM texture moves (Renderer::moveToVram) never run.
#define OS_VRAM_VADDR 0u
#define OS_VRAM_SIZE 0xFFFFFFFFu

// ---- RomFS (unused: game files live in ux0:data) ---------------------------
Result romfsInit(void);
Result romfsExit(void);

// ---- NDSP: the 3DS DSP mixer, emulated in software (ctru_ndsp.cpp) ---------
typedef enum { NDSP_OUTPUT_MONO = 0, NDSP_OUTPUT_STEREO = 1, NDSP_OUTPUT_SURROUND = 2 } ndspOutputMode;
typedef enum { NDSP_CLIP_NORMAL = 0, NDSP_CLIP_SOFT = 1 } ndspClippingMode;
typedef struct { u16 index; s16 history0; s16 history1; } ndspAdpcmData;
typedef struct tag_ndspWaveBuf ndspWaveBuf;
enum { NDSP_WBUF_FREE = 0, NDSP_WBUF_QUEUED = 1, NDSP_WBUF_PLAYING = 2, NDSP_WBUF_DONE = 3 };
struct tag_ndspWaveBuf {
	union { s8* data_pcm8; s16* data_pcm16; u8* data_adpcm; const void* data_vaddr; };
	u32 nsamples;
	ndspAdpcmData* adpcm_data;
	u32 offset;
	bool looping;
	u8 status;
	u16 sequence_id;
	ndspWaveBuf* next;
};
typedef void (*ndspCallback)(void* data);
enum { NDSP_ENCODING_PCM8 = 0, NDSP_ENCODING_PCM16, NDSP_ENCODING_ADPCM };
#define NDSP_CHANNELS(n) ((u32)(n) & 3)
#define NDSP_ENCODING(n) (((u32)(n) & 3) << 2)
enum {
	NDSP_FORMAT_MONO_PCM8 = NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_PCM8),
	NDSP_FORMAT_MONO_PCM16 = NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_PCM16),
	NDSP_FORMAT_MONO_ADPCM = NDSP_CHANNELS(1) | NDSP_ENCODING(NDSP_ENCODING_ADPCM),
	NDSP_FORMAT_STEREO_PCM8 = NDSP_CHANNELS(2) | NDSP_ENCODING(NDSP_ENCODING_PCM8),
	NDSP_FORMAT_STEREO_PCM16 = NDSP_CHANNELS(2) | NDSP_ENCODING(NDSP_ENCODING_PCM16),
	NDSP_FORMAT_PCM8 = NDSP_FORMAT_MONO_PCM8, NDSP_FORMAT_PCM16 = NDSP_FORMAT_MONO_PCM16, NDSP_FORMAT_ADPCM = NDSP_FORMAT_MONO_ADPCM,
	NDSP_FRONT_BYPASS = BIT(4), NDSP_3D_SURROUND_PREPROCESSED = BIT(6),
};
typedef enum { NDSP_INTERP_POLYPHASE = 0, NDSP_INTERP_LINEAR = 1, NDSP_INTERP_NONE = 2 } ndspInterpType;
Result ndspInit(void);
void ndspExit(void);
u32 ndspGetFrameCount(void);
void ndspSetMasterVol(float volume);
void ndspSetOutputMode(ndspOutputMode mode);
void ndspSetClippingMode(ndspClippingMode mode);
void ndspSetCallback(ndspCallback callback, void* data);
void ndspChnReset(int id);
bool ndspChnIsPlaying(int id);
u32 ndspChnGetSamplePos(int id);
void ndspChnSetPaused(int id, bool paused);
void ndspChnSetFormat(int id, u16 format);
void ndspChnSetInterp(int id, ndspInterpType type);
void ndspChnSetRate(int id, float rate);
void ndspChnSetMix(int id, float mix[12]);
void ndspChnSetAdpcmCoefs(int id, u16 coefs[16]);
void ndspChnWaveBufClear(int id);
void ndspChnWaveBufAdd(int id, ndspWaveBuf* buf);
void ndspChnIirMonoSetEnable(int id, bool enable);
bool ndspChnIirMonoSetParamsLowPassFilter(int id, float f0);

#ifdef __cplusplus
}
#endif
