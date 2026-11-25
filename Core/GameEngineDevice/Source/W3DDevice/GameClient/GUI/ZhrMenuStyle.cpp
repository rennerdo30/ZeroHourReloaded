/*
**  ZeroHourReloaded — Modern Tactical Shell Menu & UI Styling
**  Copyright (C) 2026 ZeroHourReloaded Contributors (GPL-3.0-or-later)
*/

#include "W3DDevice/GameClient/ZhrMenuStyle.h"

#include <cstring>
#include <cctype>

#include "Common/GlobalData.h"
#include "GameClient/Color.h"
#include "GameClient/Display.h"
#include "GameClient/DisplayString.h"
#include "GameClient/DisplayStringManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GameClient.h"
#include "GameClient/GameFont.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowGlobal.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Image.h"
#include "GameClient/Shell.h"
#include "WW3D2/render2d.h"

#ifdef ZHR_VULKAN_BACKEND
#include <zhr_ffi.h>
#endif

namespace ZhrMenuStyle
{

namespace
{
DisplayString *s_statusBadgeText = nullptr;
UnsignedInt s_cachedGfxMode = 0xFFFFFFFFu;
UnsignedInt s_lastBackdropFrame = 0xFFFFFFFFu;

bool containsCaseInsensitive(const char *haystack, const char *needle)
{
	if (!haystack || !needle || !*needle)
		return false;
	const size_t nlen = std::strlen(needle);
	for (const char *p = haystack; *p; ++p)
	{
		size_t i = 0;
		while (i < nlen && p[i] &&
			   std::tolower(static_cast<unsigned char>(p[i])) ==
				   std::tolower(static_cast<unsigned char>(needle[i])))
		{
			++i;
		}
		if (i == nlen)
			return true;
	}
	return false;
}

UnsignedInt currentGraphicsPacked()
{
#ifdef ZHR_VULKAN_BACKEND
	return zhr_vk_get_graphics_mode();
#else
	return 0;
#endif
}

void setupUntextured2D(Render2DClass &r2d)
{
	r2d.Set_Coordinate_Range(Render2DClass::Get_Screen_Resolution());
	r2d.Enable_Texturing(false);
	r2d.Enable_Alpha(true);
}

void drawFrostedQuad(const RectClass &rect, unsigned long topColor, unsigned long bottomColor)
{
	if (TheDisplay)
		TheDisplay->flush();

	Render2DClass glass;
	setupUntextured2D(glass);
	glass.Add_Quad_VGradient(rect, topColor, bottomColor);

#ifdef ZHR_VULKAN_BACKEND
	zhr_vk_set_material_hint(2);
#endif
	glass.Render();
#ifdef ZHR_VULKAN_BACKEND
	zhr_vk_set_material_hint(0);
#endif
}

void addCornerBrackets(Render2DClass &r2d, float x0, float y0, float x1, float y1, float len, float thick, unsigned long color)
{
	// Top-left
	r2d.Add_Quad(RectClass(x0, y0, x0 + len, y0 + thick), color);
	r2d.Add_Quad(RectClass(x0, y0, x0 + thick, y0 + len), color);
	// Top-right
	r2d.Add_Quad(RectClass(x1 - len, y0, x1, y0 + thick), color);
	r2d.Add_Quad(RectClass(x1 - thick, y0, x1, y0 + len), color);
	// Bottom-left
	r2d.Add_Quad(RectClass(x0, y1 - thick, x0 + len, y1), color);
	r2d.Add_Quad(RectClass(x0, y1 - len, x0 + thick, y1), color);
	// Bottom-right
	r2d.Add_Quad(RectClass(x1 - len, y1 - thick, x1, y1), color);
	r2d.Add_Quad(RectClass(x1 - thick, y1 - len, x1, y1), color);
}

void drawStatusPill(Int screenW, Int screenH)
{
	if (!TheDisplayStringManager || !TheFontLibrary || screenW < 640 || screenH < 400)
		return;

	const UnsignedInt packed = currentGraphicsPacked();
	const UnsignedInt mode = packed & 0x3u;
	const bool pbrOn = (packed & 0x4u) != 0u;
	const bool postOn = (packed & 0x8u) != 0u;
	const bool texOn = (packed & 0x10u) != 0u;

	if (!s_statusBadgeText)
	{
		s_statusBadgeText = TheDisplayStringManager->newDisplayString();
		if (!s_statusBadgeText)
			return;
		GameFont *font = TheFontLibrary->getFont("Generals", 12, FALSE);
		if (!font)
			font = TheFontLibrary->getFont("Arial", 12, FALSE);
		if (font)
			s_statusBadgeText->setFont(font);
		s_cachedGfxMode = 0xFFFFFFFFu;
	}

	if (s_cachedGfxMode != packed)
	{
		s_cachedGfxMode = packed;
		UnicodeString label;
		if (mode == 2u)
		{
			label.format(
				L"ZeroHourReloaded  |  [F9] A/B Split  [F10] PBR:%ls  [F11] HDR:%ls  [F8] 2x:%ls",
				pbrOn ? L"ON" : L"OFF",
				postOn ? L"ON" : L"OFF",
				texOn ? L"ON" : L"OFF");
		}
		else
		{
			label.format(
				L"ZeroHourReloaded  |  Vulkan 1.3 \u2022 SDL3 \u2022 FFmpeg 8.1  |  [F9] Remastered  [F10] PBR:%ls  [F11] HDR:%ls  [F8] 2x:%ls  [F7] Mesh",
				pbrOn ? L"ON" : L"OFF",
				postOn ? L"ON" : L"OFF",
				texOn ? L"ON" : L"OFF");
		}
		s_statusBadgeText->setText(label);
	}

	Int textW = 0, textH = 0;
	s_statusBadgeText->getSize(&textW, &textH);
	if (textW <= 0 || textH <= 0)
		return;

	const Int padX = 12;
	const Int padY = 5;
	const Int pillW = textW + padX * 2;
	const Int pillH = textH + padY * 2;
	const Int pillX = (mode == 2u) ? (screenW / 2 + 16) : 24;
	const Int pillY = screenH - pillH - 18;

	if (pillX + pillW > screenW - 12)
		return;

	const RectClass pillRect(
		static_cast<float>(pillX),
		static_cast<float>(pillY),
		static_cast<float>(pillX + pillW),
		static_cast<float>(pillY + pillH));

	drawFrostedQuad(
		pillRect,
		RGBA_TO_INT32(8, 17, 30, 190),
		RGBA_TO_INT32(5, 11, 20, 212));

	Render2DClass r2d;
	setupUntextured2D(r2d);
	r2d.Add_Outline(pillRect, 1.0f, RGBA_TO_INT32(84, 168, 240, 110));
	// Left cyan accent pip
	r2d.Add_Quad(
		RectClass(pillRect.Left, pillRect.Top, pillRect.Left + 3.0f, pillRect.Bottom),
		RGBA_TO_INT32(72, 196, 255, 225));
	r2d.Render();

	s_statusBadgeText->draw(
		pillX + padX,
		pillY + padY,
		GameMakeColor(218, 236, 252, 235),
		GameMakeColor(0, 6, 14, 210));
}
} // namespace

Bool isActive()
{
#ifdef ZHR_VULKAN_BACKEND
	if (!TheShell || !TheShell->isShellActive())
		return FALSE;
	return (currentGraphicsPacked() & 0x3u) != 0u ? TRUE : FALSE;
#else
	return FALSE;
#endif
}

Bool isSplitScreen()
{
#ifdef ZHR_VULKAN_BACKEND
	if (!TheShell || !TheShell->isShellActive())
		return FALSE;
	return (currentGraphicsPacked() & 0x3u) == 2u ? TRUE : FALSE;
#else
	return FALSE;
#endif
}

Bool isShellRulerWindow(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return FALSE;
	const char *name = instData->m_decoratedNameString.str();
	if (containsCaseInsensitive(name, "MainMenuRuler") ||
		containsCaseInsensitive(name, "ShellRuler"))
	{
		return TRUE;
	}
	if (BitIsSet(window->winGetStatus(), WIN_STATUS_IMAGE))
	{
		const Image *img = window->winGetEnabledImage(0);
		if (img)
		{
			const char *imgName = img->getName().str();
			if (containsCaseInsensitive(imgName, "Ruler") ||
				containsCaseInsensitive(imgName, "MainMenuGrid"))
			{
				return TRUE;
			}
		}
	}
	return FALSE;
}

Bool shouldStyleShellPanel(GameWindow *window, WinInstanceData *instData)
{
	if (!isActive() || !window || !instData)
		return FALSE;

	const char *name = instData->m_decoratedNameString.str();

	// Never override logo art, faction emblems, tooltips, or full-screen invisible root parents.
	if (containsCaseInsensitive(name, "Logo") ||
		containsCaseInsensitive(name, "Faction") ||
		containsCaseInsensitive(name, "GrowMarker") ||
		containsCaseInsensitive(name, "Parent") ||
		containsCaseInsensitive(name, "Movie") ||
		containsCaseInsensitive(name, "Preview") ||
		containsCaseInsensitive(name, "MapPreview") ||
		containsCaseInsensitive(name, "Thumbnail"))
	{
		return FALSE;
	}

	ICoord2D size;
	window->winGetSize(&size.x, &size.y);
	const Int screenW = TheDisplay ? TheDisplay->getWidth() : 1280;
	const Int screenH = TheDisplay ? TheDisplay->getHeight() : 720;

	// Skip tiny decorative widgets and full-screen overlays.
	if (size.x < 120 || size.y < 72)
		return FALSE;
	if (size.x >= screenW - 24 && size.y >= screenH - 24)
		return FALSE;

	// MainMenu dropdown panel containers (`MapBorder`, `MapBorder1..4`) and dialog panels.
	if (containsCaseInsensitive(name, "MapBorder") ||
		containsCaseInsensitive(name, "MenuBorder") ||
		containsCaseInsensitive(name, "DialogBorder") ||
		containsCaseInsensitive(name, "Panel"))
	{
		return TRUE;
	}

	// Non-image translucent/colored box panels with borders in shell dialogs (Options, Skirmish, Load, etc.).
	if (!BitIsSet(window->winGetStatus(), WIN_STATUS_IMAGE))
	{
		const Color fill = window->winGetEnabledColor(0);
		const Color border = window->winGetEnabledBorderColor(0);
		if (fill != WIN_COLOR_UNDEFINED || border != WIN_COLOR_UNDEFINED)
		{
			return TRUE;
		}
	}

	return FALSE;
}

Bool shouldStyleShellButton(GameWindow *window, WinInstanceData *instData)
{
	if (!isActive() || !window || !instData)
		return FALSE;

	// Never touch in-game cameo/commandbar overlay buttons.
	if (BitIsSet(window->winGetStatus(), WIN_STATUS_USE_OVERLAY_STATES))
		return FALSE;

	// Only style labeled buttons; pure-icon buttons (arrows, flags, close icons) keep their artwork.
	if (instData->getTextLength() <= 0)
		return FALSE;

	ICoord2D size;
	window->winGetSize(&size.x, &size.y);
	if (size.x < 56 || size.y < 18)
		return FALSE;

	return TRUE;
}

void drawShellBackdropFrame(Bool rightHalfOnly)
{
	if (!TheDisplay)
		return;

	if (TheGameClient)
	{
		const UnsignedInt curFrame = TheGameClient->getFrame();
		if (s_lastBackdropFrame == curFrame)
			return;
		s_lastBackdropFrame = curFrame;
	}

	const Int w = TheDisplay->getWidth();
	const Int h = TheDisplay->getHeight();
	if (w <= 0 || h <= 0)
		return;

	TheDisplay->flush();

	const float xStart = rightHalfOnly ? (static_cast<float>(w) * 0.5f) : 0.0f;
	const float xEnd = static_cast<float>(w);
	const float fh = static_cast<float>(h);

	Render2DClass r2d;
	setupUntextured2D(r2d);

	// Soft cinematic top and bottom edge vignettes (frame the live 3D shell battle cleanly).
	const float topVig = fh * 0.085f;
	const float botVig = fh * 0.115f;
	r2d.Add_Quad_VGradient(
		RectClass(xStart, 0.0f, xEnd, topVig),
		RGBA_TO_INT32(4, 9, 17, 155),
		RGBA_TO_INT32(4, 9, 17, 0));
	r2d.Add_Quad_VGradient(
		RectClass(xStart, fh - botVig, xEnd, fh),
		RGBA_TO_INT32(4, 9, 17, 0),
		RGBA_TO_INT32(4, 9, 17, 175));

	// Ultra-thin tactical perimeter hairline + clean L-shaped corner brackets.
	const float m = 14.0f;
	const float frameLeft = rightHalfOnly ? xStart : m;
	const float frameRight = xEnd - m;
	const float frameTop = m;
	const float frameBottom = fh - m;
	const unsigned long lineCol = RGBA_TO_INT32(96, 176, 244, 58);

	// Top & bottom subtle perimeter hairlines
	r2d.Add_Quad(RectClass(frameLeft, frameTop, frameRight, frameTop + 1.0f), lineCol);
	r2d.Add_Quad(RectClass(frameLeft, frameBottom - 1.0f, frameRight, frameBottom), lineCol);
	// Right edge hairline (and left edge when not right-half-only)
	r2d.Add_Quad(RectClass(frameRight - 1.0f, frameTop, frameRight, frameBottom), lineCol);
	if (!rightHalfOnly)
	{
		r2d.Add_Quad(RectClass(frameLeft, frameTop, frameLeft + 1.0f, frameBottom), lineCol);
	}

	const float bracketLen = 18.0f;
	const unsigned long bracketCol = RGBA_TO_INT32(92, 206, 255, 195);
	// Right corners (always)
	r2d.Add_Quad(RectClass(frameRight - bracketLen, frameTop, frameRight, frameTop + 2.0f), bracketCol);
	r2d.Add_Quad(RectClass(frameRight - 2.0f, frameTop, frameRight, frameTop + bracketLen), bracketCol);
	r2d.Add_Quad(RectClass(frameRight - bracketLen, frameBottom - 2.0f, frameRight, frameBottom), bracketCol);
	r2d.Add_Quad(RectClass(frameRight - 2.0f, frameBottom - bracketLen, frameRight, frameBottom), bracketCol);

	if (!rightHalfOnly)
	{
		// Left corners
		r2d.Add_Quad(RectClass(frameLeft, frameTop, frameLeft + bracketLen, frameTop + 2.0f), bracketCol);
		r2d.Add_Quad(RectClass(frameLeft, frameTop, frameLeft + 2.0f, frameTop + bracketLen), bracketCol);
		r2d.Add_Quad(RectClass(frameLeft, frameBottom - 2.0f, frameLeft + bracketLen, frameBottom), bracketCol);
		r2d.Add_Quad(RectClass(frameLeft, frameBottom - bracketLen, frameLeft + 2.0f, frameBottom), bracketCol);
	}

	r2d.Render();

	// Minimal ZeroHourReloaded engine status pill
	drawStatusPill(w, h);
}

void drawGlassPanel(Int x, Int y, Int width, Int height, Bool accentHighlight)
{
	if (width <= 4 || height <= 4)
		return;

	if (TheDisplay)
		TheDisplay->flush();

	const float x0 = static_cast<float>(x);
	const float y0 = static_cast<float>(y);
	const float x1 = static_cast<float>(x + width);
	const float y1 = static_cast<float>(y + height);

	// 1. Soft multi-layer outer elevation shadow
	{
		Render2DClass shadow;
		setupUntextured2D(shadow);
		shadow.Add_Quad(RectClass(x0 - 8.0f, y0 - 4.0f, x1 + 8.0f, y1 + 12.0f), RGBA_TO_INT32(0, 4, 10, 52));
		shadow.Add_Quad(RectClass(x0 - 3.0f, y0 - 1.0f, x1 + 3.0f, y1 + 5.0f), RGBA_TO_INT32(0, 4, 10, 96));
		shadow.Render();
	}

	// 2. Real-time Vulkan frosted-glass backdrop tinted with deep tactical slate-navy gradient
	const RectClass panelRect(x0, y0, x1, y1);
	drawFrostedQuad(
		panelRect,
		RGBA_TO_INT32(10, 22, 40, 156),
		RGBA_TO_INT32(6, 14, 26, 186));

	// 3. Top ambient glass gleam, crisp metallic border, and cyan tactical corner accents
	Render2DClass chrome;
	setupUntextured2D(chrome);

	const float gleamH = (static_cast<float>(height) * 0.22f < 28.0f) ? (static_cast<float>(height) * 0.22f) : 28.0f;
	chrome.Add_Quad_VGradient(
		RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 1.0f + gleamH),
		RGBA_TO_INT32(110, 198, 255, accentHighlight ? 44 : 26),
		RGBA_TO_INT32(110, 198, 255, 0));

