/*
**	ZeroHourReloaded — Cross-Platform SDL3 Window & Event Host
*/

#include "SDLDevice/SDL3Platform.h"
#include "SDLDevice/SDL3Keyboard.h"

#include "Common/Debug.h"
#include "Common/GameEngine.h"
#include "GameClient/GameClient.h"
#include "GameClient/Mouse.h"

#ifdef _WIN32
#include <windows.h>
#include "Win32Device/GameClient/Win32Mouse.h"
extern Win32Mouse* TheWin32Mouse;
extern DWORD TheMessageTime;
#endif

#include <SDL3/SDL.h>

namespace
{
	SDL_Window* g_sdlWindow = nullptr;
	Bool g_sdlInitialized = FALSE;
	Bool g_focused = TRUE;
	Bool g_minimized = FALSE;
}

namespace ZhrSDL3Platform
{

Bool createWindow(const char* title, Int width, Int height, Bool windowed, void** outNativeHandle)
{
	if (outNativeHandle)
		*outNativeHandle = nullptr;

	if (!g_sdlInitialized)
	{
		if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
		{
			DEBUG_LOG(("ZhrSDL3Platform::createWindow - SDL_Init failed: %s", SDL_GetError()));
			return FALSE;
		}
		g_sdlInitialized = TRUE;
	}

	SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
	if (!windowed)
	{
		flags |= SDL_WINDOW_FULLSCREEN;
	}
	else
	{
		flags |= SDL_WINDOW_RESIZABLE;
	}

	const Int w = width > 0 ? width : 1280;
	const Int h = height > 0 ? height : 720;
	g_sdlWindow = SDL_CreateWindow(title ? title : "ZeroHourReloaded", w, h, flags);
	if (!g_sdlWindow)
	{
		DEBUG_LOG(("ZhrSDL3Platform::createWindow - SDL_CreateWindow failed: %s", SDL_GetError()));
		return FALSE;
	}

	SDL_SetWindowPosition(g_sdlWindow, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
	SDL_ShowWindow(g_sdlWindow);
	SDL_RaiseWindow(g_sdlWindow);
	g_focused = TRUE;
	g_minimized = FALSE;

#ifdef _WIN32
	SDL_PropertiesID props = SDL_GetWindowProperties(g_sdlWindow);
	void* hwnd = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
	if (outNativeHandle)
		*outNativeHandle = hwnd;
#endif

	return TRUE;
}

void destroyWindow()
{
	if (g_sdlWindow)
	{
		SDL_DestroyWindow(g_sdlWindow);
		g_sdlWindow = nullptr;
	}
	if (g_sdlInitialized)
	{
		SDL_Quit();
		g_sdlInitialized = FALSE;
	}
}

SDL_Window* getWindow()
{
	return g_sdlWindow;
}

Bool isFocused()
{
	return g_focused;
}

Bool isMinimized()
{
	return g_minimized;
}

void pumpEvents()
{
	if (!g_sdlInitialized)
		return;

	SDL_Event ev{};
	while (SDL_PollEvent(&ev))
	{
		const Uint32 nowMs = static_cast<Uint32>(SDL_GetTicks());
#ifdef _WIN32
		TheMessageTime = nowMs;
#endif

		switch (ev.type)
		{
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			if (TheGameEngine)
			{
				TheGameEngine->setQuitting(TRUE);
			}
			break;

		case SDL_EVENT_WINDOW_FOCUS_GAINED:
			g_focused = TRUE;
			if (TheGameEngine)
				TheGameEngine->setIsActive(TRUE);
			if (TheMouse)
				TheMouse->regainFocus();
			break;

		case SDL_EVENT_WINDOW_FOCUS_LOST:
			g_focused = FALSE;
			if (TheGameEngine)
				TheGameEngine->setIsActive(FALSE);
			if (TheMouse)
				TheMouse->loseFocus();
			break;

		case SDL_EVENT_WINDOW_MINIMIZED:
			g_minimized = TRUE;
			g_focused = FALSE;
			if (TheGameEngine)
				TheGameEngine->setIsActive(FALSE);
			break;

		case SDL_EVENT_WINDOW_RESTORED:
			g_minimized = FALSE;
			g_focused = TRUE;
			if (TheGameEngine)
				TheGameEngine->setIsActive(TRUE);
			break;

		case SDL_EVENT_KEY_DOWN:
			SDL3Keyboard::pushSDLEvent(static_cast<uint32_t>(ev.key.scancode), TRUE, ev.key.repeat ? TRUE : FALSE);
			break;

		case SDL_EVENT_KEY_UP:
			SDL3Keyboard::pushSDLEvent(static_cast<uint32_t>(ev.key.scancode), FALSE, FALSE);
			break;

#ifdef _WIN32
		case SDL_EVENT_MOUSE_MOTION:
			if (TheWin32Mouse)
			{
				const int mx = static_cast<int>(ev.motion.x);
				const int my = static_cast<int>(ev.motion.y);
				TheWin32Mouse->addWin32Event(WM_MOUSEMOVE, 0, MAKELPARAM(mx, my), nowMs);
			}
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
			if (TheWin32Mouse)
			{
				const int mx = static_cast<int>(ev.button.x);
				const int my = static_cast<int>(ev.button.y);
				const LPARAM lp = MAKELPARAM(mx, my);
				const bool dbl = (ev.button.clicks >= 2);
				UINT msg = 0;
				if (ev.button.button == SDL_BUTTON_LEFT)
					msg = dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN;
				else if (ev.button.button == SDL_BUTTON_RIGHT)
					msg = dbl ? WM_RBUTTONDBLCLK : WM_RBUTTONDOWN;
				else if (ev.button.button == SDL_BUTTON_MIDDLE)
					msg = dbl ? WM_MBUTTONDBLCLK : WM_MBUTTONDOWN;
				if (msg != 0)
					TheWin32Mouse->addWin32Event(msg, 0, lp, nowMs);
			}
			break;

		case SDL_EVENT_MOUSE_BUTTON_UP:
			if (TheWin32Mouse)
			{
				const int mx = static_cast<int>(ev.button.x);
				const int my = static_cast<int>(ev.button.y);
				const LPARAM lp = MAKELPARAM(mx, my);
				UINT msg = 0;
				if (ev.button.button == SDL_BUTTON_LEFT)
					msg = WM_LBUTTONUP;
				else if (ev.button.button == SDL_BUTTON_RIGHT)
					msg = WM_RBUTTONUP;
				else if (ev.button.button == SDL_BUTTON_MIDDLE)
					msg = WM_MBUTTONUP;
				if (msg != 0)
					TheWin32Mouse->addWin32Event(msg, 0, lp, nowMs);
			}
			break;

		case SDL_EVENT_MOUSE_WHEEL:
			if (TheWin32Mouse)
			{
				const short delta = static_cast<short>(ev.wheel.y * 120.0f);
				const int mx = static_cast<int>(ev.wheel.mouse_x);
				const int my = static_cast<int>(ev.wheel.mouse_y);
				TheWin32Mouse->addWin32Event(WM_MOUSEWHEEL, MAKEWPARAM(0, delta), MAKELPARAM(mx, my), nowMs);
			}
			break;
#endif

		default:
			break;
		}
	}

#ifdef _WIN32
	TheMessageTime = 0;
#endif
}

} // namespace ZhrSDL3Platform
