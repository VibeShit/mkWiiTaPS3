// libctru system services on the PS3: time, threads, synchronization, input,
// APT (the XMB's quit request), display stubs and memory
// (see platform/ps3/include/3ds.h).
#include <3ds.h>
#include <io/move.h>
#include <io/pad.h>
#include <spurs/spurs.h>
#include <sys/spu.h>
#include <sysmodule/sysmodule.h>
#include <lv2/mutex.h>
#include <ppu-asm.h>
#include <sys/process.h>
#include <sys/sem.h>
#include <sys/systime.h>
#include <sys/thread.h>
#include <sysutil/sysutil.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>

// The main thread: default priority, 1 MiB of stack (newlib's printf family
// and Kinoko's deeper call chains want more than the 3DS's 64 KiB).
SYS_PROCESS_PARAM(1001, 0x100000);

// ---- Time ------------------------------------------------------------------
// The PPU time base, in 3DS ARM11 ticks (SYSCLOCK_ARM11 per second).
extern "C" u64 svcGetSystemTick(void) {
	static const u64 frequency = sysGetTimebaseFrequency();
	const u64 tb = __gettime();
	return tb / frequency * SYSCLOCK_ARM11 + tb % frequency * SYSCLOCK_ARM11 / frequency;
}
extern "C" void svcSleepThread(s64 ns) {
	if (ns <= 0) { sysThreadYield(); return; }
	sysUsleep(u32(ns / 1000 > 0 ? ns / 1000 : 1));
}
extern "C" Result svcGetThreadPriority(s32* out, Handle) { *out = 0x30; return 0; }
extern "C" u64 osGetTime(void) {
	u64 sec = 0, nsec = 0;
	sysGetCurrentTime(&sec, &nsec);
	return sec * 1000ULL + nsec / 1000000ULL;
}
extern "C" void osSetSpeedupEnable(bool) {}
extern "C" float osGet3DSliderState(void) { return 0.f; }

// ---- Synchronization -------------------------------------------------------
// Objects are created on first use: state 0 = none, 1 = creating, 2 = ready.
template <class Create>
static void* lazyImpl(volatile int& state, void*& impl, Create create) {
	auto* s = reinterpret_cast<std::atomic<int>*>(const_cast<int*>(&state));
	if (s->load(std::memory_order_acquire) == 2) return impl;
	int expected = 0;
	if (s->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
		impl = create();
		s->store(2, std::memory_order_release);
		return impl;
	}
	while (s->load(std::memory_order_acquire) != 2) sysThreadYield();
	return impl;
}
// Lightweight mutexes: an uncontended lock or unlock stays in user space.
static sys_lwmutex_t* newLwMutex(bool recursive) {
	auto* m = static_cast<sys_lwmutex_t*>(memalign(8, sizeof(sys_lwmutex_t)));
	std::memset(m, 0, sizeof *m);
	sys_lwmutex_attr_t attr{};
	attr.attr_protocol = SYS_LWMUTEX_PROTOCOL_PRIO;
	attr.attr_recursive = recursive ? SYS_LWMUTEX_ATTR_RECURSIVE : SYS_LWMUTEX_ATTR_NOT_RECURSIVE;
	std::memcpy(attr.name, "mkwlock", 8);
	sysLwMutexCreate(m, &attr);
	return m;
}
static sys_lwmutex_t* lockOf(LightLock* lock) {
	return static_cast<sys_lwmutex_t*>(lazyImpl(lock->state, lock->impl, [] { return static_cast<void*>(newLwMutex(false)); }));
}
extern "C" void LightLock_Init(LightLock* lock) { lock->state = 0; lock->impl = nullptr; lockOf(lock); }
extern "C" void LightLock_Lock(LightLock* lock) { sysLwMutexLock(lockOf(lock), 0); }
extern "C" int LightLock_TryLock(LightLock* lock) { return sysLwMutexTryLock(lockOf(lock)) != 0 ? 1 : 0; }
extern "C" void LightLock_Unlock(LightLock* lock) { sysLwMutexUnlock(lockOf(lock)); }

