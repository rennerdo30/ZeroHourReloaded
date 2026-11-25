/*
**  ZeroHourReloaded — Modern Tactical Shell Menu & UI Styling
**  Copyright (C) 2026 ZeroHourReloaded Contributors (GPL-3.0-or-later)
*/

#include "W3DDevice/GameClient/ZhrMenuStyle.h"

#include <cstring>
#include <cctype>
#include <algorithm>

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
#include "GameClient/GlobalLanguage.h"
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

const char *localWindowName(const char *decorated)
{
	if (!decorated)
		return "";
	const char *colon = std::strrchr(decorated, ':');
	return colon ? (colon + 1) : decorated;
}

bool isRemasteredGraphicsActive()
{
#ifdef ZHR_VULKAN_BACKEND
	return (zhr_vk_get_graphics_mode() & 0x3u) != 0u;
#else
	return false;
#endif
}

UnsignedInt currentGraphicsPacked()
{
#ifdef ZHR_VULKAN_BACKEND
	return zhr_vk_get_graphics_mode();
#else
	return 0;
#endif
}

bool isInModalMenuHierarchy(GameWindow *window)
{
	Int depth = 0;
	for (GameWindow *cur = window; cur != nullptr && depth < 12; cur = cur->winGetParent(), ++depth)
	{
		WinInstanceData *inst = cur->winGetInstanceData();
		if (!inst)
			continue;
		const char *name = inst->m_decoratedNameString.str();
		if (containsCaseInsensitive(name, "OptionsMenu") ||
			containsCaseInsensitive(name, "WinAdvancedDisplay") ||
			containsCaseInsensitive(name, "Zhr"))
		{
			return true;
		}
	}
	return false;
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

	const Int targetFontPt = TheGlobalLanguageData
		? std::max(10, TheGlobalLanguageData->adjustFontSize(9))
		: 12;
	static Int s_statusBadgeFontPt = -1;

	if (!s_statusBadgeText || s_statusBadgeFontPt != targetFontPt)
	{
		if (!s_statusBadgeText)
			s_statusBadgeText = TheDisplayStringManager->newDisplayString();
		if (!s_statusBadgeText)
			return;
		GameFont *font = TheFontLibrary->getFont("Generals", targetFontPt, FALSE);
		if (!font)
			font = TheFontLibrary->getFont("Arial", targetFontPt, FALSE);
		if (font)
			s_statusBadgeText->setFont(font);
		s_statusBadgeFontPt = targetFontPt;
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

	const Real uiScale = (TheDisplay && !TheDisplay->isUiScaleClassicStretched())
		? std::max(1.0f, TheDisplay->getDpiScale())
		: 1.0f;
	const Int padX = static_cast<Int>(12.0f * uiScale);
	const Int padY = static_cast<Int>(5.0f * uiScale);
	const Int pillW = textW + padX * 2;
	const Int pillH = textH + padY * 2;
	const Int margin = static_cast<Int>(18.0f * uiScale);
	const Int pillX = (mode == 2u) ? (screenW / 2 + margin) : static_cast<Int>(24.0f * uiScale);
	const Int pillY = screenH - pillH - margin;

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
	return isRemasteredGraphicsActive() ? TRUE : FALSE;
#else
	return FALSE;
#endif
}

Bool isStyledForWindow(GameWindow *window)
{
	if (!isRemasteredGraphicsActive())
		return FALSE;
	if (TheShell && TheShell->isShellActive())
		return TRUE;
	return (window && isInModalMenuHierarchy(window)) ? TRUE : FALSE;
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

Bool isDividerLineWindow(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return FALSE;
	const char *local = localWindowName(instData->m_decoratedNameString.str());
	if (!local || !*local)
		return FALSE;
	if (containsCaseInsensitive(local, "ZhrDivider"))
		return TRUE;
	// Classic .wnd divider windows: Line, Line1..Line9, AdvanceOptionsLine, ToggleOnOffLine, LowResLine, ParticleCapLine
	if (_stricmp(local, "Line") == 0)
		return TRUE;
	if (std::strlen(local) == 5 && _strnicmp(local, "Line", 4) == 0 && std::isdigit(static_cast<unsigned char>(local[4])))
		return TRUE;
	if (containsCaseInsensitive(local, "OptionsLine") ||
		containsCaseInsensitive(local, "OnOffLine") ||
		containsCaseInsensitive(local, "LowResLine") ||
		containsCaseInsensitive(local, "ParticleCapLine"))
	{
		return TRUE;
	}
	return FALSE;
}

Bool isSubPanelWindow(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return FALSE;
	const char *name = instData->m_decoratedNameString.str();
	return containsCaseInsensitive(name, "ZhrSubPanel") ? TRUE : FALSE;
}

Bool shouldStyleShellPanel(GameWindow *window, WinInstanceData *instData)
{
	if (!isStyledForWindow(window) || !window || !instData)
		return FALSE;

	if (isDividerLineWindow(window, instData) || isSubPanelWindow(window, instData))
		return FALSE;

	// Never paint frosted glass onto transparent click-catcher / SEE_THRU wrapper containers.
	if (BitIsSet(window->winGetStatus(), WIN_STATUS_SEE_THRU))
		return FALSE;

	const char *name = instData->m_decoratedNameString.str();

	// Explicitly allow the centered Options / Advanced Display modal frames to receive frosted-glass styling.
	if (containsCaseInsensitive(name, "OptionsMenuParentOld") ||
		containsCaseInsensitive(name, "WinAdvancedDisplayOptions"))
	{
		return TRUE;
	}

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
	if (!isStyledForWindow(window) || !window || !instData)
		return FALSE;

	// Never touch in-game cameo/commandbar overlay buttons.
	if (BitIsSet(window->winGetStatus(), WIN_STATUS_USE_OVERLAY_STATES))
		return FALSE;

	// Intercept horizontal slider thumbs (rendered natively by drawTacticalSlider) and ComboBox dropdown caps.
	GameWindow *parent = window->winGetParent();
	if (parent)
	{
		if (BitIsSet(parent->winGetStyle(), GWS_HORZ_SLIDER))
			return TRUE;
		if (BitIsSet(parent->winGetStyle(), GWS_COMBO_BOX))
			return TRUE;
	}

	// Only style labeled buttons; pure-icon buttons (arrows, flags, close icons) keep their artwork.
	if (instData->getTextLength() <= 0)
		return FALSE;

	ICoord2D size;
	window->winGetSize(&size.x, &size.y);
	if (size.x < 48 || size.y < 18)
		return FALSE;

	return TRUE;
}

Bool shouldStyleShellGadget(GameWindow *window)
{
	return isStyledForWindow(window);
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

	const float m = 14.0f;
	const float frameLeft = rightHalfOnly ? xStart : m;
	const float frameRight = xEnd - m;
	const float frameTop = m;
	const float frameBottom = fh - m;
	const unsigned long lineCol = RGBA_TO_INT32(96, 176, 244, 58);

	r2d.Add_Quad(RectClass(frameLeft, frameTop, frameRight, frameTop + 1.0f), lineCol);
	r2d.Add_Quad(RectClass(frameLeft, frameBottom - 1.0f, frameRight, frameBottom), lineCol);
	r2d.Add_Quad(RectClass(frameRight - 1.0f, frameTop, frameRight, frameBottom), lineCol);
	if (!rightHalfOnly)
	{
		r2d.Add_Quad(RectClass(frameLeft, frameTop, frameLeft + 1.0f, frameBottom), lineCol);
	}

	const float bracketLen = 18.0f;
	const unsigned long bracketCol = RGBA_TO_INT32(92, 206, 255, 195);
	r2d.Add_Quad(RectClass(frameRight - bracketLen, frameTop, frameRight, frameTop + 2.0f), bracketCol);
	r2d.Add_Quad(RectClass(frameRight - 2.0f, frameTop, frameRight, frameTop + bracketLen), bracketCol);
	r2d.Add_Quad(RectClass(frameRight - bracketLen, frameBottom - 2.0f, frameRight, frameBottom), bracketCol);
	r2d.Add_Quad(RectClass(frameRight - 2.0f, frameBottom - bracketLen, frameRight, frameBottom), bracketCol);

	if (!rightHalfOnly)
	{
		r2d.Add_Quad(RectClass(frameLeft, frameTop, frameLeft + bracketLen, frameTop + 2.0f), bracketCol);
		r2d.Add_Quad(RectClass(frameLeft, frameTop, frameLeft + 2.0f, frameTop + bracketLen), bracketCol);
		r2d.Add_Quad(RectClass(frameLeft, frameBottom - 2.0f, frameLeft + bracketLen, frameBottom), bracketCol);
		r2d.Add_Quad(RectClass(frameLeft, frameBottom - bracketLen, frameLeft + 2.0f, frameBottom), bracketCol);
	}

	r2d.Render();

	drawStatusPill(w, h);
}

void drawDividerLine(Int x, Int y, Int width, Int height)
{
	if (width <= 0 || height <= 0)
		return;
	if (TheDisplay)
		TheDisplay->flush();

	Render2DClass r2d;
	setupUntextured2D(r2d);

	if (width >= height)
	{
		const float cy = static_cast<float>(y) + static_cast<float>(height) * 0.5f;
		const float x0 = static_cast<float>(x);
		const float x1 = static_cast<float>(x + width);
		const float mid = (x0 + x1) * 0.5f;
		r2d.Add_Quad_HGradient(
			RectClass(x0, cy, mid, cy + 1.0f),
			RGBA_TO_INT32(78, 178, 248, 36),
			RGBA_TO_INT32(96, 198, 255, 148));
		r2d.Add_Quad_HGradient(
			RectClass(mid, cy, x1, cy + 1.0f),
			RGBA_TO_INT32(96, 198, 255, 148),
			RGBA_TO_INT32(78, 178, 248, 36));
	}
	else
	{
		const float cx = static_cast<float>(x) + static_cast<float>(width) * 0.5f;
		const float y0 = static_cast<float>(y);
		const float y1 = static_cast<float>(y + height);
		const float mid = (y0 + y1) * 0.5f;
		r2d.Add_Quad_VGradient(
			RectClass(cx, y0, cx + 1.0f, mid),
			RGBA_TO_INT32(78, 178, 248, 28),
			RGBA_TO_INT32(96, 198, 255, 130));
		r2d.Add_Quad_VGradient(
			RectClass(cx, mid, cx + 1.0f, y1),
			RGBA_TO_INT32(96, 198, 255, 130),
			RGBA_TO_INT32(78, 178, 248, 28));
	}

	r2d.Render();
}

void drawSubPanel(Int x, Int y, Int width, Int height)
{
	if (width <= 8 || height <= 8)
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

	// Recessed dark tactical slate card
	r2d.Add_Quad_VGradient(
		rect,
		RGBA_TO_INT32(8, 18, 33, 168),
		RGBA_TO_INT32(5, 12, 23, 192));

	// Subtle 1px metallic border
	r2d.Add_Outline(rect, 1.0f, RGBA_TO_INT32(82, 162, 234, 88));

	// Crisp 2px top luminous cyan accent header strip
	r2d.Add_Quad_HGradient(
		RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 3.0f),
		RGBA_TO_INT32(74, 198, 255, 200),
		RGBA_TO_INT32(74, 198, 255, 36));

	r2d.Render();
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
		shadow.Add_Quad(RectClass(x0 - 8.0f, y0 - 4.0f, x1 + 8.0f, y1 + 12.0f), RGBA_TO_INT32(0, 4, 10, 56));
		shadow.Add_Quad(RectClass(x0 - 3.0f, y0 - 1.0f, x1 + 3.0f, y1 + 5.0f), RGBA_TO_INT32(0, 4, 10, 104));
		shadow.Render();
	}

	// 2. Real-time Vulkan frosted-glass backdrop tinted with deep tactical slate-navy gradient
	const RectClass panelRect(x0, y0, x1, y1);
	const bool isLargeModal = (width >= 600 && height >= 420);
	drawFrostedQuad(
		panelRect,
		isLargeModal ? RGBA_TO_INT32(8, 18, 34, 214) : RGBA_TO_INT32(10, 22, 40, 178),
		isLargeModal ? RGBA_TO_INT32(5, 11, 22, 234) : RGBA_TO_INT32(6, 13, 25, 208));

	// 3. Top ambient glass gleam, crisp metallic border, and cyan tactical corner accents
	Render2DClass chrome;
	setupUntextured2D(chrome);

	const float gleamH = (static_cast<float>(height) * 0.16f < 28.0f) ? (static_cast<float>(height) * 0.16f) : 28.0f;
	chrome.Add_Quad_VGradient(
		RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 1.0f + gleamH),
		RGBA_TO_INT32(110, 198, 255, accentHighlight ? 44 : 26),
		RGBA_TO_INT32(110, 198, 255, 0));

	chrome.Add_Outline(
		panelRect,
		1.0f,
		accentHighlight ? RGBA_TO_INT32(112, 212, 255, 195) : RGBA_TO_INT32(92, 174, 244, 126));

	chrome.Add_Quad(
		RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f),
		RGBA_TO_INT32(196, 234, 255, 145));

	chrome.Add_Quad(
		RectClass(x0 + 1.0f, y1 - 2.0f, x1 - 1.0f, y1 - 1.0f),
		RGBA_TO_INT32(0, 6, 14, 145));

	addCornerBrackets(chrome, x0, y0, x1, y1, 14.0f, 2.0f, RGBA_TO_INT32(88, 206, 255, 228));

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

	// 1. Suppress legacy horizontal slider thumb bitmap (drawTacticalSlider renders the sleek modern pill thumb).
	GameWindow *parent = window->winGetParent();
	if (parent && BitIsSet(parent->winGetStyle(), GWS_HORZ_SLIDER))
	{
		return;
	}

	ICoord2D origin, size;
	window->winGetScreenPosition(&origin.x, &origin.y);
	window->winGetSize(&size.x, &size.y);

	const Bool enabled = BitIsSet(window->winGetStatus(), WIN_STATUS_ENABLED);
	const Bool hilited = enabled && BitIsSet(instData->getState(), WIN_STATE_HILITED);
	const Bool selected = enabled && BitIsSet(instData->getState(), WIN_STATE_SELECTED);

	// 2. ComboBox dropdown arrow cap: clean slate cap + crisp vector chevron.
	if (parent && BitIsSet(parent->winGetStyle(), GWS_COMBO_BOX))
	{
		if (TheDisplay)
			TheDisplay->flush();

		const float x0 = static_cast<float>(origin.x);
		const float y0 = static_cast<float>(origin.y);
		const float x1 = static_cast<float>(origin.x + size.x);
		const float y1 = static_cast<float>(origin.y + size.y);
		const RectClass rect(x0, y0, x1, y1);

		Render2DClass r2d;
		setupUntextured2D(r2d);
		const unsigned long topCol = !enabled
			? RGBA_TO_INT32(18, 28, 42, 180)
			: hilited
				? RGBA_TO_INT32(30, 88, 146, 242)
				: RGBA_TO_INT32(16, 40, 68, 232);
		const unsigned long botCol = !enabled
			? RGBA_TO_INT32(12, 20, 32, 195)
			: hilited
				? RGBA_TO_INT32(18, 56, 98, 248)
				: RGBA_TO_INT32(10, 26, 48, 242);
		const unsigned long borderCol = !enabled
			? RGBA_TO_INT32(72, 92, 114, 90)
			: hilited
				? RGBA_TO_INT32(116, 220, 255, 240)
				: RGBA_TO_INT32(84, 166, 238, 155);
		const unsigned long chevCol = !enabled
			? RGBA_TO_INT32(120, 142, 164, 190)
			: hilited
				? RGBA_TO_INT32(255, 255, 255, 255)
				: RGBA_TO_INT32(174, 224, 255, 245);

		r2d.Add_Quad_VGradient(rect, topCol, botCol);
		r2d.Add_Outline(rect, 1.0f, borderCol);

		const float cx = (x0 + x1) * 0.5f;
		const float cy = (y0 + y1) * 0.5f;
		const float arm = std::max(3.5f, std::min(6.0f, static_cast<float>(size.x) * 0.24f));
		const float drop = arm * 0.65f;
		r2d.Add_Line(Vector2(cx - arm, cy - drop * 0.5f), Vector2(cx, cy + drop * 0.5f), 2.0f, chevCol);
		r2d.Add_Line(Vector2(cx, cy + drop * 0.5f), Vector2(cx + arm, cy - drop * 0.5f), 2.0f, chevCol);
		r2d.Render();
		return;
	}

	const char *name = instData->m_decoratedNameString.str();

	// 3. Horizontal Tab Bar Button (`ZhrTab*`)
	if (containsCaseInsensitive(name, "ZhrTab"))
	{
		if (TheDisplay)
			TheDisplay->flush();

		const float x0 = static_cast<float>(origin.x) + 2.0f;
		const float y0 = static_cast<float>(origin.y) + 1.0f;
		const float x1 = static_cast<float>(origin.x + size.x) - 2.0f;
		const float y1 = static_cast<float>(origin.y + size.y) - 1.0f;
		const RectClass rect(x0, y0, x1, y1);

		Render2DClass r2d;
		setupUntextured2D(r2d);

		if (selected)
		{
			r2d.Add_Quad_VGradient(
				rect,
				RGBA_TO_INT32(22, 68, 114, 210),
				RGBA_TO_INT32(14, 42, 76, 232));
			r2d.Add_Outline(rect, 1.0f, RGBA_TO_INT32(94, 196, 255, 165));
			// Luminous 3px cyan active underline
			r2d.Add_Quad(RectClass(x0, y1 - 3.0f, x1, y1), RGBA_TO_INT32(82, 214, 255, 255));
		}
		else if (hilited)
		{
			r2d.Add_Quad_VGradient(
				rect,
				RGBA_TO_INT32(18, 48, 82, 175),
				RGBA_TO_INT32(11, 28, 52, 198));
			r2d.Add_Outline(rect, 1.0f, RGBA_TO_INT32(86, 178, 246, 125));
			r2d.Add_Quad(RectClass(x0, y1 - 2.0f, x1, y1), RGBA_TO_INT32(96, 198, 255, 160));
		}
		else
		{
			r2d.Add_Quad_VGradient(
				rect,
				RGBA_TO_INT32(10, 24, 42, 130),
				RGBA_TO_INT32(7, 16, 30, 155));
			r2d.Add_Outline(rect, 1.0f, RGBA_TO_INT32(68, 128, 186, 68));
		}
		r2d.Render();

		DisplayString *text = instData->getTextDisplayString();
		if (text && text->getTextLength() > 0)
		{
			if (text->getFont() != window->winGetFont())
				text->setFont(window->winGetFont());
			Int textW = 0, textH = 0;
			text->getSize(&textW, &textH);
			const Color textCol = selected
				? GameMakeColor(255, 255, 255, 255)
				: hilited
					? GameMakeColor(232, 246, 255, 255)
					: GameMakeColor(172, 202, 228, 232);
			const Color dropCol = GameMakeColor(0, 8, 18, 220);
			text->draw(
				origin.x + (size.x - textW) / 2,
				origin.y + (size.y - textH) / 2,
				textCol,
				dropCol);
		}
		return;
	}

	// 4. Standard Tactical Push Button
	const bool isExitOrBack =
		containsCaseInsensitive(name, "Exit") ||
		containsCaseInsensitive(name, "Quit") ||
		containsCaseInsensitive(name, "Cancel") ||
		containsCaseInsensitive(name, "ButtonBack") ||
		containsCaseInsensitive(name, "AdvanceBack");

	const bool isPrimaryAccept =
		containsCaseInsensitive(name, "ButtonAccept") ||
		containsCaseInsensitive(name, "AdvanceAccept");

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
		if (isExitOrBack)
		{
			topCol = RGBA_TO_INT32(104, 34, 38, 236);
			botCol = RGBA_TO_INT32(64, 18, 24, 244);
			borderCol = RGBA_TO_INT32(255, 122, 104, 238);
			railCol = RGBA_TO_INT32(255, 108, 92, 255);
		}
		else if (isPrimaryAccept)
		{
			topCol = RGBA_TO_INT32(22, 106, 142, 242);
			botCol = RGBA_TO_INT32(12, 66, 98, 248);
			borderCol = RGBA_TO_INT32(110, 238, 255, 248);
			railCol = RGBA_TO_INT32(94, 242, 218, 255);
		}
		else
		{
			topCol = RGBA_TO_INT32(26, 74, 126, 236);
			botCol = RGBA_TO_INT32(14, 44, 82, 244);
			borderCol = RGBA_TO_INT32(116, 220, 255, 242);
			railCol = RGBA_TO_INT32(255, 208, 76, 255);
		}
		topGleamCol = RGBA_TO_INT32(224, 244, 255, 115);
	}
	else
	{
		if (isPrimaryAccept)
		{
			topCol = RGBA_TO_INT32(18, 74, 108, 216);
			botCol = RGBA_TO_INT32(10, 46, 74, 232);
			borderCol = RGBA_TO_INT32(84, 212, 248, 195);
			railCol = RGBA_TO_INT32(72, 228, 214, 248);
		}
		else
		{
			topCol = RGBA_TO_INT32(16, 36, 62, 174);
			botCol = RGBA_TO_INT32(9, 22, 42, 202);
			borderCol = isExitOrBack ? RGBA_TO_INT32(182, 92, 96, 138) : RGBA_TO_INT32(88, 168, 242, 126);
			railCol = isExitOrBack ? RGBA_TO_INT32(232, 88, 76, 230) : RGBA_TO_INT32(72, 192, 255, 232);
		}
		topGleamCol = RGBA_TO_INT32(190, 232, 255, 58);
	}

	drawFrostedQuad(rect, topCol, botCol);

	Render2DClass r2d;
	setupUntextured2D(r2d);

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

	r2d.Add_Outline(rect, 1.0f, borderCol);
	r2d.Add_Quad(RectClass(x0 + 1.0f, y0 + 1.0f, x1 - 1.0f, y0 + 2.0f), topGleamCol);

	const float railW = (hilited || selected) ? 4.0f : 3.0f;
	r2d.Add_Quad(RectClass(x0, y0, x0 + railW, y1), railCol);

	if (hilited || selected)
	{
		r2d.Add_Quad(RectClass(x1 - 2.0f, y0 + 4.0f, x1, y1 - 4.0f), borderCol);
	}

	r2d.Render();

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

