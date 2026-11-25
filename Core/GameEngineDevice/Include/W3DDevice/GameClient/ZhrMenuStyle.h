/*
**  ZeroHourReloaded — Modern Tactical Shell Menu & UI Styling
**  Copyright (C) 2026 ZeroHourReloaded Contributors (GPL-3.0-or-later)
*/

#pragma once

#include "Lib/BaseType.h"

class GameWindow;
class WinInstanceData;

namespace ZhrMenuStyle
{
/// Returns true when the modern remastered shell UI is active (Remastered or SplitScreen mode while in shell).
Bool isActive();

/// Returns true when 50/50 Split-Screen A/B mode is active (left = classic 2003, right = modern remastered).
Bool isSplitScreen();

/// Returns true if `window` is the legacy full-screen stretched blue ruler / grid overlay.
Bool isShellRulerWindow(GameWindow *window, WinInstanceData *instData);

/// Returns true if `window` is a shell panel container that should receive frosted-glass panel styling.
Bool shouldStyleShellPanel(GameWindow *window, WinInstanceData *instData);

/// Returns true if `window` is a shell push button that should receive modern tactical button styling.
Bool shouldStyleShellButton(GameWindow *window, WinInstanceData *instData);

/// Draws the clean widescreen shell vignette, razor-thin tactical perimeter frame, and bottom-left engine status pill.
void drawShellBackdropFrame(Bool rightHalfOnly = FALSE);

/// Draws a real-time Vulkan frosted-glass tactical panel with drop shadow, metallic border, top glass gleam, and corner ticks.
void drawGlassPanel(Int x, Int y, Int width, Int height, Bool accentHighlight = FALSE);

/// Draws a crisp vector metallic border (modern replacement for tiled 20px 2003 pixel-art borders in shell menus).
void drawPanelBorder(Int x, Int y, Int width, Int height);

/// Draws a modern tactical shell push button (dark-slate metallic gradient, luminous left accent rail, hover glow, crisp text).
void drawTacticalButton(GameWindow *window, WinInstanceData *instData);

/// Releases cached DisplayString resources on display reset/shutdown.
void reset();
}