static sys_lwmutex_t* recursiveOf(RecursiveLock* lock) {
	return static_cast<sys_lwmutex_t*>(lazyImpl(lock->state, lock->impl, [] { return static_cast<void*>(newLwMutex(true)); }));
}
extern "C" void RecursiveLock_Init(RecursiveLock* lock) { lock->state = 0; lock->impl = nullptr; recursiveOf(lock); }
extern "C" void RecursiveLock_Lock(RecursiveLock* lock) { sysLwMutexLock(recursiveOf(lock), 0); }
extern "C" void RecursiveLock_Unlock(RecursiveLock* lock) { sysLwMutexUnlock(recursiveOf(lock)); }

// An event is a binary semaphore: Signal makes one count available (at most
// one), a one-shot wait consumes it. A sticky event stays signaled until Clear.
static sys_sem_t eventSem(LightEvent* event) {
	auto* s = reinterpret_cast<std::atomic<int>*>(const_cast<int*>(&event->state));
	if (s->load(std::memory_order_acquire) == 2) return event->id;
	int expected = 0;
	if (s->compare_exchange_strong(expected, 1, std::memory_order_acq_rel)) {
		sys_sem_attr_t attr{};
		attr.attr_protocol = SYS_SEM_ATTR_PROTOCOL;
		attr.attr_pshared = SYS_SEM_ATTR_PSHARED;
		std::memcpy(attr.name, "mkwevnt", 8);
		sys_sem_t sem = 0;
		sysSemCreate(&sem, &attr, 0, 1);
		event->id = sem;
		s->store(2, std::memory_order_release);
		return sem;
	}
	while (s->load(std::memory_order_acquire) != 2) sysThreadYield();
	return event->id;
}
extern "C" void LightEvent_Init(LightEvent* event, ResetType reset) {
	event->state = 0; event->id = 0; event->reset = reset; eventSem(event);
}
extern "C" void LightEvent_Clear(LightEvent* event) { sysSemTryWait(eventSem(event)); }
// Posting past the maximum of one fails harmlessly: the event stays signaled.
extern "C" void LightEvent_Signal(LightEvent* event) { sysSemPost(eventSem(event), 1); }
extern "C" int LightEvent_TryWait(LightEvent* event) {
	const sys_sem_t sem = eventSem(event);
	if (sysSemTryWait(sem) != 0) return 0;
	if (event->reset == RESET_STICKY) sysSemPost(sem, 1);
	return 1;
}
extern "C" void LightEvent_Wait(LightEvent* event) {
	const sys_sem_t sem = eventSem(event);
	sysSemWait(sem, 0); // 0: no timeout
	if (event->reset == RESET_STICKY) sysSemPost(sem, 1);
}
extern "C" int LightEvent_WaitTimeout(LightEvent* event, s64 timeout_ns) {
	const sys_sem_t sem = eventSem(event);
	// lv2 reads a zero timeout as "forever": poll instead.
	const s32 rc = timeout_ns < 1000 ? sysSemTryWait(sem) : sysSemWait(sem, u64(timeout_ns / 1000));
	if (rc != 0) return 1;
	if (event->reset == RESET_STICKY) sysSemPost(sem, 1);
	return 0;
}

// ---- Threads ---------------------------------------------------------------
struct Thread_tag {
	sys_ppu_thread_t id = 0;
	ThreadFunc entry = nullptr;
	void* arg = nullptr;
	bool detached = false;
};
static void threadTrampoline(void* argp) {
	Thread_tag* t = static_cast<Thread_tag*>(argp);
	t->entry(t->arg);
	if (t->detached) delete t;
	sysThreadExit(0);
}
// 3DS 0x18 (highest) .. 0x3F (lowest) around the main thread's 0x30 map onto
// lv2 priorities around the main thread's 1001 (0 highest .. 3071 lowest).
static s32 ps3Priority(int prio) {
	const int p = 1001 + (prio - 0x30) * 10;
	return p < 100 ? 100 : p > 3071 ? 3071 : p;
}
extern "C" Thread threadCreate(ThreadFunc entrypoint, void* arg, size_t stack_size, int prio, int, bool detached) {
	auto* t = new Thread_tag;
	t->entry = entrypoint; t->arg = arg; t->detached = detached;
	// PPU threads need more stack than the 3DS: newlib's printf family alone uses several KiB.
	const u64 stack = stack_size < 0x10000 ? 0x10000 : (stack_size + 0xFFF) & ~size_t(0xFFF);
	char name[] = "mkw_thread";
	if (sysThreadCreate(&t->id, threadTrampoline, t, ps3Priority(prio), stack, detached ? 0 : THREAD_JOINABLE, name) != 0) {
		delete t;
		return nullptr;
	}
	return detached ? reinterpret_cast<Thread>(1) : t;
}
// lv2 joins have no timeout; the game joins only threads it has told to stop.
extern "C" Result threadJoin(Thread thread, u64) {
	if (!thread || reinterpret_cast<uintptr_t>(thread) == 1 || !thread->id) return 0;
	u64 status = 0;
	const s32 rc = sysThreadJoin(thread->id, &status);
	thread->id = 0;
	return rc == 0 ? 0 : -1;
}
extern "C" void threadFree(Thread thread) {
	if (!thread || reinterpret_cast<uintptr_t>(thread) == 1) return;
	if (thread->id) sysThreadDetach(thread->id);
	delete thread;
}
extern "C" void threadExit(int rc) { sysThreadExit(u64(rc)); }