void drawTacticalCheckBox(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return;
	if (TheDisplay)
		TheDisplay->flush();

	ICoord2D origin, size;
	window->winGetScreenPosition(&origin.x, &origin.y);
	window->winGetSize(&size.x, &size.y);

	const Bool enabled = BitIsSet(window->winGetStatus(), WIN_STATUS_ENABLED);
	const Bool hilited = enabled && BitIsSet(instData->getState(), WIN_STATE_HILITED);
	const Bool checked = BitIsSet(instData->getState(), WIN_STATE_SELECTED);

	const Int boxDim = std::max(16, std::min(22, size.y - 6));
	const float bx0 = static_cast<float>(origin.x + 4);
	const float by0 = static_cast<float>(origin.y + (size.y - boxDim) / 2);
	const float bx1 = bx0 + static_cast<float>(boxDim);
	const float by1 = by0 + static_cast<float>(boxDim);

	Render2DClass r2d;
	setupUntextured2D(r2d);

	// Subtle hover row highlight for tactile feedback
	if (hilited)
	{
		r2d.Add_Quad_HGradient(
			RectClass(
				static_cast<float>(origin.x),
				static_cast<float>(origin.y + 1),
				static_cast<float>(origin.x + size.x),
				static_cast<float>(origin.y + size.y - 1)),
			RGBA_TO_INT32(42, 118, 184, 58),
			RGBA_TO_INT32(42, 118, 184, 8));
	}

	const RectClass boxRect(bx0, by0, bx1, by1);
	if (!enabled)
	{
		r2d.Add_Quad(boxRect, checked ? RGBA_TO_INT32(42, 72, 96, 175) : RGBA_TO_INT32(12, 20, 32, 165));
		r2d.Add_Outline(boxRect, 1.0f, RGBA_TO_INT32(76, 98, 120, 120));
	}
	else if (checked)
	{
		r2d.Add_Quad_VGradient(
			boxRect,
			hilited ? RGBA_TO_INT32(68, 204, 255, 250) : RGBA_TO_INT32(44, 176, 246, 242),
			hilited ? RGBA_TO_INT32(30, 146, 224, 255) : RGBA_TO_INT32(20, 120, 198, 248));
		r2d.Add_Outline(boxRect, 1.0f, RGBA_TO_INT32(154, 236, 255, 255));
	}
	else
	{
		r2d.Add_Quad_VGradient(
			boxRect,
			RGBA_TO_INT32(10, 22, 38, 228),
			RGBA_TO_INT32(6, 14, 26, 240));
		r2d.Add_Outline(
			boxRect,
			1.0f,
			hilited ? RGBA_TO_INT32(114, 216, 255, 235) : RGBA_TO_INT32(84, 164, 234, 155));
	}

	// Crisp vector checkmark
	if (checked)
	{
		const unsigned long tickCol = enabled
			? RGBA_TO_INT32(255, 255, 255, 255)
			: RGBA_TO_INT32(168, 190, 208, 210);
		const float w = static_cast<float>(boxDim);
		const Vector2 p0(bx0 + w * 0.22f, by0 + w * 0.52f);
		const Vector2 p1(bx0 + w * 0.43f, by0 + w * 0.74f);
		const Vector2 p2(bx0 + w * 0.80f, by0 + w * 0.28f);
		r2d.Add_Line(p0, p1, 2.2f, tickCol);
		r2d.Add_Line(p1, p2, 2.2f, tickCol);
	}

	r2d.Render();

	// Crisp vertically centered label text
	DisplayString *text = instData->getTextDisplayString();
	if (text && text->getTextLength() > 0)
	{
		if (text->getFont() != window->winGetFont())
			text->setFont(window->winGetFont());

		Int textW = 0, textH = 0;
		text->getSize(&textW, &textH);

		Color textColor, dropColor;
		if (!enabled)
		{
			textColor = GameMakeColor(136, 154, 174, 200);
			dropColor = GameMakeColor(0, 6, 14, 150);
		}
		else if (hilited)
		{
			textColor = GameMakeColor(255, 255, 255, 255);
			dropColor = GameMakeColor(0, 10, 22, 230);
		}
		else
		{
			textColor = GameMakeColor(226, 240, 252, 250);
			dropColor = GameMakeColor(0, 8, 18, 215);
		}

		const Int textX = origin.x + 4 + boxDim + 10;
		const Int textY = origin.y + (size.y - textH) / 2;
		text->draw(textX, textY, textColor, dropColor);
	}
}