	// Crisp 1px outer metallic border
	chrome.Add_Outline(
		panelRect,
		1.0f,
		accentHighlight ? RGBA_TO_INT32(112, 212, 255, 195) : RGBA_TO_INT32(92, 174, 244, 118));

	// Luminous 1px top inner specular edge
	chrome.Add_Quad(
		RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f),
		RGBA_TO_INT32(196, 234, 255, 145));

	// Subtle bottom dark bevel
	chrome.Add_Quad(
		RectClass(x0 + 1.0f, y1 - 2.0f, x1 - 1.0f, y1 - 1.0f),
		RGBA_TO_INT32(0, 6, 14, 140));

	// Crisp tactical corner brackets
	addCornerBrackets(chrome, x0, y0, x1, y1, 12.0f, 2.0f, RGBA_TO_INT32(88, 206, 255, 228));

	chrome.Render();
}

void drawPanelBorder(Int x, Int y, Int width, Int height)
{
	if (width <= 4 || height <= 4)
		return;

	if (TheDisplay)
		TheDisplay->flush();

	const float x0 = static_cast<float>(x);
	const float y0 = static_cast<float>(y);
	const float x1 = static_cast<float>(x + width);
	const float y1 = static_cast<float>(y + height);
	const RectClass rect(x0, y0, x1, y1);

	Render2DClass r2d;
	setupUntextured2D(r2d);
	r2d.Add_Outline(rect, 1.0f, RGBA_TO_INT32(92, 174, 244, 125));
	r2d.Add_Quad(RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f), RGBA_TO_INT32(180, 228, 255, 88));
	r2d.Render();
}