// ---- APT, CFG ----------------------------------------------------------------
// "Quit Game" in the XMB: aptMainLoop returns false from then on, and the
// game leaves its loops and returns from main (the system allows a few seconds).
static std::atomic<bool> quitRequested{false};
static void onSysutil(u64 status, u64, void*) {
	if (status == SYSUTIL_EXIT_GAME) quitRequested.store(true);
}
static aptHookCookie* hooks = nullptr;
extern "C" bool aptMainLoop(void) {
	static bool registered = false;
	if (!registered) { sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, onSysutil, nullptr); registered = true; }
	sysUtilCheckCallback();
	if (!quitRequested.load()) return true;
	static bool notified = false;
	if (!notified) {
		notified = true;
		for (aptHookCookie* c = hooks; c; c = c->next) c->callback(APTHOOK_ONEXIT, c->param);
	}
	return false;
}
extern "C" void aptHook(aptHookCookie* cookie, aptHookFn callback, void* param) {
	cookie->callback = callback; cookie->param = param; cookie->next = hooks; hooks = cookie;
}
extern "C" void aptUnhook(aptHookCookie* cookie) {
	for (aptHookCookie** p = &hooks; *p; p = &(*p)->next)
		if (*p == cookie) { *p = cookie->next; return; }
}
// The PS3 outperforms a New 3DS, so the game takes its New 3DS paths.
extern "C" Result APT_CheckNew3DS(bool* out) { *out = true; return 0; }
extern "C" Result APT_GetAppCpuTimeLimit(u32* percent) { *percent = 100; return 0; }
extern "C" Result APT_SetAppCpuTimeLimit(u32) { return 0; }
// No CFG service: stereo detection fails, so the game runs in 2D.
extern "C" Result cfguInit(void) { return -1; }
extern "C" void cfguExit(void) {}
extern "C" Result CFGU_GetSystemModel(u8* model) { *model = CFG_MODEL_N2DSXL; return -1; }
extern "C" Result CFGU_GetModelNintendo2DS(u8* value) { *value = 0; return -1; }
extern "C" Result CFGU_GetConfigInfoBlk2(u32, u32, void*) { return -1; }