void drawTacticalSlider(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return;
	if (TheDisplay)
		TheDisplay->flush();

	ICoord2D origin, size;
	window->winGetScreenPosition(&origin.x, &origin.y);
	window->winGetSize(&size.x, &size.y);

	SliderData *s = static_cast<SliderData *>(window->winGetUserData());
	float frac = 0.5f;
	if (s && s->maxVal > s->minVal)
	{
		frac = static_cast<float>(s->position - s->minVal) / static_cast<float>(s->maxVal - s->minVal);
		frac = std::max(0.0f, std::min(1.0f, frac));
	}

	const Bool enabled = BitIsSet(window->winGetStatus(), WIN_STATUS_ENABLED);
	const Bool hilited = enabled && BitIsSet(instData->getState(), WIN_STATE_HILITED);

	const float thumbW = static_cast<float>(std::max(12, std::min(16, size.y - 4)));
	const float thumbH = static_cast<float>(std::max(16, std::min(24, size.y - 2)));
	const float trackH = static_cast<float>(std::max(6, std::min(9, size.y / 3)));

	const float trackLeft = static_cast<float>(origin.x) + thumbW * 0.5f;
	const float trackRight = static_cast<float>(origin.x + size.x) - thumbW * 0.5f;
	const float cy = static_cast<float>(origin.y) + static_cast<float>(size.y) * 0.5f;
	const float trackTop = cy - trackH * 0.5f;
	const float trackBot = cy + trackH * 0.5f;
	const float thumbCenterX = trackLeft + frac * std::max(1.0f, trackRight - trackLeft);

	Render2DClass r2d;
	setupUntextured2D(r2d);

	// Recessed dark track
	const RectClass trackRect(trackLeft, trackTop, trackRight, trackBot);
	r2d.Add_Quad_VGradient(
		trackRect,
		RGBA_TO_INT32(6, 14, 26, 232),
		RGBA_TO_INT32(10, 22, 38, 220));
	r2d.Add_Outline(
		trackRect,
		1.0f,
		hilited ? RGBA_TO_INT32(102, 198, 255, 165) : RGBA_TO_INT32(72, 140, 206, 110));

	// Active cyan gradient fill
	if (thumbCenterX > trackLeft + 1.5f)
	{
		const RectClass fillRect(trackLeft + 1.0f, trackTop + 1.0f, thumbCenterX, trackBot - 1.0f);
		r2d.Add_Quad_HGradient(
			fillRect,
			enabled ? RGBA_TO_INT32(22, 126, 208, 240) : RGBA_TO_INT32(64, 92, 118, 180),
			enabled ? RGBA_TO_INT32(84, 218, 255, 255) : RGBA_TO_INT32(94, 124, 148, 190));
	}

	// Soft subtle tick marks at 25%, 50%, 75%
	for (int tick = 1; tick <= 3; ++tick)
	{
		const float tx = trackLeft + (trackRight - trackLeft) * (static_cast<float>(tick) * 0.25f);
		r2d.Add_Quad(
			RectClass(tx, trackBot + 2.0f, tx + 1.0f, trackBot + 5.0f),
			RGBA_TO_INT32(92, 168, 232, 84));
	}

	// Modern glowing pill thumb handle
	const RectClass thumbRect(
		thumbCenterX - thumbW * 0.5f,
		cy - thumbH * 0.5f,
		thumbCenterX + thumbW * 0.5f,
		cy + thumbH * 0.5f);
	// Soft shadow behind thumb
	r2d.Add_Quad(
		RectClass(thumbRect.Left - 2.0f, thumbRect.Top - 1.0f, thumbRect.Right + 2.0f, thumbRect.Bottom + 3.0f),
		RGBA_TO_INT32(0, 6, 14, 110));
	r2d.Add_Quad_VGradient(
		thumbRect,
		!enabled
			? RGBA_TO_INT32(104, 124, 142, 220)
			: hilited
				? RGBA_TO_INT32(236, 250, 255, 255)
				: RGBA_TO_INT32(182, 228, 255, 250),
		!enabled
			? RGBA_TO_INT32(72, 88, 104, 220)
			: hilited
				? RGBA_TO_INT32(108, 218, 255, 255)
				: RGBA_TO_INT32(64, 178, 244, 250));
	r2d.Add_Outline(
		thumbRect,
		1.0f,
		hilited ? RGBA_TO_INT32(255, 255, 255, 255) : RGBA_TO_INT32(124, 226, 255, 240));
	// Center tactical grip line
	r2d.Add_Quad(
		RectClass(thumbCenterX - 0.5f, cy - thumbH * 0.25f, thumbCenterX + 0.5f, cy + thumbH * 0.25f),
		RGBA_TO_INT32(8, 36, 68, 170));

	r2d.Render();
}

