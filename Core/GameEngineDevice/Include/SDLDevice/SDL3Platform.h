/*
**	ZeroHourReloaded — Cross-Platform SDL3 Window & Event Host
**
**	Replaces Win32 CreateWindow/WndProc/DirectInput lifecycle with SDL3 while
**	exposing the native OS window handle (HWND / X11 / Wayland / Cocoa) to
**	`zhr-vulkan` and forwarding input events to Zero Hour's Keyboard & Mouse.
*/

#pragma once

#include "Lib/BaseType.h"

struct SDL_Window;

namespace ZhrSDL3Platform
{
	/// Initializes SDL3 video/events and creates the main Vulkan-capable window.
	/// On Windows, writes the native HWND into `*outNativeHandle` (if non-null).
	Bool createWindow(const char* title, Int width, Int height, Bool windowed, void** outNativeHandle);

	/// Destroys the SDL3 window and shuts down SDL3 cleanly.
	void destroyWindow();

	/// Returns the active SDL_Window* (or nullptr if headless/uninitialized).
	SDL_Window* getWindow();

	/// Pumps all pending SDL3 events and dispatches quit, focus, keyboard, and mouse events.
	void pumpEvents();

	/// Returns whether the SDL3 window currently has OS input focus.
	Bool isFocused();

	/// Returns whether the SDL3 window is currently minimized.
	Bool isMinimized();

	/// Returns the OS display DPI scale factor for the active window (e.g. 1.0, 1.25, 1.5, 2.0).
	Real getDisplayDpiScale();
}
