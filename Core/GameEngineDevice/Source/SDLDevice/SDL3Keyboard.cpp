/*
**	ZeroHourReloaded — Cross-Platform SDL3 Keyboard (replaces DirectInput8)
*/

#include "SDLDevice/SDL3Keyboard.h"
#include "zhr_ffi.h"

#include <SDL3/SDL.h>

#include <mutex>

namespace
{
	struct QueuedKeyEvent
	{
		UnsignedByte zhKey;
		UnsignedShort state;
	};

	constexpr size_t kKeyQueueCap = 512;
	std::mutex g_keyMutex;
	QueuedKeyEvent g_keyQueue[kKeyQueueCap]{};
	size_t g_keyHead = 0;
	size_t g_keyTail = 0;
}

SDL3Keyboard::SDL3Keyboard() = default;

SDL3Keyboard::~SDL3Keyboard() = default;

void SDL3Keyboard::init()
{
	{
		std::lock_guard<std::mutex> lock(g_keyMutex);
		g_keyHead = 0;
		g_keyTail = 0;
	}
	Keyboard::init();
}

void SDL3Keyboard::reset()
{
	{
		std::lock_guard<std::mutex> lock(g_keyMutex);
		g_keyHead = 0;
		g_keyTail = 0;
	}
	Keyboard::reset();
}

void SDL3Keyboard::update()
{
	Keyboard::update();
}

Bool SDL3Keyboard::getCapsState()
{
	return (SDL_GetModState() & SDL_KMOD_CAPS) != 0 ? TRUE : FALSE;
}

void SDL3Keyboard::pushSDLEvent(uint32_t sdlScancode, Bool isDown, Bool isRepeat)
{
	// Ignore OS autorepeat events; Zero Hour's Keyboard base class generates its own deterministic repeat.
	if (isRepeat)
		return;

	const UnsignedByte zhKey = zhr_platform_sdl_scancode_to_zh_key(sdlScancode);
	if (zhKey == KEY_NONE)
		return;

	std::lock_guard<std::mutex> lock(g_keyMutex);
	const size_t nextTail = (g_keyTail + 1) % kKeyQueueCap;
	if (nextTail == g_keyHead)
		return; // Queue full

	g_keyQueue[g_keyTail].zhKey = zhKey;
	g_keyQueue[g_keyTail].state = isDown ? KEY_STATE_DOWN : KEY_STATE_UP;
	g_keyTail = nextTail;
}

void SDL3Keyboard::getKey(KeyboardIO* key)
{
	if (!key)
		return;

	key->key = KEY_NONE;
	key->status = KeyboardIO::STATUS_UNUSED;
	key->state = KEY_STATE_NONE;

	std::lock_guard<std::mutex> lock(g_keyMutex);
	if (g_keyHead == g_keyTail)
		return;

	const QueuedKeyEvent ev = g_keyQueue[g_keyHead];
	g_keyHead = (g_keyHead + 1) % kKeyQueueCap;

	key->key = ev.zhKey;
	key->state = ev.state;
	key->status = KeyboardIO::STATUS_UNUSED;
}
