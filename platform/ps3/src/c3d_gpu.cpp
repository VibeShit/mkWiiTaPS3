// citro3d on the PS3's RSX, through PSL1GHT's librsx: the GPU state machine,
// "LINEAR" memory, textures, render targets, draws and presentation (see
// c3d_internal.hpp). Structured as the Vita port's c3d_gpu.cpp.
//
// Conventions kept from the PICA so the game's renderer runs unchanged:
//  - LINEAR memory (linearAlloc) is main memory mapped for the RSX; draws read
//    vertex and index data in place, as the PICA does.
//  - C3D_FrameBegin waits until the GPU has finished the previous frame, as
//    citro3d does: the game rewrites per-frame vertex data after it.
//  - Render targets keep their portrait 3DS shapes (240x424 top screen) and are
//    rendered at Scale times that size. FrameEnd rotates the top target onto
//    the display, undoing Mtx_PerspTilt/Mtx_OrthoTilt.
//  - Window coordinates and texture rows follow GL's lower-left origin, as the
//    PICA's do: row 0 of a target or texture is its bottom (v = 0).
//  - Depth keeps the PICA's values (c3d_shader.cpp), so GEQUAL/clear-to-0 work.
//  - Skinned draws are skinned here on the CPU (see skinVertices).
#include "c3d_internal.hpp"
#include <3ds.h>
#include <rsx/rsx.h>
#include <sysutil/video.h>
#include <sys/systime.h>
#include <lv2/thread.h>
#include <ppu-asm.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <unordered_map>
#include <vector>
#include <malloc.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace c3dps3;

extern "C" {
C3D_FVec C3D_FVUnif[2][C3D_FVUNIF_COUNT];
C3D_IVec C3D_IVUnif[2][C3D_IVUNIF_COUNT];
u16 C3D_BoolUnifs[2];
bool C3D_FVUnifDirty[2][C3D_FVUNIF_COUNT];
bool C3D_IVUnifDirty[2][C3D_IVUNIF_COUNT];
bool C3D_BoolUnifsDirty[2];
}

namespace c3dps3 {
gcmContextData* context = nullptr;
uint64_t nowUs() {
	static const uint64_t frequency = sysGetTimebaseFrequency();
	const uint64_t tb = __gettime();
	return tb / frequency * 1000000ULL + tb % frequency * 1000000ULL / frequency;
}
}

namespace {

// Vertex data types the RSX reads without normalizing (GCM's UB256, S32K).
constexpr u8 VertexUB256 = 7, VertexS32K = 5;
// Main memory the RSX reads (IO mapped): the command buffer, LINEAR memory
// and the per-frame scratch area.
constexpr uint32_t CommandSize = 8u << 20; // two frames' commands (switchCommandHalf)
constexpr uint32_t LinearSize = 64u << 20;
constexpr uint32_t ScratchSize = 16u << 20;
constexpr uint32_t IoSize = CommandSize + LinearSize + ScratchSize;
constexpr u8 FenceLabel = 255;
constexpr const char* DataDir = "ux0:data/mkw"; // mapped onto the hard disk (ctru_file.cpp)

// Internal resolution: render targets are this many times their canvas size.
// 2 (the default) renders the 16:9 top screen at 848x480; quality.txt
// "resolution=3" gives 1272x720. Read once, before any target exists.
float Scale = 2.f;
inline int scaled(int v, float scale = Scale) { return int(std::lround(float(v) * scale)); }
inline int scaledSpan(int from, int size, float scale = Scale) { return scaled(from + size, scale) - scaled(from, scale); }
// Splitscreen (c3dport_SetResolutionDivisor): the top screen's target is drawn
// at Scale / Divisor, into its lower-left corner, which present() stretches.
float Divisor = 1.f;
// Mirrored splitscreen views (c3dport_SetCullSwap): front and back swap.
bool cullSwap = false;

bool displayReady = false;
uint32_t displayWidth = 1280, displayHeight = 720;
float displayAspect = 16.f / 9.f;
void* displayBuffer[2] = {};
uint32_t displayOffset[2] = {}, displayPitch = 0;
void* displayDepth = nullptr;
uint32_t displayDepthOffset = 0, displayDepthPitch = 0;
unsigned backBuffer = 0;

LightLock rsxHeapLock;

// ---- Fences ----------------------------------------------------------------------
// Each frame ends with a backend label write; anything the GPU may still read
// is released once the label passes the frame it was last used in.
std::atomic<uint32_t> submittedFence{0};
std::atomic<bool> inFrame{false};
volatile uint32_t* fenceLabel = nullptr;
uint32_t completedFence() { return fenceLabel ? *fenceLabel : 0; }
bool passed(uint32_t completed, uint32_t fence) { return int32_t(completed - fence) >= 0; }
// The fence a resource released now must wait for.
uint32_t releaseFence() { return submittedFence.load() + (inFrame.load() ? 1 : 0); }
// The command buffer is two halves, one per frame, so GCM's segment callback
// never runs (PSL1GHT's commands keep context->current in a register across
// it and write to the old segment). A frame ends with its fence and a jump to
// the other half, written before PUT passes them; FrameBegin waits for the
// frame that last used that half.
u32* commandBegin = nullptr;
uint32_t commandHalfWords = 0;
unsigned commandHalf = 0;
u32* checkedTo = nullptr; // DEBUG: checkCommands
const char* lastCheck = "start";
void switchCommandHalf() {
	commandHalf ^= 1;
	u32* begin = commandBegin + commandHalf * commandHalfWords;
	uint32_t offset = 0;
	rsxAddressToOffset(begin, &offset);
	rsxSetJumpCommand(context, offset);
	context->begin = begin;
	context->current = begin;
	context->end = begin + commandHalfWords - 1;
	checkedTo = begin;
	lastCheck = "frame start";
}
void writeFence(bool frameEnd = false) {
	const uint32_t value = submittedFence.load() + 1;
	rsxSetWriteBackendLabel(context, FenceLabel, value);
	if (frameEnd) switchCommandHalf();
	rsxFlushBuffer(context);
	submittedFence.store(value);
}
void waitFence(uint32_t value) {
	while (!passed(completedFence(), value)) usleep(30);
}
// Everything submitted so far has completed (render thread).
void waitIdle() { writeFence(); waitFence(submittedFence.load()); }

// DEBUG: parses the commands written since the last check; a header that does
// not line up is logged with the step that wrote it, before the RSX reads it.
void checkCommands(const char* where) {
	if (!checkedTo) return;
	static sys_ppu_thread_t thread = 0;
	static unsigned switches = 0;
	sys_ppu_thread_t self = 0;
	sysThreadGetId(&self);
	if (self != thread && switches++ < 50) std::printf("c3dps3: commands from thread %llx at '%s' (was %llx)\n", (unsigned long long)self, where, (unsigned long long)thread);
	thread = self;
	u32* p = checkedTo;
	while (p < context->current) {
		const u32 h = *p;
		const u32 count = (h >> 18) & 0x7ff;
		// 2: a call (the system's flip routine), 0x20000: a return.
		if (h && h != 0x20000 && (h & 3) != 2 && ((h & 0xa0030003) || !count)) {
			std::printf("c3dps3: bad command 0x%08x at +0x%x, written between '%s' and '%s' (from +0x%x to +0x%x)\n", unsigned(h),
				unsigned((p - commandBegin) * 4), lastCheck, where, unsigned((checkedTo - commandBegin) * 4), unsigned((context->current - commandBegin) * 4));
			for (u32* q = std::max(commandBegin, checkedTo - 16); q < context->current && q < p + 48; q += 4)
				std::printf("  +0x%05x: %08x %08x %08x %08x\n", unsigned((q - commandBegin) * 4), unsigned(q[0]), unsigned(q[1]), unsigned(q[2]), unsigned(q[3]));
			std::fflush(stdout);
			std::abort();
		}
		p += h && h != 0x20000 && (h & 3) != 2 ? 1 + count : 1;
	}
	checkedTo = p;
	lastCheck = where;
}


// RSX local memory released when the GPU is done with it.
struct DeadBlock { void* ptr; uint32_t fence; };
std::vector<DeadBlock> deadVram;
void freeVramLater(void* p) {
	if (!p) return;
	LightLock_Lock(&rsxHeapLock);
	deadVram.push_back({p, releaseFence()});
	LightLock_Unlock(&rsxHeapLock);
}
void collectVram() {
	const uint32_t done = completedFence();
	LightLock_Lock(&rsxHeapLock);
	for (size_t i = 0; i < deadVram.size();)
		if (passed(done, deadVram[i].fence)) { rsxFree(deadVram[i].ptr); deadVram[i] = deadVram.back(); deadVram.pop_back(); }
		else ++i;
	LightLock_Unlock(&rsxHeapLock);
}

// ---- LINEAR memory ----------------------------------------------------------------
// A first-fit heap over the IO-mapped region. Freed blocks are reused once the
// GPU has passed the frame that may still read them.
class LinearHeap {
public:
	void init(uint8_t* base, uint32_t size) {
		LightLock_Init(&lock_);
		base_ = uintptr_t(base); size_ = size;
		free_[base_] = size;
		freeBytes_ = size;
	}
	void* alloc(size_t size, size_t alignment) {
		if (!size) size = 1;
		size = (size + 127) & ~size_t(127);
		if (alignment < 128) alignment = 128;
		LightLock_Lock(&lock_);
		collectLocked();
		void* p = allocLocked(size, alignment);
		LightLock_Unlock(&lock_);
		return p;
	}
	void release(void* p) {
		if (!p) return;
		LightLock_Lock(&lock_);
		pending_.push_back({uintptr_t(p), releaseFence()});
		collectLocked();
		LightLock_Unlock(&lock_);
	}
	size_t sizeOf(void* p) {
		LightLock_Lock(&lock_);
		auto it = used_.find(uintptr_t(p));
		const size_t size = it == used_.end() ? 0 : it->second;
		LightLock_Unlock(&lock_);
		return size;
	}
	void* realloc(void* p, size_t size) {
		if (!p) return alloc(size, 128);
		const size_t old = sizeOf(p);
		void* q = alloc(size, 128);
		if (!q) return nullptr;
		std::memcpy(q, p, std::min(old, size));
		release(p);
		return q;
	}
	size_t freeBytes() {
		LightLock_Lock(&lock_);
		collectLocked();
		const size_t bytes = freeBytes_;
		LightLock_Unlock(&lock_);
		return bytes;
	}
	bool contains(const void* p) const { return uintptr_t(p) >= base_ && uintptr_t(p) < base_ + size_; }
private:
	struct Pending { uintptr_t at; uint32_t fence; };
	void* allocLocked(size_t size, size_t alignment) {
		for (auto it = free_.begin(); it != free_.end(); ++it) {
			const uintptr_t start = it->first, end = it->first + it->second;
			const uintptr_t at = (start + alignment - 1) & ~uintptr_t(alignment - 1);
			if (at + size > end) continue;
			free_.erase(it);
			if (at > start) free_[start] = at - start;
			if (at + size < end) free_[at + size] = end - (at + size);
			used_[at] = size;
			freeBytes_ -= size;
			return reinterpret_cast<void*>(at);
		}
		return nullptr;
	}
	void freeLocked(uintptr_t at) {
		auto used = used_.find(at);
		if (used == used_.end()) return;
		uintptr_t start = at, size = used->second;
		used_.erase(used);
		freeBytes_ += size;
		auto next = free_.lower_bound(start);
		if (next != free_.end() && start + size == next->first) { size += next->second; next = free_.erase(next); }
		if (next != free_.begin()) {
			auto prev = std::prev(next);
			if (prev->first + prev->second == start) { start = prev->first; size += prev->second; free_.erase(prev); }
		}
		free_[start] = size;
	}
	void collectLocked() {
		if (pending_.empty()) return;
		const uint32_t done = completedFence();
		for (size_t i = 0; i < pending_.size();)
			if (passed(done, pending_[i].fence)) { freeLocked(pending_[i].at); pending_[i] = pending_.back(); pending_.pop_back(); }
			else ++i;
	}
	LightLock lock_;
	uintptr_t base_ = 0;
	size_t size_ = 0, freeBytes_ = 0;
	std::map<uintptr_t, size_t> free_;
	std::unordered_map<uintptr_t, size_t> used_;
	std::vector<Pending> pending_;
} linear;

// ---- Scratch ----------------------------------------------------------------------
// Per-frame RSX-readable memory (render thread): skinned vertices, widened
// indices, constant rows, the present quad. Reused once the frame is done.
uint8_t* scratchBase = nullptr;
uint32_t scratchUsed = 0;
uint8_t* scratchAlloc(uint32_t size, uint32_t alignment = 128) {
	uint32_t at = (scratchUsed + alignment - 1) & ~(alignment - 1);
	if (at + size > ScratchSize) {
		// Full: let the GPU finish what reads it, then start over.
		waitIdle();
		at = 0;
	}
	if (size > ScratchSize) return nullptr;
	scratchUsed = at + size;
	return scratchBase + at;
}
uint32_t ioOffset(const void* p) {
	uint32_t offset = 0;
	gcmAddressToOffset(p, &offset);
	return offset;
}

// ---- Textures -------------------------------------------------------------------
// One mip level converted for the RSX: rows bottom to top (GL order), in the
// texture's RSX format (big-endian A8R8G8B8 or R5G6B5).
struct Converted {
	unsigned w = 0, h = 0, rowBytes = 0;
	std::vector<uint8_t> data;
};
struct GpuTexture {
	// Levels converted where the data was flushed (often the asset loader's
	// thread), so the drawing thread only copies them to RSX memory.
	std::vector<Converted> prepared;
	bool dirty = true;
	bool renderTarget = false;
	size_t bytes = 0; // the CPU (PICA) data
	// RSX storage.
	void* vram = nullptr;
	uint32_t offset = 0, pitch = 0;
	uint16_t width = 0, height = 0;
	uint8_t format = 0, levels = 1;
	bool hasAlpha = true;
};
LightLock textureLock;
std::map<uintptr_t, GpuTexture*> textures; // by tex->data

GpuTexture* findTexture(const void* data) {
	auto it = textures.find(uintptr_t(data));
	return it == textures.end() ? nullptr : it->second;
}
// The texture whose CPU data contains addr (C3D_SyncTextureCopy targets).
GpuTexture* textureContaining(const void* addr) {
	auto it = textures.upper_bound(uintptr_t(addr));
	if (it == textures.begin()) return nullptr;
	--it;
	return uintptr_t(addr) < it->first + it->second->bytes ? it->second : nullptr;
}

uint8_t mortonTable[64];
void initMorton() {
	for (unsigned y = 0; y < 8; ++y)
		for (unsigned x = 0; x < 8; ++x) {
			unsigned m = 0;
			for (unsigned b = 0; b < 3; ++b) m |= ((x >> b) & 1u) << (2 * b) | ((y >> b) & 1u) << (2 * b + 1);
			mortonTable[y * 8 + x] = uint8_t(m);
		}
}
// PICA tiled texel (x, y), y counting memory rows. Memory row 0 is the top of
// the image (texcoord v = 1): conversion writes it to the last RSX row.
inline unsigned tiled(unsigned x, unsigned y, unsigned width) {
	return ((y >> 3) * (width >> 3) + (x >> 3)) * 64 + mortonTable[(y & 7) * 8 + (x & 7)];
}
// A PICA block word: stored little-endian.
inline uint64_t le64(const uint8_t* p) {
	uint64_t v = 0;
	for (int i = 7; i >= 0; --i) v = v << 8 | p[i];
	return v;
}

// ETC1 block decode (Azahar's etc1.cpp): texel index 4x+y, y along memory rows.
const uint8_t Etc1Modifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};
inline int expand5(int v) { return (v << 3) | (v >> 2); }
inline int expand4(int v) { return v * 17; }
void decodeEtc1Texel(uint64_t block, unsigned x, unsigned y, uint8_t* rgb) {
	const unsigned texel = 4 * x + y;
	if ((block >> 32) & 1) std::swap(x, y); // flip
	int r, g, b;
	if ((block >> 33) & 1) { // differential
		auto sext3 = [](int v) { return v & 4 ? v - 8 : v; };
		r = int((block >> 59) & 31); g = int((block >> 51) & 31); b = int((block >> 43) & 31);
		if (x >= 2) { r += sext3(int((block >> 56) & 7)); g += sext3(int((block >> 48) & 7)); b += sext3(int((block >> 40) & 7)); }
		r = expand5(r & 31); g = expand5(g & 31); b = expand5(b & 31);
	} else if (x < 2) {
		r = expand4(int((block >> 60) & 15)); g = expand4(int((block >> 52) & 15)); b = expand4(int((block >> 44) & 15));
	} else {
		r = expand4(int((block >> 56) & 15)); g = expand4(int((block >> 48) & 15)); b = expand4(int((block >> 40) & 15));
	}
	const unsigned table = x < 2 ? unsigned((block >> 37) & 7) : unsigned((block >> 34) & 7);
	int modifier = Etc1Modifiers[table][(block >> texel) & 1];
	if ((block >> (16 + texel)) & 1) modifier = -modifier;
	rgb[0] = uint8_t(std::clamp(r + modifier, 0, 255));
	rgb[1] = uint8_t(std::clamp(g + modifier, 0, 255));
	rgb[2] = uint8_t(std::clamp(b + modifier, 0, 255));
}