// ---- HID ---------------------------------------------------------------------
// Local players' controllers: DualShock 3s and PlayStation Moves ("devices").
// By default the players take the connected DualShock 3s in port order, then
// the Moves; hidAssignPlayers (the multiplayer controller choice) sets them.
// The 3DS calls (hidKeysHeld...) read player 1, the hidPad* calls any player.
//
// A DualShock 3, laid out as on the Vita: Cross accelerates and confirms (3DS
// A), Circle brakes and backs out (B), Triangle is X and Square is Y; L1/R1
// are L/R (L2/R2 as well); the left stick is the Circle Pad.
//
// A Move, held sideways like the Wii Wheel (buttons facing the player): its
// tilt steers (the Circle Pad's x; tilting it forwards and back is the y, for
// menus and aiming items), the Move button accelerates and confirms (A), T
// drifts and brakes (R), Cross uses the item (L), Square throws it backwards,
// Circle looks behind and backs out (B), Triangle or a shake tricks (X),
// Select sets the current tilt as straight ahead.
namespace {
constexpr int DualShockPorts = 7;
constexpr int Moves = 4;
struct PadState {
	u32 held = 0, down = 0, up = 0, repeat = 0;
	u32 repeatCount = 0, repeatHeld = 0;
	u32 lastKeys = 0;
	int lastRx = 0, lastRy = 0;
	circlePosition circle{};
	bool connected = false;
};
// Devices: DualShock ports 0..6, then Moves (HID_DEVICE_MOVE + 0..3).
PadState dualShocks[DualShockPorts];
PadState moves[Moves];
PadState* device(int id) {
	if (id >= 0 && id < DualShockPorts) return &dualShocks[id];
	if (id >= HID_DEVICE_MOVE && id < HID_DEVICE_MOVE + Moves) return &moves[id - HID_DEVICE_MOVE];
	return nullptr;
}
// Players: playerDevice[player], playerCount of them; chosen ones or automatic.
int playerDevice[HID_MAX_PLAYERS] = {};
unsigned playerCount = 0;
int assigned[HID_MAX_PLAYERS] = {};
unsigned assignedCount = 0;
u32 repeatDelay = 30, repeatInterval = 6;
touchPosition touch{};
bool inputReady = false;
s16 stickAxis(int raw) {
	int v = raw - 128;
	if (v > -24 && v < 24) return 0; // DualShock 3 sticks rest a little off centre.
	v = v * 156 / 127;
	return s16(v < -156 ? -156 : v > 156 ? 156 : v);
}
// Keys and stick of one scan: the Circle Pad's directions, edges and repeat.
void finishScan(PadState& s, u32 keys, int rx, int ry) {
	// The 3DS HID module reports the Circle Pad as directions past about a quarter throw.
	if (s.circle.dx >= 40) keys |= KEY_CPAD_RIGHT;
	if (s.circle.dx <= -40) keys |= KEY_CPAD_LEFT;
	if (s.circle.dy >= 40) keys |= KEY_CPAD_UP;
	if (s.circle.dy <= -40) keys |= KEY_CPAD_DOWN;
	if (rx >= 40) keys |= KEY_CSTICK_RIGHT;
	if (rx <= -40) keys |= KEY_CSTICK_LEFT;
	if (ry >= 40) keys |= KEY_CSTICK_UP;
	if (ry <= -40) keys |= KEY_CSTICK_DOWN;
	s.down = keys & ~s.held;
	s.up = s.held & ~keys;
	s.held = keys;
	// libctru repeat: the held set fires again after `delay` scans, then every `interval`.
	s.repeat = s.down;
	if (s.held && s.held == s.repeatHeld) {
		if (++s.repeatCount >= repeatDelay) { s.repeat = s.held; s.repeatCount = repeatDelay - repeatInterval; }
	} else { s.repeatCount = 0; s.repeatHeld = s.held; }
}
void scanDualShock(PadState& s, int port, bool connected) {
	u32 keys = s.lastKeys;
	int rx = s.lastRx, ry = s.lastRy;
	padData pad{};
	// len 0: nothing changed since the last read; keep the previous state.
	if (connected && ioPadGetData(port, &pad) == 0 && pad.len > 0) {
		const u16 d1 = pad.button[2], d2 = pad.button[3];
		keys = 0;
		if (d2 & PAD_CTRL_CROSS) keys |= KEY_A;
		if (d2 & PAD_CTRL_CIRCLE) keys |= KEY_B;
		if (d2 & PAD_CTRL_TRIANGLE) keys |= KEY_X;
		if (d2 & PAD_CTRL_SQUARE) keys |= KEY_Y;
		if (d2 & (PAD_CTRL_L1 | PAD_CTRL_L2)) keys |= KEY_L;
		if (d2 & (PAD_CTRL_R1 | PAD_CTRL_R2)) keys |= KEY_R;
		if (d1 & PAD_CTRL_START) keys |= KEY_START;
		if (d1 & PAD_CTRL_SELECT) keys |= KEY_SELECT;
		if (d1 & PAD_CTRL_UP) keys |= KEY_DUP;
		if (d1 & PAD_CTRL_DOWN) keys |= KEY_DDOWN;
		if (d1 & PAD_CTRL_LEFT) keys |= KEY_DLEFT;
		if (d1 & PAD_CTRL_RIGHT) keys |= KEY_DRIGHT;
		s.circle.dx = stickAxis(pad.button[6]);
		s.circle.dy = s16(-stickAxis(pad.button[7]));
		rx = stickAxis(pad.button[4]);
		ry = -stickAxis(pad.button[5]);
	}
	if (!connected) { keys = 0; s.circle = {}; rx = ry = 0; }
	s.connected = connected;
	s.lastKeys = keys; s.lastRx = rx; s.lastRy = ry;
	finishScan(s, keys, rx, ry);
}

// ---- PlayStation Move (libgem, inertial only: no PlayStation Eye needed) ----
// libgem's pad bits (gemPadData::buttons).
enum : u16 { MoveSelect = 1, MoveT = 2, MoveButton = 4, MoveStart = 8, MoveTriangle = 16, MoveCircle = 32, MoveCross = 64, MoveSquare = 128 };
struct MoveTilt {
	bool neutralSet = false;
	float roll0 = 0, pitch0 = 0; // straight ahead (radians)
	float steer = 0, pitch = 0;  // smoothed, -1..1
	unsigned shake = 0;          // scans the shake trick stays held
	unsigned rumble = 0;         // scans of rumble left (controller chosen)
	bool rumbling = false;
};
MoveTilt tilt[Moves];
bool gemReady = false, gemTried = false;
// quality.txt: moveSteerAngle (degrees of tilt for a full turn, default 30),
// moveSteerInvert (1 turns the other way).
float steerAngle = 30.f * 3.14159265f / 180.f;
bool steerInvert = false;
float lane(const vec_float4& v, int i) { float f[4]; std::memcpy(f, &v, sizeof f); return f[i]; }
void initGem() {
	gemTried = true;
	if (FILE* f = std::fopen("ux0:data/mkw/quality.txt", "rb")) {
		char line[128];
		float value = 0;
		while (std::fgets(line, sizeof line, f)) {
			if (std::sscanf(line, " moveSteerAngle = %f", &value) == 1) steerAngle = std::clamp(value, 5.f, 90.f) * 3.14159265f / 180.f;
			else if (std::sscanf(line, " moveSteerInvert = %f", &value) == 1) steerInvert = value != 0;
		}
		std::fclose(f);
	}
	// libgem runs its jobs on SPURS; the camera module is its dependency.
	sysModuleLoad(SYSMODULE_CAMERA);
	if (sysModuleLoad(SYSMODULE_GEM) != 0) { std::printf("move: libgem module unavailable\n"); return; }
	sysSpuInitialize(6, 0);
	sys_ppu_thread_t self;
	int priority = 1001;
	if (sysThreadGetId(&self) == 0) sysThreadGetPriority(self, &priority);
	static Spurs* spurs = static_cast<Spurs*>(memalign(SPURS_ALIGN, sizeof(Spurs)));
	SpursAttribute attribute;
	if (!spurs || spursAttributeInitialize(&attribute, 2, 250, priority - 1, true) != 0 ||
		spursAttributeSetNamePrefix(&attribute, "mkwgem", 6) != 0 || spursInitializeWithAttribute(spurs, &attribute) != 0) {
		std::printf("move: SPURS unavailable\n");
		return;
	}
	const s32 size = gemGetMemorySize(Moves);
	void* memory = size > 0 ? memalign(128, size_t(size)) : nullptr;
	gemAttribute gem{};
	gem.version = MOVE_VERSION; gem.max = Moves; gem.memory = memory; gem.spurs = spurs;
	const u8 priorities[8] = {1, 1, 0, 0, 0, 0, 0, 0};
	std::memcpy(gem.spu_priorities, priorities, sizeof priorities);
	const s32 result = memory ? gemInit(&gem) : -1;
	gemReady = result == 0;
	std::printf("move: libgem %s (%d), steering %.0f degrees%s\n", gemReady ? "ready" : "failed", int(result),
		double(steerAngle * 180.f / 3.14159265f), steerInvert ? " inverted" : "");
	if (gemReady) for (u32 m = 0; m < Moves; ++m) gemReset(m);
}
float wrapAngle(float a) {
	while (a > 3.14159265f) a -= 6.2831853f;
	while (a < -3.14159265f) a += 6.2831853f;
	return a;
}
void scanMove(PadState& s, int m, bool connected) {
	auto& t = tilt[m];
	gemInertialState inertial{};
	if (connected && gemGetInertialState(u32(m), GEM_INERTIAL_LATEST, 0, &inertial) != 0) connected = false;
	if (!connected) {
		s.connected = false; s.circle = {}; t = {};
		finishScan(s, 0, 0, 0);
		return;
	}
	s.connected = true;
	const u16 buttons = inertial.pad.buttons;
	// The accelerometer (in g) at rest points away from gravity. Held sideways,
	// turning the Move like a wheel rotates it in its face's (x, y) plane;
	// tilting it forwards or back moves it out of that plane (z).
	const float ax = lane(inertial.accelerometer, 0), ay = lane(inertial.accelerometer, 1), az = lane(inertial.accelerometer, 2);
	const float plane = std::sqrt(ax * ax + ay * ay), length = std::sqrt(plane * plane + az * az);
	if (plane > .35f && length > .5f && length < 1.6f) {
		const float roll = std::atan2(ay, ax), pitch = std::atan2(az, plane);
		if (!t.neutralSet || (buttons & MoveSelect)) { t.roll0 = roll; t.pitch0 = pitch; t.neutralSet = true; }
		float turn = wrapAngle(roll - t.roll0) / steerAngle;
		if (steerInvert) turn = -turn;
		if (std::fabs(turn) < .08f) turn = 0; // a small dead zone keeps straight lines straight
		turn = std::clamp(turn, -1.f, 1.f);
		const float lean = std::clamp(wrapAngle(pitch - t.pitch0) / .5f, -1.f, 1.f);
		// Light smoothing against hand tremor; no visible lag at 60 Hz.
		t.steer += (turn - t.steer) * .5f;
		t.pitch += (lean - t.pitch) * .5f;
	}
	// A shake (the Wii Remote's trick) is a burst of rotation.
	const float gx = lane(inertial.gyro, 0), gy = lane(inertial.gyro, 1), gz = lane(inertial.gyro, 2);
	if (gx * gx + gy * gy + gz * gz > 9.f * 9.f) t.shake = 10;
	else if (t.shake) --t.shake;
	u32 keys = 0;
	if (buttons & MoveButton) keys |= KEY_A;
	if (buttons & MoveT) keys |= KEY_R;
	if (buttons & (MoveCross | MoveSquare)) keys |= KEY_L;
	if (buttons & MoveCircle) keys |= KEY_B;
	if ((buttons & MoveTriangle) || t.shake) keys |= KEY_X;
	if (buttons & MoveStart) keys |= KEY_START;
	s.circle.dx = s16(std::lround(t.steer * 156.f));
	// Square aims the item backwards (the stick held down).
	s.circle.dy = (buttons & MoveSquare) ? s16(-156) : s16(std::lround(t.pitch * 156.f));
	finishScan(s, keys, 0, 0);
	// A chosen Move rumbles briefly.
	const bool rumble = t.rumble > 0;
	if (t.rumble) --t.rumble;
	if (rumble != t.rumbling) { gemSetRumble(u32(m), rumble ? 160 : 0); t.rumbling = rumble; }
}
const PadState* player(unsigned index) {
	static const PadState none{};
	if (index >= playerCount) return &none;
	const PadState* s = device(playerDevice[index]);
	return s ? s : &none;
}
} // namespace
extern "C" void hidScanInput(void) {
	if (!inputReady) { ioPadInit(7); inputReady = true; }
	if (!gemTried) initGem();
	padInfo info{};
	const bool ok = ioPadGetInfo(&info) == 0;
	for (int port = 0; port < DualShockPorts; ++port)
		scanDualShock(dualShocks[port], port, ok && port < int(info.max) && info.status[port]);
	gemInfo moveInfo{};
	const bool movesOk = gemReady && gemGetInfo(&moveInfo) == 0;
	for (int m = 0; m < Moves; ++m)
		scanMove(moves[m], m, movesOk && u32(m) < moveInfo.max && moveInfo.status[m] == 1);
	playerCount = 0;
	if (assignedCount) {
		for (unsigned p = 0; p < assignedCount; ++p) playerDevice[playerCount++] = assigned[p];
	} else {
		for (int port = 0; port < DualShockPorts && playerCount < HID_MAX_PLAYERS; ++port)
			if (dualShocks[port].connected) playerDevice[playerCount++] = port;
		for (int m = 0; m < Moves && playerCount < HID_MAX_PLAYERS; ++m)
			if (moves[m].connected) playerDevice[playerCount++] = HID_DEVICE_MOVE + m;
	}
}
extern "C" u32 hidKeysHeld(void) { return player(0)->held; }
extern "C" u32 hidKeysDown(void) { return player(0)->down; }
extern "C" u32 hidKeysUp(void) { return player(0)->up; }
extern "C" u32 hidKeysDownRepeat(void) { return player(0)->repeat; }
extern "C" void hidSetRepeatParameters(u32 delay, u32 interval) { repeatDelay = delay; repeatInterval = interval ? interval : 1; }
extern "C" void hidCircleRead(circlePosition* pos) { *pos = player(0)->circle; }
extern "C" void hidTouchRead(touchPosition* pos) { *pos = touch; }
extern "C" unsigned hidPadCount(void) { return playerCount; }
extern "C" u32 hidPadKeysHeld(unsigned player) { return ::player(player)->held; }
extern "C" u32 hidPadKeysDown(unsigned player) { return ::player(player)->down; }
extern "C" u32 hidPadKeysDownRepeat(unsigned player) { return ::player(player)->repeat; }
extern "C" void hidPadCircleRead(unsigned player, circlePosition* pos) { *pos = ::player(player)->circle; }
extern "C" u32 hidDevicesDown(u32 keys) {
	u32 mask = 0;
	for (int port = 0; port < DualShockPorts; ++port) if (dualShocks[port].down & keys) mask |= 1u << port;
	for (int m = 0; m < Moves; ++m) if (moves[m].down & keys) mask |= 1u << (HID_DEVICE_MOVE + m);
	return mask;
}
extern "C" u32 hidDevicesConnected(void) {
	u32 mask = 0;
	for (int port = 0; port < DualShockPorts; ++port) if (dualShocks[port].connected) mask |= 1u << port;
	for (int m = 0; m < Moves; ++m) if (moves[m].connected) mask |= 1u << (HID_DEVICE_MOVE + m);
	return mask;
}
extern "C" void hidAssignPlayers(const int* devices, unsigned count) {
	assignedCount = 0;
	for (unsigned p = 0; p < count && p < HID_MAX_PLAYERS; ++p) {
		if (!device(devices[p])) continue;
		assigned[assignedCount++] = devices[p];
		if (devices[p] >= HID_DEVICE_MOVE) {
			// Chosen: rumble, and straight ahead is how it is held now.
			auto& t = tilt[devices[p] - HID_DEVICE_MOVE];
			t.rumble = 20; t.neutralSet = false;
		}
	}
	hidScanInput();
}
extern "C" int hidPadDevice(unsigned player) { return player < playerCount ? playerDevice[player] : -1; }