void drawTacticalTextEntryFrame(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return;
	if (TheDisplay)
		TheDisplay->flush();

	ICoord2D origin, size;
	window->winGetScreenPosition(&origin.x, &origin.y);
	window->winGetSize(&size.x, &size.y);

	const Bool enabled = BitIsSet(window->winGetStatus(), WIN_STATUS_ENABLED);
	const Bool focused = (TheWindowManager && TheWindowManager->winGetFocus() == window);
	const Bool hilited = enabled && (BitIsSet(instData->getState(), WIN_STATE_HILITED) || focused);

	const float x0 = static_cast<float>(origin.x);
	const float y0 = static_cast<float>(origin.y);
	const float x1 = static_cast<float>(origin.x + size.x);
	const float y1 = static_cast<float>(origin.y + size.y);
	const RectClass rect(x0, y0, x1, y1);

	Render2DClass r2d;
	setupUntextured2D(r2d);

	r2d.Add_Quad_VGradient(
		rect,
		!enabled ? RGBA_TO_INT32(12, 19, 30, 185) : RGBA_TO_INT32(7, 16, 30, 228),
		!enabled ? RGBA_TO_INT32(10, 16, 26, 195) : RGBA_TO_INT32(10, 22, 40, 236));

	const unsigned long borderCol = !enabled
		? RGBA_TO_INT32(68, 86, 108, 95)
		: focused
			? RGBA_TO_INT32(114, 224, 255, 250)
			: hilited
				? RGBA_TO_INT32(98, 200, 255, 220)
				: RGBA_TO_INT32(76, 152, 222, 145);

	r2d.Add_Outline(rect, 1.0f, borderCol);
	if (focused)
	{
		r2d.Add_Quad(RectClass(x0 + 1.0f, y1 - 2.0f, x1 - 1.0f, y1 - 1.0f), RGBA_TO_INT32(82, 214, 255, 230));
	}
	r2d.Render();
}

