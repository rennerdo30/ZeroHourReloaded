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

/// Returns true when `window` belongs to a styled shell or modal menu (works both in shell and in-game Options).
Bool isStyledForWindow(GameWindow *window);

/// Returns true when 50/50 Split-Screen A/B mode is active (left = classic 2003, right = modern remastered).
Bool isSplitScreen();

/// Returns true if `window` is the legacy full-screen stretched blue ruler / grid overlay.
Bool isShellRulerWindow(GameWindow *window, WinInstanceData *instData);

/// Returns true if `window` is a thin horizontal/vertical divider line window (never styled as a glass panel).
Bool isDividerLineWindow(GameWindow *window, WinInstanceData *instData);

/// Returns true if `window` is a recessed section sub-card (`ZhrSubPanel*`) inside a dialog.
Bool isSubPanelWindow(GameWindow *window, WinInstanceData *instData);

/// Returns true if `window` is a shell panel container that should receive frosted-glass panel styling.
Bool shouldStyleShellPanel(GameWindow *window, WinInstanceData *instData);

/// Returns true if `window` is a shell push button (including tabs, combo arrows, slider thumbs) that should receive modern tactical styling.
Bool shouldStyleShellButton(GameWindow *window, WinInstanceData *instData);

/// Returns true if `window` is a shell gadget (checkbox, slider, combo, text entry, listbox) that should receive modern vector styling.
Bool shouldStyleShellGadget(GameWindow *window);

/// Draws the clean widescreen shell vignette, razor-thin tactical perimeter frame, and bottom-left engine status pill.
void drawShellBackdropFrame(Bool rightHalfOnly = FALSE);

/// Draws a crisp 1px tactical cyan hairline divider (replaces legacy 2003 Line windows cleanly at any resolution).
void drawDividerLine(Int x, Int y, Int width, Int height);

/// Draws a recessed translucent slate section card for grouping settings cleanly inside dialogs.
void drawSubPanel(Int x, Int y, Int width, Int height);

/// Draws a real-time Vulkan frosted-glass tactical panel with drop shadow, metallic border, top glass gleam, and corner ticks.
void drawGlassPanel(Int x, Int y, Int width, Int height, Bool accentHighlight = FALSE);

/// Draws a crisp vector metallic border (modern replacement for tiled 20px 2003 pixel-art borders in shell menus).
void drawPanelBorder(Int x, Int y, Int width, Int height);

/// Draws a modern tactical shell push button, tab button, or combo dropdown chevron (and suppresses legacy slider thumb bitmaps).
void drawTacticalButton(GameWindow *window, WinInstanceData *instData);

/// Draws a modern vector checkbox with crisp checkmark and hover row highlight.
void drawTacticalCheckBox(GameWindow *window, WinInstanceData *instData);

/// Draws a modern vector horizontal slider with cyan gradient progress bar and glowing pill thumb.
void drawTacticalSlider(GameWindow *window, WinInstanceData *instData);

/// Draws the modern vector backdrop and focus/hover border for a TextEntry gadget.
void drawTacticalTextEntryFrame(GameWindow *window, WinInstanceData *instData);

/// Draws the modern vector frame for a ComboBox container.
void drawTacticalComboBox(GameWindow *window, WinInstanceData *instData);

/// Draws the modern frosted-glass backdrop and border for a ListBox / ComboBox popup list.
void drawTacticalListBoxFrame(Int x, Int y, Int width, Int height, Bool hilited);

/// Draws the modern cyan selection bar inside a ListBox / ComboBox popup list.
void drawTacticalListSelectionBar(Int x0, Int y0, Int x1, Int y1);

/// Releases cached DisplayString resources on display reset/shutdown.
void reset();
}