size_t levelBytes(GPU_TEXCOLOR fmt, unsigned w, unsigned h) {
	static const uint8_t bits[16] = {32, 24, 16, 16, 16, 16, 16, 8, 8, 8, 4, 4, 4, 8, 0, 0};
	return size_t(w) * h * bits[fmt & 15] / 8;
}
// ETC1 (opaque) becomes R5G6B5, four times its size; everything else A8R8G8B8.
bool isRgb565(GPU_TEXCOLOR fmt) { return fmt == GPU_ETC1; }
inline void putArgb(uint8_t* o, unsigned r, unsigned g, unsigned b, unsigned a) { o[0] = uint8_t(a); o[1] = uint8_t(r); o[2] = uint8_t(g); o[3] = uint8_t(b); }

// Converts one mip level of tiled PICA data for the RSX, with the PICA's
// sampled values (Azahar's texture_decode.cpp).
Converted convertLevel(GPU_TEXCOLOR fmt, unsigned w, unsigned h, const uint8_t* src) {
	Converted c;
	c.w = w; c.h = h;
	if (fmt == GPU_ETC1 || fmt == GPU_ETC1A4) {
		// 8x8 tiles of four 4x4 blocks (x/4 + 2*(y/4)); ETC1A4 blocks are an
		// alpha word then the colour word.
		const bool alpha = fmt == GPU_ETC1A4;
		const unsigned bpp = alpha ? 4 : 2, blockBytes = alpha ? 16 : 8;
		c.rowBytes = w * bpp;
		c.data.assign(size_t(c.rowBytes) * h, 0);
		for (unsigned ty = 0; ty < h / 8; ++ty)
			for (unsigned tx = 0; tx < w / 8; ++tx)
				for (unsigned s = 0; s < 4; ++s) {
					const uint8_t* p = src + ((ty * (w / 8) + tx) * 4 + s) * blockBytes;
					const uint64_t alphaWord = alpha ? le64(p) : 0, color = le64(p + (alpha ? 8 : 0));
					for (unsigned y = 0; y < 4; ++y)
						for (unsigned x = 0; x < 4; ++x) {
							const unsigned px = tx * 8 + (s & 1) * 4 + x, py = ty * 8 + (s >> 1) * 4 + y;
							uint8_t rgb[3];
							decodeEtc1Texel(color, x, y, rgb);
							uint8_t* o = c.data.data() + size_t(h - 1 - py) * c.rowBytes + size_t(px) * bpp;
							if (alpha) putArgb(o, rgb[0], rgb[1], rgb[2], unsigned(expand4(int((alphaWord >> (4 * (x * 4 + y))) & 15))));
							else {
								const unsigned v = unsigned(rgb[0] >> 3) << 11 | unsigned(rgb[1] >> 2) << 5 | unsigned(rgb[2] >> 3);
								o[0] = uint8_t(v >> 8); o[1] = uint8_t(v);
							}
						}
				}
		return c;
	}
	c.rowBytes = w * 4;
	c.data.assign(size_t(c.rowBytes) * h, 0);
	auto u16 = [](const uint8_t* p) { return unsigned(p[0] | p[1] << 8); };
	auto x5 = [](unsigned v) { return (v << 3) | (v >> 2); };
	auto x6 = [](unsigned v) { return (v << 2) | (v >> 4); };
	for (unsigned y = 0; y < h; ++y)
		for (unsigned x = 0; x < w; ++x) {
			const unsigned t = tiled(x, y, w);
			uint8_t* o = c.data.data() + size_t(h - 1 - y) * c.rowBytes + size_t(x) * 4;
			switch (fmt) {
			case GPU_RGBA8: { const uint8_t* p = src + t * 4; putArgb(o, p[3], p[2], p[1], p[0]); break; }
			case GPU_RGB8: { const uint8_t* p = src + t * 3; putArgb(o, p[2], p[1], p[0], 255); break; }
			case GPU_RGBA5551: { const unsigned v = u16(src + t * 2); putArgb(o, x5(v >> 11), x5((v >> 6) & 31), x5((v >> 1) & 31), (v & 1) ? 255 : 0); break; }
			case GPU_RGB565: { const unsigned v = u16(src + t * 2); putArgb(o, x5(v >> 11), x6((v >> 5) & 63), x5(v & 31), 255); break; }
			case GPU_RGBA4: { const unsigned v = u16(src + t * 2); putArgb(o, (v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17); break; }
			case GPU_LA8: { const uint8_t* p = src + t * 2; putArgb(o, p[1], p[1], p[1], p[0]); break; }
			case GPU_HILO8: { const uint8_t* p = src + t * 2; putArgb(o, p[1], p[0], 0, 255); break; }
			case GPU_L8: putArgb(o, src[t], src[t], src[t], 255); break;
			case GPU_A8: putArgb(o, 0, 0, 0, src[t]); break;
			case GPU_LA4: { const unsigned v = src[t]; putArgb(o, (v >> 4) * 17, (v >> 4) * 17, (v >> 4) * 17, (v & 15) * 17); break; }
			case GPU_L4: { const unsigned v = src[t / 2], l = ((t & 1) ? v >> 4 : v & 15) * 17; putArgb(o, l, l, l, 255); break; }
			case GPU_A4: { const unsigned v = src[t / 2]; putArgb(o, 0, 0, 0, ((t & 1) ? v >> 4 : v & 15) * 17); break; }
			default: putArgb(o, 0, 0, 0, 255); break;
			}
		}
	return c;
}
std::vector<Converted> convertTexture(const C3D_Tex& tex) {
	std::vector<Converted> levels;
	const uint8_t* data = static_cast<const uint8_t*>(tex.data);
	for (int level = 0; level <= tex.maxLevel; ++level) {
		const unsigned w = std::max(8u, unsigned(tex.width) >> level), h = std::max(8u, unsigned(tex.height) >> level);
		levels.push_back(convertLevel(tex.fmt, w, h, data));
		data += levelBytes(tex.fmt, w, h);
	}
	return levels;
}
struct UploadStats { unsigned uploads = 0, converted = 0; uint64_t us = 0; } uploadStats;

// Copies converted levels into new RSX memory. Linear mip levels share the
// first level's pitch, one after another (as nouveau lays out NV40 linear
// textures). The previous storage is released when the GPU is done with it.
void uploadLevels(GpuTexture& t, const C3D_Tex& tex, const std::vector<Converted>& levels) {
	if (levels.empty()) return;
	const uint32_t pitch = levels[0].rowBytes;
	size_t total = 0;
	for (const Converted& c : levels) total += size_t(pitch) * c.h;
	void* vram = rsxAlloc(128, uint32_t(total));
	if (!vram) {
		std::printf("c3dps3: out of RSX memory for a %ux%u texture\n", levels[0].w, levels[0].h);
		return;
	}
	uint8_t* out = static_cast<uint8_t*>(vram);
	for (const Converted& c : levels) {
		for (unsigned row = 0; row < c.h; ++row) std::memcpy(out + size_t(row) * pitch, c.data.data() + size_t(row) * c.rowBytes, c.rowBytes);
		out += size_t(pitch) * c.h;
	}
	__sync();
	freeVramLater(t.vram);
	t.vram = vram;
	rsxAddressToOffset(vram, &t.offset);
	t.pitch = pitch;
	t.width = uint16_t(levels[0].w);
	t.height = uint16_t(levels[0].h);
	t.levels = uint8_t(levels.size());
	t.format = uint8_t((isRgb565(tex.fmt) ? GCM_TEXTURE_FORMAT_R5G6B5 : GCM_TEXTURE_FORMAT_A8R8G8B8) | GCM_TEXTURE_FORMAT_LIN);
	t.hasAlpha = !isRgb565(tex.fmt);
	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
}

u32 remap(bool alpha) {
	return GCM_TEXTURE_REMAP_MODE(GCM_TEXTURE_REMAP_ORDER_XYXY, GCM_TEXTURE_REMAP_COLOR_A, GCM_TEXTURE_REMAP_COLOR_R,
		GCM_TEXTURE_REMAP_COLOR_G, GCM_TEXTURE_REMAP_COLOR_B, alpha ? GCM_TEXTURE_REMAP_TYPE_REMAP : GCM_TEXTURE_REMAP_TYPE_ONE,
		GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP, GCM_TEXTURE_REMAP_TYPE_REMAP);
}
void loadTexture(u8 unit, uint32_t offset, u8 location, u8 format, unsigned levels, unsigned w, unsigned h, uint32_t pitch, bool alpha) {
	gcmTexture t;
	std::memset(&t, 0, sizeof t);
	t.format = format;
	t.mipmap = u8(levels);
	t.dimension = GCM_TEXTURE_DIMS_2D;
	t.cubemap = GCM_FALSE;
	t.remap = remap(alpha);
	t.width = u16(w);
	t.height = u16(h);
	t.depth = 1;
	t.location = location;
	t.pitch = pitch;
	t.offset = offset;
	rsxLoadTexture(context, unit, &t);
}

struct BoundUnit { const GpuTexture* texture = nullptr; void* vram = nullptr; u32 param = ~0u; };
BoundUnit boundUnit[3];
void flushClearOfColor(const void* vram);
// Binds tex to a unit, uploading its data first when it changed.
void bindTexture(int unit, const C3D_Tex* tex) {
	LightLock_Lock(&textureLock);
	GpuTexture* t = tex && tex->data ? findTexture(tex->data) : nullptr;
	if (!t) {
		LightLock_Unlock(&textureLock);
		if (boundUnit[unit].texture) { rsxTextureControl(context, u8(unit), GCM_FALSE, 0, 0, GCM_TEXTURE_MAX_ANISO_1); boundUnit[unit] = {}; }
		return;
	}
	const bool upload = t->dirty && !t->renderTarget;
	std::vector<Converted> levels;
	if (upload) levels.swap(t->prepared);
	t->dirty = false;
	LightLock_Unlock(&textureLock);
	// A target sampled before it is drawn again still owes its clear.
	if (t->renderTarget) flushClearOfColor(t->vram);
	if (upload) {
		const uint64_t start = nowUs();
		if (levels.empty()) { levels = convertTexture(*tex); ++uploadStats.converted; }
		uploadLevels(*t, *tex, levels);
		++uploadStats.uploads;
		uploadStats.us += nowUs() - start;
	}
	if (!t->vram) return;
	BoundUnit& b = boundUnit[unit];
	if (b.texture == t && b.vram == t->vram && b.param == tex->param) return;
	b.texture = t; b.vram = t->vram; b.param = tex->param;
	loadTexture(u8(unit), t->offset, GCM_LOCATION_RSX, t->format, t->levels, t->width, t->height, t->pitch, t->hasAlpha);
	const bool magLinear = tex->param & GPU_TEXTURE_MAG_FILTER(GPU_LINEAR);
	const bool minLinear = tex->param & GPU_TEXTURE_MIN_FILTER(GPU_LINEAR);
	const bool mipLinear = tex->param & GPU_TEXTURE_MIP_FILTER(GPU_LINEAR);
	u8 minFilter = minLinear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST;
	if (t->levels > 1)
		minFilter = minLinear ? (mipLinear ? GCM_TEXTURE_LINEAR_MIPMAP_LINEAR : GCM_TEXTURE_LINEAR_MIPMAP_NEAREST)
		                      : (mipLinear ? GCM_TEXTURE_NEAREST_MIPMAP_LINEAR : GCM_TEXTURE_NEAREST_MIPMAP_NEAREST);
	static const u8 wraps[4] = {GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_REPEAT, GCM_TEXTURE_MIRRORED_REPEAT};
	rsxTextureControl(context, u8(unit), GCM_TRUE, 0, u16((t->levels - 1) << 8), GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, u8(unit), 0, minFilter, magLinear ? GCM_TEXTURE_LINEAR : GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, u8(unit), wraps[(tex->param >> 12) & 3], wraps[(tex->param >> 8) & 3], GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

// ---- Render targets ---------------------------------------------------------------
struct Target {
	C3D_RenderTarget c3d; // first: C3D_RenderTarget* converts to Target*
	void* color = nullptr; void* depth = nullptr;
	uint32_t colorOffset = 0, depthOffset = 0, colorPitch = 0, depthPitch = 0;
	bool ownsColor = true, hasDepth = false;
	int width = 0, height = 0; // scaled
	// A clear waiting for the target's next use (C3D_FrameBufClear).
	bool clearColor = false, clearDepth = false;
	uint32_t clearArgb = 0, clearDepthStencil = 0;
};
Target* targetOf(C3D_RenderTarget* t) { return reinterpret_cast<Target*>(t); }
Target* targetOfFrameBuf(C3D_FrameBuf* fb) {
	return reinterpret_cast<Target*>(reinterpret_cast<uint8_t*>(fb) - offsetof(C3D_RenderTarget, frameBuf));
}
std::vector<Target*> targets;
Target* topOutput = nullptr;

uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
bool createTargetStorage(Target& t, void* colorVram) {
	t.colorPitch = alignUp(uint32_t(t.width) * 4, 64);
	t.depthPitch = t.colorPitch;
	if (colorVram) t.color = colorVram;
	else if (!(t.color = rsxAlloc(4096, t.colorPitch * uint32_t(t.height)))) return false;
	// Every surface has a depth buffer; a depth-less target never tests it.
	if (!(t.depth = rsxAlloc(4096, t.depthPitch * uint32_t(t.height)))) return false;
	rsxAddressToOffset(t.color, &t.colorOffset);
	rsxAddressToOffset(t.depth, &t.depthOffset);
	return true;
}

// ---- GPU state ------------------------------------------------------------------------
struct State {
	C3D_TexEnv env[6];
	u32 envBuf = 0, envBufColor = 0xFFFFFFFF;
	C3D_AttrInfo attr{};
	C3D_BufInfo buf{};
	C3D_Tex* tex[3] = {};
	uint8_t program = ProgramScene;
	// Effect state, as citro3d's C3D_Effect fields.
	bool depthEnable = true; u8 depthFunc = GPU_GREATER; u8 writeMask = GPU_WRITE_ALL;
	bool alphaEnable = false; u8 alphaFunc = GPU_ALWAYS; u8 alphaRef = 0;
	u8 blendEq[2] = {GPU_BLEND_ADD, GPU_BLEND_ADD};
	u8 blendFactor[4] = {GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA};
	u32 blendColor = 0;
	u8 cull = GPU_CULL_BACK_CCW;
	bool stencilEnable = false; u8 stencilFunc = GPU_ALWAYS; u8 stencilRef = 0, stencilInMask = 0xFF, stencilWriteMask = 0;
	u8 stencilOp[3] = {GPU_STENCIL_KEEP, GPU_STENCIL_KEEP, GPU_STENCIL_KEEP};
	int viewport[4] = {0, 0, 240, 400};
	bool scissor = false; int scissorBox[4] = {};
	Target* target = nullptr;
} state;

uint64_t frameNumber = 0;
bool profile = false; // ux0:data/mkw/profile.txt

u32 testFunc(u8 f) {
	static const u32 funcs[8] = {GCM_NEVER, GCM_ALWAYS, GCM_EQUAL, GCM_NOTEQUAL, GCM_LESS, GCM_LEQUAL, GCM_GREATER, GCM_GEQUAL};
	return funcs[f & 7];
}
u16 blendFactor(u8 f) {
	static const u16 factors[15] = {GCM_ZERO, GCM_ONE, GCM_SRC_COLOR, GCM_ONE_MINUS_SRC_COLOR, GCM_DST_COLOR, GCM_ONE_MINUS_DST_COLOR,
		GCM_SRC_ALPHA, GCM_ONE_MINUS_SRC_ALPHA, GCM_DST_ALPHA, GCM_ONE_MINUS_DST_ALPHA,
		GCM_CONSTANT_COLOR, GCM_ONE_MINUS_CONSTANT_COLOR, GCM_CONSTANT_ALPHA, GCM_ONE_MINUS_CONSTANT_ALPHA, GCM_SRC_ALPHA_SATURATE};
	return f < 15 ? factors[f] : GCM_ONE;
}
u16 blendEquation(u8 e) {
	static const u16 eqs[5] = {GCM_FUNC_ADD, GCM_FUNC_SUBTRACT, GCM_FUNC_REVERSE_SUBTRACT, GCM_FUNC_MIN, GCM_FUNC_MAX};
	return e < 5 ? eqs[e] : GCM_FUNC_ADD;
}
u32 stencilOperation(u8 op) {
	static const u32 ops[8] = {GCM_KEEP, GCM_ZERO, GCM_REPLACE, GCM_INCR, GCM_DECR, GCM_INVERT, GCM_INCR_WRAP, GCM_DECR_WRAP};
	return ops[op & 7];
}
// PICA 0xAABBGGRR to the RSX's 0xAARRGGBB.
u32 argb(u32 c) { return (c & 0xFF00FF00u) | (c & 0xFF) << 16 | (c >> 16 & 0xFF); }

// Window coordinates with GL's lower-left origin: NDC y = -1 is row 0.
void setViewport(int x, int y, int w, int h) {
	const float scale[4] = {w * 0.5f, h * 0.5f, 1.f, 0.f};
	const float offset[4] = {x + w * 0.5f, y + h * 0.5f, 0.f, 0.f};
	rsxSetViewport(context, u16(x), u16(y), u16(w), u16(h), 0.f, 1.f, scale, offset);
}
void bindSurface(uint32_t colorOffset, uint32_t colorPitch, uint32_t depthOffset, uint32_t depthPitch, int width, int height) {
	gcmSurface sf;
	std::memset(&sf, 0, sizeof sf);
	sf.colorFormat = GCM_SURFACE_A8R8G8B8;
	sf.colorTarget = GCM_SURFACE_TARGET_0;
	sf.colorLocation[0] = GCM_LOCATION_RSX;
	sf.colorOffset[0] = colorOffset;
	sf.colorPitch[0] = colorPitch;
	for (int i = 1; i < GCM_MAX_MRT_COUNT; ++i) { sf.colorLocation[i] = GCM_LOCATION_RSX; sf.colorOffset[i] = 0; sf.colorPitch[i] = 64; }
	sf.depthFormat = GCM_SURFACE_ZETA_Z24S8;
	sf.depthLocation = GCM_LOCATION_RSX;
	sf.depthOffset = depthOffset;
	sf.depthPitch = depthPitch;
	sf.type = GCM_SURFACE_TYPE_LINEAR;
	sf.antiAlias = GCM_SURFACE_CENTER_1;
	sf.width = u16(width);
	sf.height = u16(height);
	sf.x = 0;
	sf.y = 0;
	rsxSetSurface(context, &sf);
	for (u8 i = 0; i < 8; ++i) rsxSetViewportClip(context, i, u16(width), u16(height));
}

// The RSX state last applied; only differences are sent. Clears and the
// present change state behind it: they invalidate the copy.
struct AppliedState {
	bool valid = false;
	Target* target = nullptr;
	float scale = 0.f;
	int viewport[4] = {};
	bool scissor = false; int scissorBox[4] = {};
	bool depthEnable = false; u8 depthFunc = 0, writeMask = 0;
	u8 blendEq[2] = {}, blendFactor[4] = {};
	u32 blendColor = 0;
	u8 cull = 0;
	bool stencilEnable = false; u8 stencilFunc = 0, stencilRef = 0, stencilInMask = 0, stencilWriteMask = 0, stencilOp[3] = {};
	const ProgramBlob* vertex = nullptr;
	const ProgramBlob* fragment = nullptr;
	uint32_t constantRow = ~0u;
	uint16_t attribEnabled = 0;
} applied;
// The vertex arrays stay as they are: attribEnabled still describes them.
void invalidateApplied() {
	const uint16_t attribEnabled = applied.attribEnabled;
	applied = AppliedState{};
	applied.attribEnabled = attribEnabled;
	for (auto& b : boundUnit) b = {};
}

// Clears wait for the target's first use (as the Vita port's): done just
// before the target is drawn, sampled or presented.
void flushClear(Target* t) {
	if (!t->clearColor && !t->clearDepth) return;
	bindSurface(t->colorOffset, t->colorPitch, t->depthOffset, t->depthPitch, t->width, t->height);
	rsxSetScissor(context, 0, 0, 4095, 4095);
	u32 mask = 0;
	if (t->clearColor) {
		rsxSetColorMask(context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
		rsxSetClearColor(context, t->clearArgb);
		mask |= GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A;
	}
	if (t->clearDepth) {
		rsxSetDepthWriteEnable(context, GCM_TRUE);
		rsxSetStencilMask(context, 0xFF);
		rsxSetClearDepthStencil(context, t->clearDepthStencil);
		mask |= GCM_CLEAR_Z | GCM_CLEAR_S;
	}
	rsxClearSurface(context, mask);
	checkCommands("clear");
	t->clearColor = t->clearDepth = false;
	invalidateApplied();
}
void flushClearOfColor(const void* vram) {
	for (Target* t : targets)
		if (t->color == vram && (t->clearColor || t->clearDepth)) flushClear(t);
}

void applyFixedState() {
	AppliedState& a = applied;
	Target* t = state.target;
	// Binds the target and clears it; the clear's state changes invalidate a.
	if (t->clearColor || t->clearDepth) flushClear(t);
	const bool all = !a.valid;
	if (all || a.target != t) {
		bindSurface(t->colorOffset, t->colorPitch, t->depthOffset, t->depthPitch, t->width, t->height);
		a.target = t;
	}
	const float scale = t == topOutput ? Scale / Divisor : Scale;
	if (all || a.target != t || std::memcmp(a.viewport, state.viewport, sizeof a.viewport) || a.scale != scale) {
		setViewport(scaled(state.viewport[0], scale), scaled(state.viewport[1], scale), scaledSpan(state.viewport[0], state.viewport[2], scale), scaledSpan(state.viewport[1], state.viewport[3], scale));
		std::memcpy(a.viewport, state.viewport, sizeof a.viewport);
	}
	if (all || a.target != t || a.scissor != state.scissor || a.scale != scale || (state.scissor && std::memcmp(a.scissorBox, state.scissorBox, sizeof a.scissorBox))) {
		if (state.scissor)
			rsxSetScissor(context, u16(std::max(0, scaled(state.scissorBox[0], scale))), u16(std::max(0, scaled(state.scissorBox[1], scale))),
				u16(std::max(0, scaledSpan(state.scissorBox[0], state.scissorBox[2], scale))), u16(std::max(0, scaledSpan(state.scissorBox[1], state.scissorBox[3], scale))));
		else rsxSetScissor(context, 0, 0, 4095, 4095);
		a.scissor = state.scissor; std::memcpy(a.scissorBox, state.scissorBox, sizeof a.scissorBox);
	}
	a.scale = scale;
	// PICA writes depth only while the depth test is on; a depth-less target
	// never tests or writes it.
	const bool depthOn = state.depthEnable && t->hasDepth;
	if (all || a.depthEnable != depthOn || a.depthFunc != state.depthFunc || a.writeMask != state.writeMask) {
		rsxSetDepthTestEnable(context, depthOn ? GCM_TRUE : GCM_FALSE);
		rsxSetDepthFunc(context, testFunc(state.depthFunc));
		rsxSetDepthWriteEnable(context, depthOn && (state.writeMask & GPU_WRITE_DEPTH) ? GCM_TRUE : GCM_FALSE);
		u32 mask = 0;
		if (state.writeMask & GPU_WRITE_RED) mask |= GCM_COLOR_MASK_R;
		if (state.writeMask & GPU_WRITE_GREEN) mask |= GCM_COLOR_MASK_G;
		if (state.writeMask & GPU_WRITE_BLUE) mask |= GCM_COLOR_MASK_B;
		if (state.writeMask & GPU_WRITE_ALPHA) mask |= GCM_COLOR_MASK_A;
		rsxSetColorMask(context, mask);
		a.depthEnable = depthOn; a.depthFunc = state.depthFunc; a.writeMask = state.writeMask;
	}
	if (all) {
		rsxSetBlendEnable(context, GCM_TRUE);
		rsxSetAlphaTestEnable(context, GCM_FALSE);
		rsxSetDitherEnable(context, GCM_FALSE);
		rsxSetLogicOpEnable(context, GCM_FALSE);
		rsxSetShadeModel(context, GCM_SHADE_MODEL_SMOOTH);
		rsxSetZMinMaxControl(context, GCM_TRUE, GCM_FALSE, GCM_FALSE);
		rsxSetUserClipPlaneControl(context, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE,
			GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE, GCM_USER_CLIP_PLANE_DISABLE);
		rsxSetColorMaskMrt(context, 0);
		rsxSetTwoSidedStencilTestEnable(context, GCM_FALSE);
	}
	if (all || std::memcmp(a.blendEq, state.blendEq, 2) || std::memcmp(a.blendFactor, state.blendFactor, 4) || a.blendColor != state.blendColor) {
		rsxSetBlendEquation(context, blendEquation(state.blendEq[0]), blendEquation(state.blendEq[1]));
		rsxSetBlendFunc(context, blendFactor(state.blendFactor[0]), blendFactor(state.blendFactor[1]),
			blendFactor(state.blendFactor[2]), blendFactor(state.blendFactor[3]));
		rsxSetBlendColor(context, argb(state.blendColor), 0);
		std::memcpy(a.blendEq, state.blendEq, 2); std::memcpy(a.blendFactor, state.blendFactor, 4); a.blendColor = state.blendColor;
	}
	if (all || a.cull != state.cull) {
		if (state.cull == GPU_CULL_NONE) rsxSetCullFaceEnable(context, GCM_FALSE);
		else {
			// GL's counter-clockwise front, seen through the RSX's upper-left
			// winding rule with these lower-left window coordinates.
			rsxSetCullFaceEnable(context, GCM_TRUE);
			rsxSetFrontFace(context, GCM_FRONTFACE_CW);
			rsxSetCullFace(context, state.cull == GPU_CULL_FRONT_CCW ? GCM_CULL_FRONT : GCM_CULL_BACK);
		}
		a.cull = state.cull;
	}
	const bool stencilOn = state.stencilEnable && t->hasDepth;
	if (all || a.stencilEnable != stencilOn || (stencilOn && (a.stencilFunc != state.stencilFunc || a.stencilRef != state.stencilRef ||
		a.stencilInMask != state.stencilInMask || a.stencilWriteMask != state.stencilWriteMask || std::memcmp(a.stencilOp, state.stencilOp, 3)))) {
		rsxSetStencilTestEnable(context, stencilOn ? GCM_TRUE : GCM_FALSE);
		if (stencilOn) {
			rsxSetStencilFunc(context, testFunc(state.stencilFunc), state.stencilRef, state.stencilInMask);
			rsxSetStencilMask(context, state.stencilWriteMask);
			rsxSetStencilOp(context, stencilOperation(state.stencilOp[0]), stencilOperation(state.stencilOp[1]), stencilOperation(state.stencilOp[2]));
		}
		a.stencilEnable = stencilOn; a.stencilFunc = state.stencilFunc; a.stencilRef = state.stencilRef;
		a.stencilInMask = state.stencilInMask; a.stencilWriteMask = state.stencilWriteMask; std::memcpy(a.stencilOp, state.stencilOp, 3);
	}
	a.valid = true;
}

// ---- Programs and constants ----------------------------------------------------------
bool constantsUploaded = false;
void loadVertexProgram(const ProgramBlob& p) {
	if (applied.vertex == &p) return;
	auto* vp = reinterpret_cast<const rsxVertexProgram*>(p.bytes.data());
	void* ucode = nullptr; u32 size = 0;
	rsxVertexProgramGetUCode(vp, &ucode, &size);
	rsxLoadVertexProgram(context, vp, ucode);
	applied.vertex = &p;
}
void loadFragmentProgram(const ProgramBlob& p) {
	if (applied.fragment == &p) return;
	rsxLoadFragmentProgramLocation(context, reinterpret_cast<const rsxFragmentProgram*>(p.bytes.data()), p.ucodeOffset, GCM_LOCATION_RSX);
	applied.fragment = &p;
}
// The PICA float uniforms the programs read, when changed (c0-c24; the bone
// palette is read by skinVertices).
void uploadVertexConstants() {
	bool dirty = !constantsUploaded;
	for (int i = 0; i < SceneRegisters; ++i)
		if (C3D_FVUnifDirty[0][i]) { C3D_FVUnifDirty[0][i] = false; dirty = true; }
	for (int i = SceneRegisters; i < C3D_FVUNIF_COUNT; ++i) C3D_FVUnifDirty[0][i] = false;
	C3D_BoolUnifsDirty[0] = false;
	if (!dirty) return;
	rsxLoadVertexProgramParameterBlock(context, 0, SceneRegisters, reinterpret_cast<const f32*>(C3D_FVUnif[0]));
	if (!constantsUploaded) rsxLoadVertexProgramParameterBlock(context, ConstLiterals, 2, vertexLiterals());
	constantsUploaded = true;
}
// The combiner constants, buffer colour and alpha reference: a row of eight
// texels in scratch memory on the constant unit, sampled by the program.
void bindConstantRow() {
	uint32_t key[ConstantTexels];
	for (int i = 0; i < 6; ++i) key[i] = state.env[i].color;
	key[6] = state.envBufColor;
	key[7] = u32(state.alphaRef) << 24;
	static uint32_t last[ConstantTexels];
	if (applied.constantRow != ~0u && !std::memcmp(key, last, sizeof key)) return;
	std::memcpy(last, key, sizeof key);
	uint8_t* row = scratchAlloc(ConstantTexels * 4);
	for (int i = 0; i < ConstantTexels; ++i) {
		const u32 c = key[i];
		putArgb(row + i * 4, c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24);
	}
	applied.constantRow = ioOffset(row);
	loadTexture(ConstantUnit, applied.constantRow, GCM_LOCATION_CELL, GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN, 1, ConstantTexels, 1, ConstantTexels * 4, true);
	rsxTextureControl(context, ConstantUnit, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, ConstantUnit, 0, GCM_TEXTURE_NEAREST, GCM_TEXTURE_NEAREST, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, ConstantUnit, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
}

// ---- Vertex attributes and skinning ----------------------------------------------------
// Where an input register's data lives: buffer, byte offset in its record,
// format (GPU_FORMATS) and components.
struct AttributeSource { int buffer = -1; unsigned offset = 0, format = 0, components = 0, stride = 0; };
void attributeSources(AttributeSource (&out)[16]) {
	for (auto& a : out) a = {};
	for (int b = 0; b < state.buf.bufCount; ++b) {
		const C3D_BufCfg& cfg = state.buf.buffers[b];
		const u64 permutation = u64(cfg.flags[0]) | (u64(cfg.flags[1] & 0xFFFF) << 32);
		const unsigned stride = (cfg.flags[1] >> 16) & 0xFFF, count = cfg.flags[1] >> 28;
		unsigned offset = 0;
		for (unsigned j = 0; j < count; ++j) {
			const unsigned loader = (permutation >> (4 * j)) & 0xF;
			if (loader >= 12) { offset += 4 * (loader - 11); continue; } // padding entries
			const unsigned reg = (state.attr.permutation >> (4 * loader)) & 0xF;
			const u32 flags = loader < 8 ? state.attr.flags[0] >> (4 * loader) : state.attr.flags[1] >> (4 * (loader - 8));
			const unsigned format = flags & 3, components = ((flags >> 2) & 3) + 1;
			static const unsigned sizes[4] = {1, 1, 2, 4};
			out[reg] = {b, offset, format, components, stride};
			offset += components * sizes[format];
		}
	}
}
float readFloat(const uint8_t* p) { float f; std::memcpy(&f, p, 4); return f; }
void writeFloat(uint8_t* p, float f) { std::memcpy(p, &f, 4); }
// The PICA program's skinning (C3D_BoolUnifs bit 3, normals with bit 4), done
// on vertices [first, end) into scratch copies of the buffers holding the
// position (v0) and normal (v1). Returns each buffer's replacement base.
void skinVertices(const AttributeSource (&src)[16], uint32_t first, uint32_t end, const uint8_t* (&bases)[12]) {
	const AttributeSource& pos = src[0];
	const AttributeSource& nrm = src[1];
	const bool normals = C3D_BoolUnifs[0] & 16;
	const AttributeSource& bone = normals ? src[4] : src[1];
	if (pos.buffer < 0 || pos.format != GPU_FLOAT || bone.buffer < 0 || bone.format != GPU_FLOAT || end <= first) return;
	if (normals && (nrm.buffer < 0 || nrm.format != GPU_FLOAT)) return;
	const uint8_t* original[12];
	for (int b = 0; b < 12; ++b) original[b] = bases[b];
	// Copy the buffers that change.
	for (int b : {pos.buffer, normals ? nrm.buffer : -1}) {
		if (b < 0 || bases[b] != original[b]) continue;
		const unsigned stride = state.buf.buffers[b].flags[1] >> 16 & 0xFFF;
		const size_t bytes = size_t(end - first) * stride;
		uint8_t* copy = scratchAlloc(uint32_t(bytes));
		if (!copy) return;
		std::memcpy(copy, original[b] + size_t(first) * stride, bytes);
		bases[b] = copy - size_t(first) * stride;
	}
	auto field = [&](const uint8_t* const* from, const AttributeSource& a, uint32_t v) { return from[a.buffer] + size_t(v) * a.stride + a.offset; };
	const C3D_FVec* bones = &C3D_FVUnif[0][25];
	constexpr int BoneRegisters = 57;
	for (uint32_t v = first; v < end; ++v) {
		const uint8_t* p = field(original, pos, v);
		const float px = readFloat(p), py = readFloat(p + 4), pz = readFloat(p + 8);
		const int a = int(std::floor(readFloat(field(original, bone, v)) + 0.5f));
		if (a < 0 || a + 3 > BoneRegisters) continue;
		const C3D_FVec& b0 = bones[a]; const C3D_FVec& b1 = bones[a + 1]; const C3D_FVec& b2 = bones[a + 2];
		uint8_t* out = const_cast<uint8_t*>(field(bases, pos, v));
		writeFloat(out, b0.x * px + b0.y * py + b0.z * pz + b0.w);
		writeFloat(out + 4, b1.x * px + b1.y * py + b1.z * pz + b1.w);
		writeFloat(out + 8, b2.x * px + b2.y * py + b2.z * pz + b2.w);
		if (!normals) continue;
		const uint8_t* q = field(original, nrm, v);
		const float n[3] = {readFloat(q), readFloat(q + 4), readFloat(q + 8)};
		// Cofactors of the bone's linear part: normals follow its inverse transpose.
		const float k0[3] = {b1.y * b2.z - b1.z * b2.y, b1.z * b2.x - b1.x * b2.z, b1.x * b2.y - b1.y * b2.x};
		const float k1[3] = {b2.y * b0.z - b2.z * b0.y, b2.z * b0.x - b2.x * b0.z, b2.x * b0.y - b2.y * b0.x};
		const float k2[3] = {b0.y * b1.z - b0.z * b1.y, b0.z * b1.x - b0.x * b1.z, b0.x * b1.y - b0.y * b1.x};
		const float det = b0.x * k0[0] + b0.y * k0[1] + b0.z * k0[2];
		float m[3];
		if (det == 0.f) {
			m[0] = b0.x * n[0] + b0.y * n[1] + b0.z * n[2];
			m[1] = b1.x * n[0] + b1.y * n[1] + b1.z * n[2];
			m[2] = b2.x * n[0] + b2.y * n[1] + b2.z * n[2];
		} else {
			const float s = det >= 0.f ? 1.f : -1.f;
			m[0] = (k0[0] * n[0] + k0[1] * n[1] + k0[2] * n[2]) * s;
			m[1] = (k1[0] * n[0] + k1[1] * n[1] + k1[2] * n[2]) * s;
			m[2] = (k2[0] * n[0] + k2[1] * n[1] + k2[2] * n[2]) * s;
		}
		const float inv = 1.f / std::sqrt(std::max(1e-12f, m[0] * m[0] + m[1] * m[1] + m[2] * m[2]));
		uint8_t* no = const_cast<uint8_t*>(field(bases, nrm, v));
		writeFloat(no, m[0] * inv); writeFloat(no + 4, m[1] * inv); writeFloat(no + 8, m[2] * inv);
	}
}

struct FrameStats {
	unsigned frames = 0, draws = 0, skinned = 0;
	uint64_t frameUs = 0, lastPresentUs = 0, beginWaitUs = 0, workUs = 0, frameStartUs = 0, drawUs = 0;
} stats;

// Vertex range [first, end) of a draw, for skinning.
void prepareDraw(uint32_t first, uint32_t end) {
	const bool skinned = state.program == ProgramScene && (C3D_BoolUnifs[0] & 8);
	FragmentKey key;
	std::memset(&key, 0, sizeof key);
	for (int i = 0; i < 6; ++i) {
		const C3D_TexEnv& e = state.env[i];
		key.srcRgb[i] = e.srcRgb; key.srcAlpha[i] = e.srcAlpha;
		key.opRgb[i] = u16(e.opRgb); key.opAlpha[i] = u16(e.opAlpha);
		key.funcRgb[i] = e.funcRgb; key.funcAlpha[i] = e.funcAlpha;
		key.scaleRgb[i] = e.scaleRgb; key.scaleAlpha[i] = e.scaleAlpha;
	}
	key.bufferUpdate = state.envBuf;
	key.alphaFunc = state.alphaEnable ? state.alphaFunc : 0xFF;
	for (int u = 0; u < 3; ++u) if (state.tex[u] && state.tex[u]->data) key.units |= 1 << u;
	key.projection0 = state.tex[0] && C3D_TexGetType(state.tex[0]) == GPU_TEX_PROJECTION;
	// Consecutive draws usually share their combiner: skip the lookup.
	static FragmentKey lastKey;
	static const ProgramBlob* lastFragment = nullptr;
	const ProgramBlob& fragment = lastFragment && !std::memcmp(&key, &lastKey, sizeof key) ? *lastFragment : fragmentProgram(key);
	lastKey = key; lastFragment = &fragment;

	checkCommands("draw start");
	for (int u = 0; u < 3; ++u) bindTexture(u, state.tex[u]);
	checkCommands("textures");
	applyFixedState();
	checkCommands("fixed state");
	loadVertexProgram(vertexProgram(state.program, C3D_BoolUnifs[0] & 7));
	checkCommands("vertex program");
	loadFragmentProgram(fragment);
	checkCommands("fragment program");
	uploadVertexConstants();
	checkCommands("vertex constants");
	bindConstantRow();
	checkCommands("constant row");

	// Attribute loaders (AttrInfo) fed from the buffers (BufInfo) in place.
	AttributeSource src[16];
	attributeSources(src);
	const uint8_t* bases[12] = {};
	for (int b = 0; b < state.buf.bufCount; ++b) bases[b] = reinterpret_cast<const uint8_t*>(uintptr_t(state.buf.buffers[b].offset));
	if (skinned) { skinVertices(src, first, end, bases); ++stats.skinned; }
	uint16_t enabled = 0;
	for (unsigned reg = 0; reg < 16; ++reg) {
		const AttributeSource& a = src[reg];
		if (a.buffer < 0) continue;
		static const u8 types[4] = {VertexUB256, VertexUB256, VertexS32K, GCM_VERTEX_DATA_TYPE_F32};
		rsxBindVertexArrayAttrib(context, u8(reg), 0, ioOffset(bases[a.buffer] + a.offset), u8(a.stride), u8(a.components), types[a.format], GCM_LOCATION_CELL);
		enabled |= 1 << reg;
	}
	// Loaders the previous draw used and this one does not: off.
	for (unsigned reg = 0; reg < 16; ++reg)
		if ((applied.attribEnabled >> reg & 1) && !(enabled >> reg & 1))
			rsxBindVertexArrayAttrib(context, u8(reg), 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	applied.attribEnabled = enabled;
}

u32 primitive(GPU_Primitive_t p) {
	switch (p) {
	case GPU_TRIANGLE_STRIP: return GCM_TYPE_TRIANGLE_STRIP;
	case GPU_TRIANGLE_FAN: return GCM_TYPE_TRIANGLE_FAN;
	default: return GCM_TYPE_TRIANGLES;
	}
}
void afterDraw() {
	checkCommands("draw");
	++stats.draws;
	if (stats.draws % 32 == 0) rsxFlushBuffer(context); // let the GPU start
}

// The quad that shows a portrait target on the landscape display.
void present(Target* top) {
	flushClear(top); // cleared but never drawn: show the clear
	bindSurface(displayOffset[backBuffer], displayPitch, displayDepthOffset, displayDepthPitch, int(displayWidth), int(displayHeight));
	// The display's rows run top to bottom: NDC y = +1 is row 0.
	const float scale[4] = {displayWidth * 0.5f, displayHeight * -0.5f, 1.f, 0.f};
	const float offset[4] = {displayWidth * 0.5f, displayHeight * 0.5f, 0.f, 0.f};
	rsxSetViewport(context, 0, 0, u16(displayWidth), u16(displayHeight), 0.f, 1.f, scale, offset);
	rsxSetScissor(context, 0, 0, 4095, 4095);
	rsxSetDepthTestEnable(context, GCM_FALSE);
	rsxSetDepthWriteEnable(context, GCM_FALSE);
	rsxSetStencilTestEnable(context, GCM_FALSE);
	rsxSetCullFaceEnable(context, GCM_FALSE);
	rsxSetBlendEnable(context, GCM_FALSE);
	rsxSetColorMask(context, GCM_COLOR_MASK_R | GCM_COLOR_MASK_G | GCM_COLOR_MASK_B | GCM_COLOR_MASK_A);
	rsxSetClearColor(context, 0xFF000000);
	rsxClearSurface(context, GCM_CLEAR_R | GCM_CLEAR_G | GCM_CLEAR_B | GCM_CLEAR_A);
	// The landscape picture (the portrait target turned) at the canvas's
	// shape on the display's: 16:9 fills a 16:9 display, a 4:3 one is letterboxed.
	const float canvas = float(top->c3d.frameBuf.height) / float(top->c3d.frameBuf.width);
	float sx = 1.f, sy = 1.f;
	if (canvas < displayAspect) sx = canvas / displayAspect;
	else sy = displayAspect / canvas;
	// Landscape (x, y from the top-left) shows portrait window (240 - y, W - x)
	// of a W-wide canvas: u = 1 - y/240, v = 1 - x/W.
	auto* quad = reinterpret_cast<float*>(scratchAlloc(4 * 4 * sizeof(float)));
	// A divided frame fills only the target's lower-left corner.
	const float su = float(scaled(top->c3d.frameBuf.width, Scale / Divisor)) / float(top->width);
	const float sv = float(scaled(top->c3d.frameBuf.height, Scale / Divisor)) / float(top->height);
	const float vertices[4][4] = {{-sx, sy, su, sv}, {sx, sy, su, 0}, {-sx, -sy, 0, sv}, {sx, -sy, 0, 0}}; // x, y, u, v
	std::memcpy(quad, vertices, sizeof vertices);
	loadTexture(0, top->colorOffset, GCM_LOCATION_RSX, GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN, 1, unsigned(top->width), unsigned(top->height), top->colorPitch, true);
	rsxTextureControl(context, 0, GCM_TRUE, 0, 0, GCM_TEXTURE_MAX_ANISO_1);
	rsxTextureFilter(context, 0, 0, GCM_TEXTURE_LINEAR, GCM_TEXTURE_LINEAR, GCM_TEXTURE_CONVOLUTION_QUINCUNX);
	rsxTextureWrapMode(context, 0, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, GCM_TEXTURE_CLAMP_TO_EDGE, 0, GCM_TEXTURE_ZFUNC_LESS, 0);
	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
	const ProgramBlob& vp = presentVertexProgram();
	const ProgramBlob& fp = presentFragmentProgram();
	void* ucode = nullptr; u32 size = 0;
	rsxVertexProgramGetUCode(reinterpret_cast<const rsxVertexProgram*>(vp.bytes.data()), &ucode, &size);
	rsxLoadVertexProgram(context, reinterpret_cast<const rsxVertexProgram*>(vp.bytes.data()), ucode);
	rsxLoadFragmentProgramLocation(context, reinterpret_cast<const rsxFragmentProgram*>(fp.bytes.data()), fp.ucodeOffset, GCM_LOCATION_RSX);
	const uint32_t at = ioOffset(quad);
	for (unsigned reg = 2; reg < 16; ++reg)
		if (applied.attribEnabled >> reg & 1) rsxBindVertexArrayAttrib(context, u8(reg), 0, 0, 0, 0, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_RSX);
	rsxBindVertexArrayAttrib(context, 0, 0, at, 16, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
	rsxBindVertexArrayAttrib(context, 1, 0, at + 8, 16, 2, GCM_VERTEX_DATA_TYPE_F32, GCM_LOCATION_CELL);
	rsxDrawVertexArray(context, GCM_TYPE_TRIANGLE_STRIP, 0, 4);
	applied.attribEnabled = 0x3;
	invalidateApplied();
	// Show it at the next vertical blank; the RSX waits for that before any
	// later command, and the frame's fence follows (C3D_FrameBegin waits on it).
	gcmSetFlip(context, u8(backBuffer));
	rsxFlushBuffer(context);
	gcmSetWaitFlip(context);
	checkCommands("present");
	backBuffer ^= 1;
}

// Display refresh counting (C3D_FrameCounter, gspWaitForVBlank).
std::atomic<uint32_t> vblanks{0};
void onVblank(const u32) { vblanks.fetch_add(1, std::memory_order_relaxed); }

} // namespace

namespace c3dps3 {
void* rsxAlloc(uint32_t alignment, uint32_t size) {
	LightLock_Lock(&rsxHeapLock);
	void* p = rsxMemalign(alignment, size);
	LightLock_Unlock(&rsxHeapLock);
	return p;
}

// ---- Display start-up (gfxInitDefault) and LINEAR memory -------------------------------
void initDisplay() {
	if (displayReady) return;
	displayReady = true;
	// printf output (this layer's status lines) goes to a log beside the game files.
	mkdir("ux0:data", 0777);
	mkdir(DataDir, 0777);
	if (std::freopen("ux0:data/mkw/ps3.log", "w", stdout)) std::setvbuf(stdout, nullptr, _IOLBF, 0);
	initMorton();
	LightLock_Init(&textureLock);
	LightLock_Init(&rsxHeapLock);
	auto marker = [](const char* path) { FILE* f = std::fopen(path, "rb"); if (f) std::fclose(f); return f != nullptr; };
	profile = marker("ux0:data/mkw/profile.txt");
	if (FILE* f = std::fopen("ux0:data/mkw/quality.txt", "rb")) {
		char line[128];
		float value = 0;
		while (std::fgets(line, sizeof line, f))
			if (std::sscanf(line, " resolution = %f", &value) == 1) Scale = std::clamp(value, 1.f, 4.f);
		std::fclose(f);
	}
	std::printf("c3dps3: internal resolution %.2fx\n", double(Scale));

	void* io = memalign(1u << 20, IoSize);
	if (!io || rsxInit(&context, CommandSize, IoSize, io) != 0) { std::printf("c3dps3: rsxInit failed\n"); std::abort(); }
	commandBegin = context->begin;
	commandHalfWords = uint32_t(reinterpret_cast<u32*>(static_cast<uint8_t*>(io) + CommandSize) - commandBegin) / 2;
	context->end = commandBegin + commandHalfWords - 1;
	checkedTo = context->current;
	linear.init(static_cast<uint8_t*>(io) + CommandSize, LinearSize);
	scratchBase = static_cast<uint8_t*>(io) + CommandSize + LinearSize;
	fenceLabel = gcmGetLabelAddress(FenceLabel);
	*fenceLabel = 0;

	// The output the system is set to (720p, 1080i/p, 480p...).
	videoState video;
	videoResolution resolution;
	if (videoGetState(0, 0, &video) != 0 || videoGetResolution(video.displayMode.resolution, &resolution) != 0) {
		std::printf("c3dps3: no video output state\n");
		std::abort();
	}
	displayWidth = resolution.width;
	displayHeight = resolution.height;
	displayAspect = video.displayMode.aspect == VIDEO_ASPECT_4_3 ? 4.f / 3.f : 16.f / 9.f;
	videoConfiguration config;
	std::memset(&config, 0, sizeof config);
	config.resolution = video.displayMode.resolution;
	config.format = VIDEO_BUFFER_FORMAT_XRGB;
	config.aspect = VIDEO_ASPECT_AUTO;
	config.pitch = displayWidth * 4;
	videoConfigure(0, &config, nullptr, 0);
	waitIdle();
	gcmSetFlipMode(GCM_FLIP_VSYNC);
	displayPitch = displayWidth * 4;
	for (unsigned i = 0; i < 2; ++i) {
		displayBuffer[i] = rsxAlloc(64, displayPitch * displayHeight);
		rsxAddressToOffset(displayBuffer[i], &displayOffset[i]);
		gcmSetDisplayBuffer(u8(i), displayOffset[i], displayPitch, displayWidth, displayHeight);
	}
	displayDepthPitch = displayPitch;
	displayDepth = rsxAlloc(64, displayDepthPitch * displayHeight);
	rsxAddressToOffset(displayDepth, &displayDepthOffset);
	gcmResetFlipStatus();
	gcmSetVBlankHandler(onVblank);
	std::printf("c3dps3: display %ux%u (%s), LINEAR %u MiB\n", unsigned(displayWidth), unsigned(displayHeight),
		displayAspect < 1.5f ? "4:3" : "16:9", unsigned(LinearSize >> 20));
}
void waitVblank() {
	const uint32_t start = vblanks.load(std::memory_order_relaxed);
	const uint64_t deadline = nowUs() + 34000;
	while (vblanks.load(std::memory_order_relaxed) == start && nowUs() < deadline) usleep(200);
}
} // namespace c3dps3

extern "C" void* linearAlloc(size_t size) { c3dps3::initDisplay(); return linear.alloc(size, 128); }
extern "C" void* linearMemAlign(size_t size, size_t alignment) { c3dps3::initDisplay(); return linear.alloc(size, alignment); }
extern "C" void* linearRealloc(void* mem, size_t size) { c3dps3::initDisplay(); return linear.realloc(mem, size); }
extern "C" size_t linearGetSize(void* mem) { return mem ? linear.sizeOf(mem) : 0; }
// The GPU may still read a block freed during a frame: it is reused once the
// frame's fence has passed.
extern "C" void linearFree(void* mem) { if (mem) linear.release(mem); }
extern "C" u32 linearSpaceFree(void) {
	c3dps3::initDisplay();
	const size_t free = linear.freeBytes();
	// Headroom for allocations made between the game's checks.
	return u32(free > (4u << 20) ? free - (4u << 20) : 0);
}

// The 3DS flushes CPU writes before the GPU reads them. A flushed range inside
// a texture's CPU data (menu movie frames) means it must be converted again.
extern "C" Result GSPGPU_FlushDataCache(const void* adr, u32) {
	LightLock_Lock(&textureLock);
	if (GpuTexture* t = textureContaining(adr)) { t->dirty = true; t->prepared.clear(); }
	LightLock_Unlock(&textureLock);
	return 0;
}

// ---- citro3d: context ------------------------------------------------------------------
bool C3D_Init(size_t) {
	c3dps3::initDisplay();
	for (auto& e : state.env) C3D_TexEnvInit(&e);
	return true;
}
void C3D_Fini(void) {}
float C3D_GetCmdBufUsage(void) { return 0.f; }
void C3D_BindProgram(shaderProgram_s* program) {
	state.program = uint8_t(program && program->vertexShader ? dvleIndex(program->vertexShader->dvle) : ProgramScene);
}
void C3D_SetViewport(u32 x, u32 y, u32 w, u32 h) {
	state.viewport[0] = int(x); state.viewport[1] = int(y); state.viewport[2] = int(w); state.viewport[3] = int(h);
}
void C3D_SetScissor(GPU_SCISSORMODE mode, u32 left, u32 top, u32 right, u32 bottom) {
	state.scissor = mode != GPU_SCISSOR_DISABLE;
	state.scissorBox[0] = int(left); state.scissorBox[1] = int(top);
	state.scissorBox[2] = int(right) - int(left); state.scissorBox[3] = int(bottom) - int(top);
}
void C3D_DrawArrays(GPU_Primitive_t primitive, int first, int size) {
	if (!state.target || size <= 0) return;
	const uint64_t start = nowUs();
	prepareDraw(u32(first), u32(first + size));
	rsxDrawVertexArray(context, ::primitive(primitive), u32(first), u32(size));
	afterDraw();
	stats.drawUs += nowUs() - start;
}
void C3D_DrawElements(GPU_Primitive_t primitive, int count, int type, const void* indices) {
	if (!state.target || count <= 0) return;
	const uint64_t start = nowUs();
	// The RSX reads 16- or 32-bit indices: 8-bit lists are widened.
	const void* drawIndices = indices;
	uint32_t lo = ~0u, hi = 0;
	const bool skinned = state.program == ProgramScene && (C3D_BoolUnifs[0] & 8);
	if (type != C3D_UNSIGNED_SHORT) {
		auto* wide = reinterpret_cast<u16*>(scratchAlloc(uint32_t(count) * 2));
		for (int i = 0; i < count; ++i) wide[i] = static_cast<const u8*>(indices)[i];
		drawIndices = wide;
	}
	if (skinned)
		for (int i = 0; i < count; ++i) {
			const uint32_t v = static_cast<const u16*>(drawIndices)[i];
			lo = std::min(lo, v); hi = std::max(hi, v);
		}
	if (lo > hi) lo = hi = 0;
	prepareDraw(lo, hi + 1);
	rsxDrawIndexArray(context, u8(::primitive(primitive)), ioOffset(drawIndices), u32(count), GCM_INDEX_TYPE_16B, GCM_LOCATION_CELL);
	afterDraw();
	stats.drawUs += nowUs() - start;
}

// ---- Texture combiner -------------------------------------------------------------------
C3D_TexEnv* C3D_GetTexEnv(int id) { return &state.env[id]; }
void C3D_SetTexEnv(int id, C3D_TexEnv* env) { state.env[id] = *env; }
void C3D_DirtyTexEnv(C3D_TexEnv*) {}
void C3D_TexEnvBufUpdate(int mode, int mask) {
	u32 val = state.envBuf;
	mask &= 0xF;
	if (mode & C3D_RGB) { val &= ~(0xFu << 8); val |= u32(mask) << 8; }
	if (mode & C3D_Alpha) { val &= ~(0xFu << 12); val |= u32(mask) << 12; }
	state.envBuf = val;
}
void C3D_TexEnvBufColor(u32 color) { state.envBufColor = color; }

// ---- Effects ----------------------------------------------------------------------------
void C3D_DepthMap(bool, float, float) {} // the game keeps citro3d's default map
void C3D_CullFace(GPU_CULLMODE mode) {
	if (cullSwap && mode != GPU_CULL_NONE) mode = mode == GPU_CULL_FRONT_CCW ? GPU_CULL_BACK_CCW : GPU_CULL_FRONT_CCW;
	state.cull = u8(mode);
}
extern "C" void c3dport_SetCullSwap(bool swap) { cullSwap = swap; }
extern "C" void c3dport_SetResolutionDivisor(float divisor) { Divisor = std::clamp(divisor, 1.f, 4.f); }
void C3D_StencilTest(bool enable, GPU_TESTFUNC function, int ref, int inputMask, int writeMask) {
	state.stencilEnable = enable; state.stencilFunc = u8(function);
	state.stencilRef = u8(ref); state.stencilInMask = u8(inputMask); state.stencilWriteMask = u8(writeMask);
}
void C3D_StencilOp(GPU_STENCILOP sfail, GPU_STENCILOP dfail, GPU_STENCILOP pass) {
	state.stencilOp[0] = u8(sfail); state.stencilOp[1] = u8(dfail); state.stencilOp[2] = u8(pass);
}
void C3D_BlendingColor(u32 color) { state.blendColor = color; }
void C3D_EarlyDepthTest(bool, GPU_EARLYDEPTHFUNC, u32) {}
void C3D_DepthTest(bool enable, GPU_TESTFUNC function, GPU_WRITEMASK writemask) {
	state.depthEnable = enable; state.depthFunc = u8(function); state.writeMask = u8(writemask);
}
void C3D_AlphaTest(bool enable, GPU_TESTFUNC function, int ref) {
	state.alphaEnable = enable; state.alphaFunc = u8(function); state.alphaRef = u8(ref);
}
void C3D_AlphaBlend(GPU_BLENDEQUATION colorEq, GPU_BLENDEQUATION alphaEq, GPU_BLENDFACTOR srcClr, GPU_BLENDFACTOR dstClr, GPU_BLENDFACTOR srcAlpha, GPU_BLENDFACTOR dstAlpha) {
	state.blendEq[0] = u8(colorEq); state.blendEq[1] = u8(alphaEq);
	state.blendFactor[0] = u8(srcClr); state.blendFactor[1] = u8(dstClr);
	state.blendFactor[2] = u8(srcAlpha); state.blendFactor[3] = u8(dstAlpha);
}
void C3D_ColorLogicOp(GPU_LOGICOP) {}
void C3D_FragOpMode(GPU_FRAGOPMODE) {}
void C3D_FragOpShadow(float, float) {}

// ---- Attributes and buffers ---------------------------------------------------------------
// Loader n: format | (components - 1) << 2 in nibble n of flags; its input
// register in nibble n of permutation (citro3d's own encoding).
void AttrInfo_Init(C3D_AttrInfo* info) { std::memset(info, 0, sizeof *info); }
int AttrInfo_AddLoader(C3D_AttrInfo* info, int regId, GPU_FORMATS format, int count) {
	if (info->attrCount == 12 || regId < 0 || regId > 15 || count < 1 || count > 4) return -1;
	const int id = info->attrCount++;
	const u32 nibble = u32(format) | u32(count - 1) << 2;
	if (id < 8) info->flags[0] |= nibble << (4 * id);
	else info->flags[1] |= nibble << (4 * (id - 8));
	info->permutation |= u64(regId) << (4 * id);
	return id;
}
int AttrInfo_AddFixed(C3D_AttrInfo*, int) { return -1; }
C3D_AttrInfo* C3D_GetAttrInfo(void) { return &state.attr; }
void C3D_SetAttrInfo(C3D_AttrInfo* info) { state.attr = *info; }
void BufInfo_Init(C3D_BufInfo* info) { std::memset(info, 0, sizeof *info); }
// Buffers are addressed by their (32-bit) effective address, as the Vita's.
int BufInfo_Add(C3D_BufInfo* info, const void* data, ptrdiff_t stride, int attribCount, u64 permutation) {
	if (info->bufCount == 12) return -1;
	const int id = info->bufCount++;
	C3D_BufCfg& cfg = info->buffers[id];
	cfg.offset = u32(uintptr_t(data));
	cfg.flags[0] = u32(permutation);
	cfg.flags[1] = u32((permutation >> 32) & 0xFFFF) | u32(stride & 0xFFF) << 16 | u32(attribCount) << 28;
	return id;
}
C3D_BufInfo* C3D_GetBufInfo(void) { return &state.buf; }
void C3D_SetBufInfo(C3D_BufInfo* info) { state.buf = *info; }

// ---- Textures ------------------------------------------------------------------------------
bool C3D_TexInitWithParams(C3D_Tex* tex, C3D_TexCube*, C3D_TexInitParams p) {
	if (p.type != GPU_TEX_2D && p.type != GPU_TEX_PROJECTION) return false;
	size_t total = 0;
	for (int level = 0; level <= p.maxLevel; ++level)
		total += levelBytes(p.format, std::max(8u, unsigned(p.width) >> level), std::max(8u, unsigned(p.height) >> level));
	void* data = memalign(0x80, total);
	if (!data) return false;
	std::memset(tex, 0, sizeof *tex);
	tex->data = data;
	tex->fmt = p.format;
	tex->size = levelBytes(p.format, p.width, p.height);
	tex->width = p.width;
	tex->height = p.height;
	tex->param = GPU_TEXTURE_MODE(p.type);
	if (p.format == GPU_ETC1) tex->param |= GPU_TEXTURE_ETC1_PARAM;
	tex->maxLevel = p.maxLevel;
	auto* record = new GpuTexture;
	record->bytes = total;
	LightLock_Lock(&textureLock);
	textures[uintptr_t(data)] = record;
	LightLock_Unlock(&textureLock);
	return true;
}
void C3D_TexLoadImage(C3D_Tex* tex, const void* data, GPU_TEXFACE, int level) {
	u32 size = 0;
	void* out = C3D_Tex2DGetImagePtr(tex, level, &size);
	std::memcpy(out, data, size);
	C3D_TexFlush(tex);
}
void C3D_TexGenerateMipmap(C3D_Tex*, GPU_TEXFACE) {}
void C3D_TexBind(int unitId, C3D_Tex* tex) { if (unitId >= 0 && unitId < 3) state.tex[unitId] = tex; }
void C3D_TexFlush(C3D_Tex* tex) {
	if (!tex || !tex->data) return;
	LightLock_Lock(&textureLock);
	GpuTexture* t = findTexture(tex->data);
	const bool renderTarget = t && t->renderTarget;
	LightLock_Unlock(&textureLock);
	if (!t || renderTarget) return;
	// Convert here, on the thread that wrote the data (menu and race assets
	// load on the asset loader's thread), so drawing only copies.
	std::vector<Converted> levels = convertTexture(*tex);
	LightLock_Lock(&textureLock);
	if ((t = findTexture(tex->data))) { t->prepared.swap(levels); t->dirty = true; }
	LightLock_Unlock(&textureLock);
}
void C3D_TexDelete(C3D_Tex* tex) {
	if (!tex || !tex->data) return;
	LightLock_Lock(&textureLock);
	auto it = textures.find(uintptr_t(tex->data));
	if (it != textures.end()) {
		if (!it->second->renderTarget) freeVramLater(it->second->vram);
		for (auto& b : boundUnit) if (b.texture == it->second) b = {};
		delete it->second;
		textures.erase(it);
	}
	LightLock_Unlock(&textureLock);
	for (auto& bound : state.tex) if (bound == tex) bound = nullptr;
	std::free(tex->data);
	tex->data = nullptr;
}
void C3D_TexShadowParams(bool, float) {}

// ---- Render targets ------------------------------------------------------------------------
extern "C" C3D_RenderTarget* c3dvita_RenderTargetCreate(int width, int height, GPU_COLORBUF colorFmt, int depthFmt) {
	auto* t = new Target{};
	t->width = scaled(width); t->height = scaled(height);
	t->hasDepth = depthFmt >= 0;
	if (!createTargetStorage(*t, nullptr)) { std::printf("c3dps3: out of RSX memory for a %dx%d target\n", t->width, t->height); delete t; return nullptr; }
	C3D_FrameBuf& fb = t->c3d.frameBuf;
	fb.colorBuf = t; // identifies the target (C3D_SyncDisplayTransfer)
	fb.depthBuf = depthFmt >= 0 ? t : nullptr;
	fb.width = u16(width); fb.height = u16(height);
	fb.colorFmt = colorFmt;
	fb.depthFmt = depthFmt >= 0 ? GPU_DEPTHBUF(depthFmt) : GPU_RB_DEPTH24;
	fb.colorMask = 0xF; fb.depthMask = depthFmt >= 0 ? 0x3 : 0;
	t->c3d.ownsColor = true; t->c3d.ownsDepth = depthFmt >= 0;
	targets.push_back(t);
	invalidateApplied();
	return &t->c3d;
}
extern "C" C3D_RenderTarget* c3dvita_RenderTargetCreateFromTex(C3D_Tex* tex, GPU_TEXFACE, int, int depthFmt) {
	LightLock_Lock(&textureLock);
	GpuTexture* record = tex && tex->data ? findTexture(tex->data) : nullptr;
	LightLock_Unlock(&textureLock);
	if (!record) return nullptr;
	auto* t = new Target{};
	t->width = scaled(tex->width); t->height = scaled(tex->height);
	t->hasDepth = depthFmt >= 0;
	t->ownsColor = false;
	if (!createTargetStorage(*t, nullptr)) { std::printf("c3dps3: out of RSX memory for a %dx%d target\n", t->width, t->height); delete t; return nullptr; }
	// The texture is sampled through its record: give it the target's storage.
	LightLock_Lock(&textureLock);
	freeVramLater(record->vram);
	record->vram = t->color; record->offset = t->colorOffset; record->pitch = t->colorPitch;
	record->width = uint16_t(t->width); record->height = uint16_t(t->height);
	record->format = GCM_TEXTURE_FORMAT_A8R8G8B8 | GCM_TEXTURE_FORMAT_LIN;
	record->levels = 1; record->hasAlpha = true;
	record->renderTarget = true; record->dirty = false; record->prepared.clear();
	LightLock_Unlock(&textureLock);
	C3D_FrameBuf& fb = t->c3d.frameBuf;
	fb.colorBuf = t; fb.depthBuf = depthFmt >= 0 ? t : nullptr;
	fb.width = tex->width; fb.height = tex->height;
	fb.colorFmt = GPU_RB_RGBA8; fb.depthFmt = depthFmt >= 0 ? GPU_DEPTHBUF(depthFmt) : GPU_RB_DEPTH24;
	fb.colorMask = 0xF; fb.depthMask = depthFmt >= 0 ? 0x3 : 0;
	targets.push_back(t);
	invalidateApplied();
	return &t->c3d;
}
void C3D_RenderTargetDelete(C3D_RenderTarget* target) {
	if (!target) return;
	Target* t = targetOf(target);
	if (topOutput == t) topOutput = nullptr;
	if (state.target == t) state.target = nullptr;
	if (applied.target == t) applied.valid = false;
	targets.erase(std::remove(targets.begin(), targets.end(), t), targets.end());
	// The colour of a texture-backed target is the texture's storage, released with it.
	if (t->ownsColor) freeVramLater(t->color);
	freeVramLater(t->depth);
	delete t;
}
void C3D_RenderTargetSetOutput(C3D_RenderTarget* target, gfxScreen_t screen, gfx3dSide_t side, u32) {
	// Only the upper screen's left eye reaches the display.
	if (screen != GFX_TOP || side != GFX_LEFT) return;
	topOutput = target ? targetOf(target) : nullptr;
}
void C3D_FrameBufClear(C3D_FrameBuf* fb, C3D_ClearBits clearBits, u32 clearColor, u32 clearDepth) {
	// Recorded, not run: see flushClear. A later clear of the same target
	// replaces the values it sets.
	Target* t = targetOfFrameBuf(fb);
	if (clearBits & C3D_CLEAR_COLOR) {
		// RGBA8 targets clear to 0xRRGGBBAA.
		t->clearArgb = (clearColor & 0xFF) << 24 | clearColor >> 8;
		t->clearColor = true;
	}
	if ((clearBits & C3D_CLEAR_DEPTH) && t->hasDepth) {
		// The depth buffer holds the PICA's own depth (24 bits) and stencil.
		t->clearDepthStencil = (clearDepth & 0xFFFFFF) << 8 | clearDepth >> 24;
		t->clearDepth = true;
	}
}
void C3D_FrameBufTransfer(C3D_FrameBuf*, gfxScreen_t, gfx3dSide_t, u32) {}

// ---- Frames ---------------------------------------------------------------------------------
float C3D_FrameRate(float fps) { return fps; }
void C3D_FrameSync(void) { waitIdle(); }
u32 C3D_FrameCounter(int) { return vblanks.load(std::memory_order_relaxed); }
bool C3D_FrameBegin(u8) {
	// citro3d waits here for the previous frame's GPU work: the game rewrites
	// LINEAR data (sprites, effects, skinned vertices) after this.
	const uint64_t start = nowUs();
	waitFence(submittedFence.load());
	stats.beginWaitUs += nowUs() - start;
	collectVram();
	scratchUsed = 0;
	inFrame.store(true);
	rsxInvalidateTextureCache(context, GCM_INVALIDATE_TEXTURE);
	applied.constantRow = ~0u;
	checkCommands("frame begin");
	stats.frameStartUs = nowUs();
	for (Target* t : targets) t->c3d.used = false;
	return true;
}
bool C3D_FrameDrawOn(C3D_RenderTarget* target) {
	if (!target) return false;
	Target* t = targetOf(target);
	t->c3d.used = true;
	state.target = t;
	C3D_SetViewport(0, 0, target->frameBuf.width, target->frameBuf.height);
	return true;
}
void C3D_FrameSplit(u8) { rsxFlushBuffer(context); }
void C3D_FrameEnd(u8) {
	if (topOutput && topOutput->c3d.used) {
		if (stats.frameStartUs) stats.workUs += nowUs() - stats.frameStartUs;
		present(topOutput);
		const uint64_t now = nowUs();
		if (stats.lastPresentUs) {
			stats.frameUs += now - stats.lastPresentUs;
			const uint64_t frame = now - stats.lastPresentUs;
			if (frame > 70000)
				std::printf("c3dps3: slow frame %.1f ms: %u texture uploads (%u converted while drawing, %.1f ms), %u shader programs (%.1f ms)\n",
					double(frame) / 1e3, uploadStats.uploads, uploadStats.converted, double(uploadStats.us) / 1e3,
					c3dps3::programsCreated, double(c3dps3::programUs) / 1e3);
		}
		uploadStats = {};
		c3dps3::programsCreated = 0;
		c3dps3::programUs = 0;
		stats.lastPresentUs = now;
		if (++stats.frames == 300) {
			std::printf("c3dps3: %.1f fps, %u draws/frame (%u skinned), wait %.2f ms, draw thread %.2f ms, in draw calls %.2f ms\n",
				stats.frameUs ? 299e6 / double(stats.frameUs) : 0.0, stats.draws / 300, stats.skinned / 300,
				double(stats.beginWaitUs) / 300e3, double(stats.workUs) / 300e3, double(stats.drawUs) / 300e3);
			const uint64_t keep = stats.lastPresentUs;
			stats = {};
			stats.lastPresentUs = keep;
		}
	}
	checkCommands("frame end");
	writeFence(true);
	inFrame.store(false);
	++frameNumber;
}
void C3D_FrameEndHook(void (*)(void*), void*) {}
float C3D_GetDrawingTime(void) { return 0.f; }
float C3D_GetProcessingTime(void) { return 0.f; }

// Readback of a target into RGBA8 rows from the portrait top (QA captures).
void C3D_SyncDisplayTransfer(u32* inadr, u32, u32* outadr, u32, u32) {
	Target* t = nullptr;
	for (Target* candidate : targets) if (candidate->c3d.frameBuf.colorBuf == inadr) t = candidate;
	if (!t) return;
	flushClear(t);
	waitIdle();
	const int w = t->c3d.frameBuf.width, h = t->c3d.frameBuf.height;
	const auto* pixels = static_cast<const uint8_t*>(t->color);
	auto* out = reinterpret_cast<uint8_t*>(outadr);
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			const uint8_t* p = pixels + size_t(y * Scale) * t->colorPitch + size_t(x * Scale) * 4; // nearest sample, A R G B
			uint8_t* o = out + (size_t(h - 1 - y) * w + x) * 4;
			o[0] = p[0]; o[1] = p[3]; o[2] = p[2]; o[3] = p[1];
		}
}
void C3D_SyncTextureCopy(u32* inadr, u32, u32* outadr, u32, u32 size, u32) {
	std::memcpy(outadr, inadr, size);
	LightLock_Lock(&textureLock);
	if (GpuTexture* t = textureContaining(outadr)) { t->dirty = true; t->prepared.clear(); }
	LightLock_Unlock(&textureLock);
}
void C3D_SyncMemoryFill(u32*, u32, u32*, u16, u32*, u32, u32*, u16) {}