// ---- Display -----------------------------------------------------------------
namespace c3dps3 { void initDisplay(); void waitVblank(); }
extern "C" void gfxInitDefault(void) { c3dps3::initDisplay(); }
extern "C" void gfxExit(void) {}
extern "C" void gfxSet3D(bool) {}
extern "C" u8* gfxGetFramebuffer(gfxScreen_t, gfx3dSide_t, u16* width, u16* height) {
	if (width) *width = 0;
	if (height) *height = 0;
	return nullptr;
}
extern "C" void gfxFlushBuffers(void) {}
extern "C" void gfxSwapBuffers(void) {}
extern "C" void gfxScreenSwapBuffers(gfxScreen_t, bool) {}
extern "C" void gspWaitForVBlank(void) { c3dps3::waitVblank(); }
extern "C" void gspWaitForEvent(GSPGPU_Event, bool) { c3dps3::waitVblank(); }
extern "C" Result GSPGPU_InvalidateDataCache(const void*, u32) { return 0; }
extern "C" Result DSP_FlushDataCache(const void*, u32) { return 0; }

// ---- Console -----------------------------------------------------------------
static PrintConsole console{};
extern "C" PrintConsole* consoleInit(gfxScreen_t, PrintConsole* c) {
	if (!c) c = &console;
	c->consoleInitialised = true;
	return c;
}
extern "C" PrintConsole* consoleGetDefault(void) { return &console; }
extern "C" void consoleClear(void) {}

// ---- Memory --------------------------------------------------------------------
// LINEAR memory is RSX-mapped main memory: see c3d_gpu.cpp.
extern "C" void* vramAlloc(size_t size) { return memalign(0x80, size ? size : 1); }
extern "C" void vramFree(void* mem) { free(mem); }
extern "C" u32 vramSpaceFree(void) { return 6u << 20; }
extern "C" u32 osConvertVirtToPhys(const void* vaddr) { return u32(uintptr_t(vaddr)); }

extern "C" Result romfsInit(void) { return -1; }
extern "C" Result romfsExit(void) { return 0; }