void drawTacticalComboBox(GameWindow *window, WinInstanceData *instData)
{
	if (!window || !instData)
		return;

	ICoord2D origin, size;
	window->winGetScreenPosition(&origin.x, &origin.y);
	window->winGetSize(&size.x, &size.y);

	DisplayString *title = instData->getTextDisplayString();
	if (title && title->getTextLength() > 0)
	{
		if (title->getFont() != window->winGetFont())
			title->setFont(window->winGetFont());
		const Bool enabled = BitIsSet(window->winGetStatus(), WIN_STATUS_ENABLED);
		title->draw(
			origin.x + 2,
			origin.y,
			enabled ? GameMakeColor(226, 240, 252, 255) : GameMakeColor(136, 154, 174, 200),
			GameMakeColor(0, 8, 18, 210));
	}
}

void drawTacticalListBoxFrame(Int x, Int y, Int width, Int height, Bool hilited)
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

	// Soft elevation shadow so dropdown lists pop crisply over sibling controls
	{
		Render2DClass shadow;
		setupUntextured2D(shadow);
		shadow.Add_Quad(RectClass(x0 - 3.0f, y0, x1 + 4.0f, y1 + 6.0f), RGBA_TO_INT32(0, 4, 10, 128));
		shadow.Render();
	}

	drawFrostedQuad(
		rect,
		RGBA_TO_INT32(8, 19, 36, 236),
		RGBA_TO_INT32(5, 13, 25, 246));

	Render2DClass r2d;
	setupUntextured2D(r2d);
	r2d.Add_Outline(
		rect,
		1.0f,
		hilited ? RGBA_TO_INT32(106, 212, 255, 225) : RGBA_TO_INT32(84, 172, 244, 175));
	r2d.Render();
}

void drawTacticalListSelectionBar(Int x0, Int y0, Int x1, Int y1)
{
	if (x1 <= x0 || y1 <= y0)
		return;
	if (TheDisplay)
		TheDisplay->flush();

	const RectClass rect(
		static_cast<float>(x0),
		static_cast<float>(y0),
		static_cast<float>(x1),
		static_cast<float>(y1));

	Render2DClass r2d;
	setupUntextured2D(r2d);
	r2d.Add_Quad_HGradient(
		rect,
		RGBA_TO_INT32(34, 126, 202, 224),
		RGBA_TO_INT32(22, 82, 146, 175));
	r2d.Add_Quad(
		RectClass(rect.Left, rect.Top, rect.Left + 3.0f, rect.Bottom),
		RGBA_TO_INT32(94, 222, 255, 255));
	r2d.Render();
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