void drawTacticalButton(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return;

	ICoord2D origin, size;
	window->winGetScreenPosition(&origin.x, &origin.y);
	window->winGetSize(&size.x, &size.y);

	const Bool enabled = BitIsSet(window->winGetStatus(), WIN_STATUS_ENABLED);
	const Bool hilited = enabled && BitIsSet(instData->getState(), WIN_STATE_HILITED);
	const Bool selected = enabled && BitIsSet(instData->getState(), WIN_STATE_SELECTED);

	const char *name = instData->m_decoratedNameString.str();
	const bool isExitOrBack =
		containsCaseInsensitive(name, "Exit") ||
		containsCaseInsensitive(name, "Quit") ||
		containsCaseInsensitive(name, "Cancel");

	// Subtle inset so stacked menu buttons breathe cleanly inside their frosted container
	const float insetX = (size.x > 100) ? 2.0f : 1.0f;
	const float insetY = (size.y > 26) ? 2.0f : 1.0f;
	const float x0 = static_cast<float>(origin.x) + insetX;
	const float y0 = static_cast<float>(origin.y) + insetY;
	const float x1 = static_cast<float>(origin.x + size.x) - insetX;
	const float y1 = static_cast<float>(origin.y + size.y) - insetY;
	const RectClass rect(x0, y0, x1, y1);

	unsigned long topCol, botCol, borderCol, railCol, topGleamCol;
	if (!enabled)
	{
		topCol = RGBA_TO_INT32(18, 26, 38, 155);
		botCol = RGBA_TO_INT32(12, 18, 28, 175);
		borderCol = RGBA_TO_INT32(72, 92, 114, 80);
		railCol = RGBA_TO_INT32(90, 110, 130, 110);
		topGleamCol = RGBA_TO_INT32(140, 165, 190, 24);
	}
	else if (selected)
	{
		topCol = RGBA_TO_INT32(34, 96, 154, 242);
		botCol = RGBA_TO_INT32(18, 58, 102, 246);
		borderCol = RGBA_TO_INT32(255, 212, 92, 245);
		railCol = RGBA_TO_INT32(255, 212, 92, 255);
		topGleamCol = RGBA_TO_INT32(255, 242, 196, 120);
	}
	else if (hilited)
	{
		topCol = isExitOrBack ? RGBA_TO_INT32(104, 34, 38, 236) : RGBA_TO_INT32(26, 74, 126, 236);
		botCol = isExitOrBack ? RGBA_TO_INT32(64, 18, 24, 244) : RGBA_TO_INT32(14, 44, 82, 244);
		borderCol = isExitOrBack ? RGBA_TO_INT32(255, 122, 104, 238) : RGBA_TO_INT32(116, 220, 255, 242);
		railCol = isExitOrBack ? RGBA_TO_INT32(255, 108, 92, 255) : RGBA_TO_INT32(255, 208, 76, 255);
		topGleamCol = RGBA_TO_INT32(224, 244, 255, 115);
	}
	else
	{
		topCol = RGBA_TO_INT32(16, 36, 62, 166);
		botCol = RGBA_TO_INT32(9, 22, 42, 194);
		borderCol = isExitOrBack ? RGBA_TO_INT32(182, 92, 96, 132) : RGBA_TO_INT32(88, 168, 242, 122);
		railCol = isExitOrBack ? RGBA_TO_INT32(232, 88, 76, 230) : RGBA_TO_INT32(72, 192, 255, 232);
		topGleamCol = RGBA_TO_INT32(190, 232, 255, 58);
	}

	// Frosted-glass base fill so live 3D shell explosions subtly glow through menu buttons
	drawFrostedQuad(rect, topCol, botCol);

	Render2DClass r2d;
	setupUntextured2D(r2d);

	// Soft hover light sweep across the left 60% of the button
	if (hilited || selected)
	{
		const float sweepX = x0 + (x1 - x0) * 0.65f;
		const unsigned long sweepCol = isExitOrBack
			? RGBA_TO_INT32(255, 128, 108, 46)
			: RGBA_TO_INT32(102, 214, 255, 52);
		r2d.Add_Quad_HGradient(
			RectClass(x0 + 3.0f, y0 + 1.0f, sweepX, y1 - 1.0f),
			sweepCol,
			RGBA_TO_INT32(102, 214, 255, 0));
	}

	// Outer 1px crisp metallic border
	r2d.Add_Outline(rect, 1.0f, borderCol);

	// 1px top specular highlight
	r2d.Add_Quad(RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f), topGleamCol);

	// Luminous left tactical accent rail (wider on hover)
	const float railW = (hilited || selected) ? 4.0f : 3.0f;
	r2d.Add_Quad(RectClass(x0, y0, x0 + railW, y1), railCol);

	// Tiny matching right-edge accent tick on hover
	if (hilited || selected)
	{
		r2d.Add_Quad(RectClass(x1 - 2.0f, y0 + 4.0f, x1, y1 - 4.0f), borderCol);
	}

	r2d.Render();

	// Render crisp 8-bit antialiased button text centered inside the button
	DisplayString *text = instData->getTextDisplayString();
	if (text && text->getTextLength() > 0)
	{
		text->setWordWrapCentered(BitIsSet(instData->getStatus(), WIN_STATUS_WRAP_CENTERED));
		text->setWordWrap(size.x - 12);

		if (text->getFont() != window->winGetFont())
			text->setFont(window->winGetFont());

		Int textW = 0, textH = 0;
		text->getSize(&textW, &textH);

		Color textColor, dropColor;
		if (!enabled)
		{
			textColor = GameMakeColor(136, 152, 170, 210);
			dropColor = GameMakeColor(0, 6, 14, 150);
		}
		else if (hilited || selected)
		{
			textColor = GameMakeColor(255, 255, 255, 255);
			dropColor = GameMakeColor(0, 12, 28, 235);
		}
		else
		{
			textColor = GameMakeColor(232, 244, 255, 255);
			dropColor = GameMakeColor(0, 8, 18, 220);
		}

		Int textX = origin.x + (size.x / 2) - (textW / 2);
		Int textY = origin.y + (size.y / 2) - (textH / 2);
		if (BitIsSet(window->winGetStatus(), WIN_STATUS_SHORTCUT_BUTTON))
		{
			textX = origin.x + 2;
			textY = origin.y;
		}
		text->draw(textX, textY, textColor, dropColor);
	}
}

void reset()
{
	if (s_statusBadgeText && TheDisplayStringManager)
	{
		TheDisplayStringManager->freeDisplayString(s_statusBadgeText);
	}
	s_statusBadgeText = nullptr;
	s_cachedGfxMode = 0xFFFFFFFFu;
	s_lastBackdropFrame = 0xFFFFFFFFu;
}

} // namespace ZhrMenuStyle
