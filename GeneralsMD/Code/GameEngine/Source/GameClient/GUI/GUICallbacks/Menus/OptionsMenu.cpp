/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: OptionsMenu.cpp //////////////////////////////////////////////////////////////////////////
// Author: Colin Day, October 2001
// Description: options menu window callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "gamespy/ghttp/ghttp.h"

#include "Common/AudioAffect.h"
#include "Common/AudioSettings.h"
#include "Common/FramePacer.h"
#include "Common/FrameRateLimit.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/OptionPreferences.h"
#include "Common/PlayerList.h"
#include "Common/GameLOD.h"
#include "Common/Recorder.h"
#include "Common/Registry.h"
#include "Common/version.h"

#include "GameClient/ClientInstance.h"
#include "GameClient/ControlBar.h"
#include "GameClient/GameClient.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GameFont.h"
#include "GameClient/HeaderTemplate.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Mouse.h"
#include "GameClient/GameText.h"
#include "GameClient/Display.h"
#include "GameClient/IMEManager.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GlobalLanguage.h"
#include "GameNetwork/FirewallHelper.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"
#include "WWDownload/Registry.h"
#include "GameClient/MessageBox.h"

#include "WW3D2/ww3d.h"
#include "WW3D2/texturefilter.h"

// This is for non-RC builds only!!!
#define VERBOSE_VERSION L"Release"



static NameKeyType		comboBoxOnlineIPID	= NAMEKEY_INVALID;
static GameWindow *		comboBoxOnlineIP		= nullptr;

static NameKeyType		comboBoxLANIPID	= NAMEKEY_INVALID;
static GameWindow *		comboBoxLANIP		= nullptr;

static NameKeyType    comboBoxAntiAliasingID   = NAMEKEY_INVALID;
static GameWindow *   comboBoxAntiAliasing     = nullptr;

static NameKeyType    comboBoxResolutionID      = NAMEKEY_INVALID;
static GameWindow *   comboBoxResolution       = nullptr;

static NameKeyType    comboBoxDetailID      = NAMEKEY_INVALID;
static GameWindow *   comboBoxDetail        = nullptr;

static NameKeyType		checkAlternateMouseID	= NAMEKEY_INVALID;
static GameWindow *		checkAlternateMouse		= nullptr;

static NameKeyType		checkRetaliationID	= NAMEKEY_INVALID;
static GameWindow *		checkRetaliation		= nullptr;

static NameKeyType		checkDoubleClickAttackMoveID	= NAMEKEY_INVALID;
static GameWindow *		checkDoubleClickAttackMove		= nullptr;

static NameKeyType		sliderScrollSpeedID	= NAMEKEY_INVALID;
static GameWindow *		sliderScrollSpeed		= nullptr;

static NameKeyType    checkLanguageFilterID = NAMEKEY_INVALID;
static GameWindow *   checkLanguageFilter   = nullptr;

static NameKeyType		checkUseCameraID		= NAMEKEY_INVALID;
static GameWindow *		checkUseCamera			= nullptr;

static NameKeyType		checkSaveCameraID		= NAMEKEY_INVALID;
static GameWindow *		checkSaveCamera			= nullptr;

static NameKeyType		checkDrawAnchorID		= NAMEKEY_INVALID;
static GameWindow *		checkDrawAnchor			= nullptr;

static NameKeyType		checkMoveAnchorID		= NAMEKEY_INVALID;
static GameWindow *		checkMoveAnchor			= nullptr;

static NameKeyType		buttonFirewallRefreshID	= NAMEKEY_INVALID;
static GameWindow *		buttonFirewallRefresh		= nullptr;
//
//static NameKeyType    checkAudioHardwareID = NAMEKEY_INVALID;
//static GameWindow *   checkAudioHardware   = nullptr;
//
//static NameKeyType    checkAudioSurroundID = NAMEKEY_INVALID;
//static GameWindow *   checkAudioSurround   = nullptr;
////volume controls
//
static NameKeyType    sliderMusicVolumeID = NAMEKEY_INVALID;
static GameWindow *   sliderMusicVolume   = nullptr;

static NameKeyType    sliderSFXVolumeID = NAMEKEY_INVALID;
static GameWindow *   sliderSFXVolume   = nullptr;

static NameKeyType    sliderVoiceVolumeID = NAMEKEY_INVALID;
static GameWindow *   sliderVoiceVolume   = nullptr;

static NameKeyType    sliderGammaID = NAMEKEY_INVALID;
static GameWindow *   sliderGamma = nullptr;

//Advanced Options Screen
static NameKeyType    WinAdvancedDisplayID      = NAMEKEY_INVALID;
static GameWindow *   WinAdvancedDisplay				= nullptr;

static NameKeyType    ButtonAdvancedAcceptID      = NAMEKEY_INVALID;
static GameWindow *   ButtonAdvancedAccept				= nullptr;

static NameKeyType    ButtonAdvancedCancelID      = NAMEKEY_INVALID;
static GameWindow *   ButtonAdvancedCancel				= nullptr;

static NameKeyType    sliderTextureResolutionID = NAMEKEY_INVALID;
static GameWindow *   sliderTextureResolution = nullptr;

static NameKeyType    sliderParticleCapID = NAMEKEY_INVALID;
static GameWindow *   sliderParticleCap = nullptr;

static NameKeyType    check3DShadowsID = NAMEKEY_INVALID;
static GameWindow *   check3DShadows   = nullptr;

static NameKeyType    check2DShadowsID = NAMEKEY_INVALID;
static GameWindow *   check2DShadows   = nullptr;

static NameKeyType    checkCloudShadowsID = NAMEKEY_INVALID;
static GameWindow *   checkCloudShadows   = nullptr;

static NameKeyType    checkGroundLightingID = NAMEKEY_INVALID;
static GameWindow *   checkGroundLighting   = nullptr;

static NameKeyType    checkSmoothWaterID = NAMEKEY_INVALID;
static GameWindow *   checkSmoothWater   = nullptr;

static NameKeyType    checkBuildingOcclusionID = NAMEKEY_INVALID;
static GameWindow *   checkBuildingOcclusion   = nullptr;

static NameKeyType    checkPropsID = NAMEKEY_INVALID;
static GameWindow *   checkProps   = nullptr;

static NameKeyType    checkExtraAnimationsID = NAMEKEY_INVALID;
static GameWindow *   checkExtraAnimations   = nullptr;

static NameKeyType    checkNoDynamicLodID = NAMEKEY_INVALID;
static GameWindow *   checkNoDynamicLod   = nullptr;

static NameKeyType    checkUnlockFpsID = NAMEKEY_INVALID;
static GameWindow *   checkUnlockFps   = nullptr;

static NameKeyType    checkHeatEffectsID = NAMEKEY_INVALID;
static GameWindow *   checkHeatEffects   = nullptr;

/*

static NameKeyType    radioHighID = NAMEKEY_INVALID;
static GameWindow *   radioHigh   = nullptr;
static NameKeyType    radioMediumID = NAMEKEY_INVALID;
static GameWindow *   radioMedium   = nullptr;
static NameKeyType    radioLowID = NAMEKEY_INVALID;
static GameWindow *   radioLow   = nullptr;

*/

DisplaySettings oldDispSettings, newDispSettings;
Bool dispChanged = FALSE;
extern Int timer;
extern void DoResolutionDialog();
static Bool ignoreSelected = FALSE;
WindowLayout *OptionsLayout = nullptr;

static OptionPreferences *pref = nullptr;

// -------------------------------------------------------------------------------------------------
// ZeroHourReloaded Modern 4-Tabbed Settings Menu & Vulkan 1.3 Remaster Controls
// -------------------------------------------------------------------------------------------------
enum ZhrOptionsTab
{
	ZHR_TAB_GRAPHICS = 0,
	ZHR_TAB_AUDIO,
	ZHR_TAB_CONTROLS,
	ZHR_TAB_NETWORK,
	ZHR_TAB_COUNT
};

static Int s_zhrActiveTab = ZHR_TAB_GRAPHICS;

// Snapshot of Vulkan Remaster settings on menu open (used to revert live previews on Cancel)
static Bool s_zhrHasVulkan = FALSE;
static UnsignedInt s_zhrSavedMode = 1u;
static Bool s_zhrSavedPbr = TRUE;
static Bool s_zhrSavedPostFx = TRUE;
static Bool s_zhrSavedRemasterTex = TRUE;
static Bool s_zhrSavedMeshSmooth = TRUE;

static UnsignedInt s_zhrCurMode = 1u;
static Bool s_zhrCurPbr = TRUE;
static Bool s_zhrCurPostFx = TRUE;
static Bool s_zhrCurRemasterTex = TRUE;
static Bool s_zhrCurMeshSmooth = TRUE;

static UnsignedInt s_zhrSavedMaxFps = 144u;
static UnsignedInt s_zhrCurMaxFps = 144u;

static Real s_zhrSavedUiScale = 0.0f;
static Real s_zhrCurUiScale = 0.0f;

// Dynamic ZHR controls (owned as children of OptionsMenuParent; destroyed with layout)
static NameKeyType s_zhrTabIDs[ZHR_TAB_COUNT] = { NAMEKEY_INVALID, NAMEKEY_INVALID, NAMEKEY_INVALID, NAMEKEY_INVALID };
static GameWindow *s_zhrTabButtons[ZHR_TAB_COUNT] = { nullptr, nullptr, nullptr, nullptr };

static GameWindow *s_zhrTitleLabel = nullptr;
static GameWindow *s_zhrHeaderDivider = nullptr;
static GameWindow *s_zhrFooterDivider = nullptr;

// Graphics tab dynamic widgets
static GameWindow *s_zhrCardDisplay = nullptr;
static GameWindow *s_zhrCardVulkan = nullptr;
static GameWindow *s_zhrLblDisplayHdr = nullptr;
static GameWindow *s_zhrLblResolution = nullptr;
static GameWindow *s_zhrLblDetail = nullptr;
static GameWindow *s_zhrLblMaxFps = nullptr;
static NameKeyType s_zhrComboMaxFpsID = NAMEKEY_INVALID;
static GameWindow *s_zhrComboMaxFps = nullptr;
static GameWindow *s_zhrLblUiScale = nullptr;
static NameKeyType s_zhrComboUiScaleID = NAMEKEY_INVALID;
static GameWindow *s_zhrComboUiScale = nullptr;
static GameWindow *s_zhrLblBrightness = nullptr;
static NameKeyType s_zhrBtnCustomDetailID = NAMEKEY_INVALID;
static GameWindow *s_zhrBtnCustomDetail = nullptr;

static GameWindow *s_zhrLblVulkanHdr = nullptr;
static GameWindow *s_zhrLblVisualMode = nullptr;
static NameKeyType s_zhrComboVisualModeID = NAMEKEY_INVALID;
static GameWindow *s_zhrComboVisualMode = nullptr;
static NameKeyType s_zhrCheckPbrID = NAMEKEY_INVALID;
static GameWindow *s_zhrCheckPbr = nullptr;
static NameKeyType s_zhrCheckPostFxID = NAMEKEY_INVALID;
static GameWindow *s_zhrCheckPostFx = nullptr;
static NameKeyType s_zhrCheckRemasterTexID = NAMEKEY_INVALID;
static GameWindow *s_zhrCheckRemasterTex = nullptr;
static NameKeyType s_zhrCheckMeshSmoothID = NAMEKEY_INVALID;
static GameWindow *s_zhrCheckMeshSmooth = nullptr;
static GameWindow *s_zhrLblVulkanHint = nullptr;

// Audio tab dynamic widgets
static GameWindow *s_zhrCardAudio = nullptr;
static GameWindow *s_zhrLblAudioHdr = nullptr;
static GameWindow *s_zhrLblMusicVol = nullptr;
static GameWindow *s_zhrLblSfxVol = nullptr;
static GameWindow *s_zhrLblVoiceVol = nullptr;

// Controls tab dynamic widgets
static GameWindow *s_zhrCardControls = nullptr;
static GameWindow *s_zhrLblControlsHdr = nullptr;
static GameWindow *s_zhrLblScrollSpeed = nullptr;

// Network tab dynamic widgets
static GameWindow *s_zhrCardNetwork = nullptr;
static GameWindow *s_zhrLblNetworkHdr = nullptr;
static GameWindow *s_zhrLblOnlineIP = nullptr;
static GameWindow *s_zhrLblLanIP = nullptr;
static GameWindow *s_zhrLblFirewallPort = nullptr;
static GameWindow *s_zhrTextEntryFirewallPort = nullptr;

// Existing .wnd intermediate sub-containers (re-aligned to 0,0 inside OptionsMenuParentOld)
static GameWindow *s_zhrModalBox = nullptr;
static GameWindow *s_zhrVideoGroup = nullptr;
static GameWindow *s_zhrAudioGroup = nullptr;
static GameWindow *s_zhrControlsGroup = nullptr;
static GameWindow *s_zhrNetworkGroup = nullptr;

static void clearZhrOptionsWidgets()
{
	for (Int i = 0; i < ZHR_TAB_COUNT; ++i)
		s_zhrTabButtons[i] = nullptr;

	s_zhrTitleLabel = nullptr;
	s_zhrHeaderDivider = nullptr;
	s_zhrFooterDivider = nullptr;

	s_zhrCardDisplay = nullptr;
	s_zhrCardVulkan = nullptr;
	s_zhrLblDisplayHdr = nullptr;
	s_zhrLblResolution = nullptr;
	s_zhrLblDetail = nullptr;
	s_zhrLblMaxFps = nullptr;
	s_zhrComboMaxFps = nullptr;
	s_zhrLblUiScale = nullptr;
	s_zhrComboUiScale = nullptr;
	s_zhrLblBrightness = nullptr;
	s_zhrBtnCustomDetail = nullptr;

	s_zhrLblVulkanHdr = nullptr;
	s_zhrLblVisualMode = nullptr;
	s_zhrComboVisualMode = nullptr;
	s_zhrCheckPbr = nullptr;
	s_zhrCheckPostFx = nullptr;
	s_zhrCheckRemasterTex = nullptr;
	s_zhrCheckMeshSmooth = nullptr;
	s_zhrLblVulkanHint = nullptr;

	s_zhrCardAudio = nullptr;
	s_zhrLblAudioHdr = nullptr;
	s_zhrLblMusicVol = nullptr;
	s_zhrLblSfxVol = nullptr;
	s_zhrLblVoiceVol = nullptr;

	s_zhrCardControls = nullptr;
	s_zhrLblControlsHdr = nullptr;
	s_zhrLblScrollSpeed = nullptr;

	s_zhrCardNetwork = nullptr;
	s_zhrLblNetworkHdr = nullptr;
	s_zhrLblOnlineIP = nullptr;
	s_zhrLblLanIP = nullptr;
	s_zhrLblFirewallPort = nullptr;
	s_zhrTextEntryFirewallPort = nullptr;

	s_zhrModalBox = nullptr;
	s_zhrVideoGroup = nullptr;
	s_zhrAudioGroup = nullptr;
	s_zhrControlsGroup = nullptr;
	s_zhrNetworkGroup = nullptr;
}

static Int zhrModeToComboIdx(UnsignedInt mode)
{
	// Combo order: 0 = Remastered (Vulkan 1.3), 1 = Split-Screen A/B, 2 = Classic 2003
	if (mode == 2u) return 1;
	if (mode == 0u) return 2;
	return 0;
}

static UnsignedInt zhrComboIdxToMode(Int idx)
{
	if (idx == 1) return 2u;
	if (idx == 2) return 0u;
	return 1u;
}

static void syncVulkanControlsFromState()
{
	if (!s_zhrHasVulkan)
		return;
	const Bool prevIgnore = ignoreSelected;
	ignoreSelected = TRUE;
	if (s_zhrComboVisualMode)
		GadgetComboBoxSetSelectedPos(s_zhrComboVisualMode, zhrModeToComboIdx(s_zhrCurMode));
	if (s_zhrCheckPbr)
		GadgetCheckBoxSetChecked(s_zhrCheckPbr, s_zhrCurPbr);
	if (s_zhrCheckPostFx)
		GadgetCheckBoxSetChecked(s_zhrCheckPostFx, s_zhrCurPostFx);
	if (s_zhrCheckRemasterTex)
		GadgetCheckBoxSetChecked(s_zhrCheckRemasterTex, s_zhrCurRemasterTex);
	if (s_zhrCheckMeshSmooth)
		GadgetCheckBoxSetChecked(s_zhrCheckMeshSmooth, s_zhrCurMeshSmooth);
	ignoreSelected = prevIgnore;
}

static void applyLiveVulkanRemasterSettings()
{
	if (!s_zhrHasVulkan || !TheDisplay)
		return;
	TheDisplay->setVulkanRemasterSettings(
		s_zhrCurMode,
		s_zhrCurPbr,
		s_zhrCurPostFx,
		s_zhrCurRemasterTex,
		s_zhrCurMeshSmooth);
}

static const UnsignedInt s_zhrFpsChoices[] = {
	30u, 60u, 120u, 144u, 240u, RenderFpsPreset::UncappedFpsValue
};

static Int zhrFpsToComboIdx(UnsignedInt fps)
{
	if (fps >= RenderFpsPreset::UncappedFpsValue) return 5;
	if (fps >= 200u) return 4;
	if (fps >= 132u) return 3;
	if (fps >= 90u) return 2;
	if (fps >= 45u) return 1;
	return 0;
}

static UnsignedInt zhrComboIdxToFps(Int idx)
{
	if (idx < 0 || idx >= 6) return 144u;
	return s_zhrFpsChoices[idx];
}

static void applyLiveMaxRenderFps(UnsignedInt maxFps)
{
	if (!TheFramePacer || !TheWritableGlobalData)
		return;
	if (maxFps >= RenderFpsPreset::UncappedFpsValue)
	{
		TheWritableGlobalData->m_framesPerSecondLimit = RenderFpsPreset::UncappedFpsValue;
		TheWritableGlobalData->m_useFpsLimit = FALSE;
		TheFramePacer->setFramesPerSecondLimit(RenderFpsPreset::UncappedFpsValue);
		TheFramePacer->setLogicTimeScaleFps(LOGICFRAMES_PER_SECOND);
		TheFramePacer->enableLogicTimeScale(TRUE);
	}
	else
	{
		if (maxFps < LOGICFRAMES_PER_SECOND)
			maxFps = LOGICFRAMES_PER_SECOND;
		TheWritableGlobalData->m_framesPerSecondLimit = (Int)maxFps;
		TheWritableGlobalData->m_useFpsLimit = TRUE;
		TheFramePacer->setFramesPerSecondLimit((Int)maxFps);
		TheFramePacer->setLogicTimeScaleFps(LOGICFRAMES_PER_SECOND);
		TheFramePacer->enableLogicTimeScale(maxFps > LOGICFRAMES_PER_SECOND);
	}
}

static const Real s_zhrUiScaleChoices[] = {
	0.0f, 0.65f, 0.75f, 0.85f, 1.00f, -1.0f
};

static Int zhrUiScaleToComboIdx(Real scale)
{
	if (scale < -0.5f) return 5;   // Classic (Stretched)
	if (scale <= 0.01f) return 0;  // Auto (DPI-Aware)
	if (scale <= 0.70f) return 1;  // 65% (Compact)
	if (scale <= 0.80f) return 2;  // 75%
	if (scale <= 0.92f) return 3;  // 85%
	return 4;                      // 100% (4:3 Fit)
}

static Real zhrComboIdxToUiScale(Int idx)
{
	if (idx < 0 || idx >= 6) return 0.0f;
	return s_zhrUiScaleChoices[idx];
}

static void placeComboBox(GameWindow *combo, Int x, Int y, Int w, Int h)
{
	if (!combo)
		return;
	GadgetComboBoxHideList(combo);
	combo->winSetPosition(x, y);
	combo->winSetSize(w, h);

	ComboBoxData *cb = static_cast<ComboBoxData *>(combo->winGetUserData());
	if (cb)
	{
		const Int dropW = (h > 24) ? h : 24;
		if (cb->dropDownButton)
		{
			cb->dropDownButton->winSetPosition(w - dropW, 0);
			cb->dropDownButton->winSetSize(dropW, h);
		}
		if (cb->editBox)
		{
			cb->editBox->winSetPosition(0, 0);
			cb->editBox->winSetSize(w - dropW, h);
		}
		if (cb->listBox)
		{
			cb->listBox->winSetPosition(0, h);
			cb->listBox->winSetSize(w, h);
		}
	}
}

static void placeSlider(GameWindow *slider, Int x, Int y, Int w, Int h)
{
	if (!slider)
		return;
	const Int curPos = GadgetSliderGetPosition(slider);
	slider->winSetPosition(x, y);
	slider->winSetSize(w, h);
	SliderData *s = static_cast<SliderData *>(slider->winGetUserData());
	if (s && s->maxVal > s->minVal)
	{
		s->numTicks = static_cast<Real>(w - HORIZONTAL_SLIDER_THUMB_WIDTH) / static_cast<Real>(s->maxVal - s->minVal);
	}
	GadgetSliderSetPosition(slider, curPos);
}

static GameWindow *createZhrPanel(GameWindow *parent, const char *name, Int x, Int y, Int w, Int h)
{
	WinInstanceData inst;
	inst.init();
	inst.m_decoratedNameString = name;
	inst.m_id = TheNameKeyGenerator->nameToKey(name);
	inst.m_style = GWS_USER_WINDOW;
	inst.m_enabledDrawData[0].color = GameMakeColor(8, 18, 33, 185);
	inst.m_enabledDrawData[0].borderColor = GameMakeColor(82, 162, 234, 110);
	GameWindow *win = TheWindowManager->winCreate(
		parent,
		WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT | WIN_STATUS_NO_FOCUS,
		x, y, w, h,
		nullptr,
		&inst);
	return win;
}

static GameWindow *createZhrLabel(
	GameWindow *parent,
	const char *name,
	const UnicodeString &text,
	Int x, Int y, Int w, Int h,
	GameFont *font,
	Color textColor,
	Bool centered = FALSE)
{
	WinInstanceData inst;
	inst.init();
	inst.m_decoratedNameString = name;
	inst.m_id = TheNameKeyGenerator->nameToKey(name);
	inst.m_style = GWS_STATIC_TEXT;
	inst.m_font = font;
	inst.m_enabledText.color = textColor;
	inst.m_enabledText.borderColor = GameMakeColor(0, 8, 18, 220);
	inst.m_disabledText.color = GameMakeColor(136, 154, 174, 210);
	inst.m_disabledText.borderColor = GameMakeColor(0, 6, 14, 180);
	inst.m_hiliteText.color = textColor;
	inst.m_hiliteText.borderColor = GameMakeColor(0, 8, 18, 220);

	TextData tData;
	tData.text = nullptr;
	tData.centered = centered;
	tData.centeredVertically = TRUE;
	tData.leftMargin = 0;
	tData.topMargin = 0;

	GameWindow *lbl = TheWindowManager->gogoGadgetStaticText(
		parent,
		WIN_STATUS_ENABLED | WIN_STATUS_NO_INPUT | WIN_STATUS_NO_FOCUS,
		x, y, w, h,
		&inst,
		&tData,
		font,
		FALSE);
	if (lbl)
	{
		lbl->winSetEnabledColor(0, WIN_COLOR_UNDEFINED);
		lbl->winSetEnabledBorderColor(0, WIN_COLOR_UNDEFINED);
		lbl->winSetDisabledColor(0, WIN_COLOR_UNDEFINED);
		lbl->winSetDisabledBorderColor(0, WIN_COLOR_UNDEFINED);
		lbl->winSetHiliteColor(0, WIN_COLOR_UNDEFINED);
		lbl->winSetHiliteBorderColor(0, WIN_COLOR_UNDEFINED);
		lbl->winSetEnabledTextColors(textColor, GameMakeColor(0, 8, 18, 220));
		GadgetStaticTextSetFont(lbl, font);
		GadgetStaticTextSetText(lbl, text);
	}
	return lbl;
}

static GameWindow *createZhrButton(
	GameWindow *parent,
	const char *name,
	const UnicodeString &text,
	Int x, Int y, Int w, Int h,
	GameFont *font,
	NameKeyType &outId)
{
	WinInstanceData inst;
	inst.init();
	inst.m_decoratedNameString = name;
	outId = TheNameKeyGenerator->nameToKey(name);
	inst.m_id = outId;
	inst.m_style = GWS_PUSH_BUTTON | GWS_MOUSE_TRACK;
	inst.m_font = font;
	inst.m_enabledText.color = GameMakeColor(232, 244, 255, 255);
	inst.m_enabledText.borderColor = GameMakeColor(0, 8, 18, 220);
	inst.m_hiliteText.color = GameMakeColor(255, 255, 255, 255);
	inst.m_hiliteText.borderColor = GameMakeColor(0, 12, 26, 235);
	inst.m_disabledText.color = GameMakeColor(136, 152, 170, 210);
	inst.m_disabledText.borderColor = GameMakeColor(0, 6, 14, 150);

	GameWindow *btn = TheWindowManager->gogoGadgetPushButton(
		parent,
		WIN_STATUS_ENABLED | WIN_STATUS_TAB_STOP,
		x, y, w, h,
		&inst,
		font,
		FALSE);
	if (btn)
	{
		// Clean dark-navy classic fallback colors (used only when ZhrMenuStyle is OFF in Classic 2003 mode)
		GadgetButtonSetEnabledColor(btn, GameMakeColor(14, 30, 54, 220));
		GadgetButtonSetEnabledBorderColor(btn, GameMakeColor(72, 148, 220, 210));
		GadgetButtonSetEnabledSelectedColor(btn, GameMakeColor(28, 76, 132, 240));
		GadgetButtonSetEnabledSelectedBorderColor(btn, GameMakeColor(112, 206, 255, 245));
		GadgetButtonSetHiliteColor(btn, GameMakeColor(24, 58, 102, 235));
		GadgetButtonSetHiliteBorderColor(btn, GameMakeColor(110, 204, 255, 245));
		GadgetButtonSetHiliteSelectedColor(btn, GameMakeColor(34, 88, 150, 245));
		GadgetButtonSetHiliteSelectedBorderColor(btn, GameMakeColor(140, 224, 255, 255));
		GadgetButtonSetDisabledColor(btn, GameMakeColor(18, 26, 38, 175));
		GadgetButtonSetDisabledBorderColor(btn, GameMakeColor(64, 82, 104, 130));
		btn->winSetFont(font);
		GadgetButtonSetText(btn, text);
	}
	return btn;
}

static GameWindow *createZhrCheckBox(
	GameWindow *parent,
	const char *name,
	const UnicodeString &text,
	Int x, Int y, Int w, Int h,
	GameFont *font,
	NameKeyType &outId)
{
	WinInstanceData inst;
	inst.init();
	inst.m_decoratedNameString = name;
	outId = TheNameKeyGenerator->nameToKey(name);
	inst.m_id = outId;
	inst.m_style = GWS_CHECK_BOX | GWS_MOUSE_TRACK;
	inst.m_font = font;
	inst.m_enabledText.color = GameMakeColor(226, 240, 252, 255);
	inst.m_enabledText.borderColor = GameMakeColor(0, 8, 18, 220);
	inst.m_hiliteText.color = GameMakeColor(255, 255, 255, 255);
	inst.m_hiliteText.borderColor = GameMakeColor(0, 12, 26, 235);
	inst.m_disabledText.color = GameMakeColor(136, 152, 170, 210);
	inst.m_disabledText.borderColor = GameMakeColor(0, 6, 14, 150);

	GameWindow *cb = TheWindowManager->gogoGadgetCheckbox(
		parent,
		WIN_STATUS_ENABLED | WIN_STATUS_TAB_STOP,
		x, y, w, h,
		&inst,
		font,
		FALSE);
	if (cb)
	{
		// Clean classic dark-navy fallback colors (no red debug placeholder in Classic 2003 mode)
		GadgetCheckBoxSetEnabledColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetEnabledBorderColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetEnabledUncheckedBoxColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetEnabledUncheckedBoxBorderColor(cb, GameMakeColor(82, 162, 234, 220));
		GadgetCheckBoxSetEnabledCheckedBoxColor(cb, GameMakeColor(96, 214, 255, 255));
		GadgetCheckBoxSetEnabledCheckedBoxBorderColor(cb, GameMakeColor(120, 222, 255, 255));
		GadgetCheckBoxSetHiliteColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetHiliteBorderColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetHiliteUncheckedBoxColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetHiliteUncheckedBoxBorderColor(cb, GameMakeColor(140, 226, 255, 255));
		GadgetCheckBoxSetHiliteCheckedBoxColor(cb, GameMakeColor(140, 236, 255, 255));
		GadgetCheckBoxSetHiliteCheckedBoxBorderColor(cb, GameMakeColor(180, 244, 255, 255));
		GadgetCheckBoxSetDisabledColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetDisabledBorderColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetDisabledUncheckedBoxColor(cb, WIN_COLOR_UNDEFINED);
		GadgetCheckBoxSetDisabledUncheckedBoxBorderColor(cb, GameMakeColor(72, 92, 114, 140));
		GadgetCheckBoxSetDisabledCheckedBoxColor(cb, GameMakeColor(92, 118, 142, 180));
		GadgetCheckBoxSetDisabledCheckedBoxBorderColor(cb, GameMakeColor(92, 118, 142, 180));
		cb->winSetFont(font);
		GadgetCheckBoxSetText(cb, text);
	}
	return cb;
}

static GameWindow *createZhrComboBox(
	GameWindow *parent,
	const char *name,
	Int x, Int y, Int w, Int h,
	GameFont *font,
	NameKeyType &outId)
{
	WinInstanceData inst;
	inst.init();
	inst.m_decoratedNameString = name;
	outId = TheNameKeyGenerator->nameToKey(name);
	inst.m_id = outId;
	inst.m_style = GWS_COMBO_BOX | GWS_MOUSE_TRACK;
	inst.m_font = font;
	inst.m_enabledText.color = GameMakeColor(236, 246, 255, 255);
	inst.m_enabledText.borderColor = GameMakeColor(0, 8, 18, 220);
	inst.m_hiliteText.color = GameMakeColor(255, 255, 255, 255);
	inst.m_hiliteText.borderColor = GameMakeColor(0, 12, 26, 235);
	inst.m_disabledText.color = GameMakeColor(136, 154, 174, 210);
	inst.m_disabledText.borderColor = GameMakeColor(0, 6, 14, 180);

	ComboBoxData cbTemplate;
	memset(&cbTemplate, 0, sizeof(cbTemplate));
	cbTemplate.isEditable = FALSE;
	cbTemplate.maxDisplay = 4;
	cbTemplate.maxChars = 64;
	cbTemplate.asciiOnly = FALSE;
	cbTemplate.lettersAndNumbersOnly = FALSE;

	// gogoGadgetComboBox deletes entryData and listboxData after copying them internally.
	cbTemplate.entryData = NEW EntryData;
	memset(cbTemplate.entryData, 0, sizeof(EntryData));
	cbTemplate.entryData->maxTextLen = 64;
	cbTemplate.entryData->drawTextFromStart = TRUE;

	cbTemplate.listboxData = NEW ListboxData;
	memset(cbTemplate.listboxData, 0, sizeof(ListboxData));
	cbTemplate.listboxData->listLength = 8;
	cbTemplate.listboxData->columns = 1;
	cbTemplate.listboxData->scrollBar = FALSE;
	cbTemplate.listboxData->multiSelect = FALSE;
	cbTemplate.listboxData->forceSelect = TRUE;
	cbTemplate.listboxData->audioFeedback = TRUE;

	GameWindow *combo = TheWindowManager->gogoGadgetComboBox(
		parent,
		WIN_STATUS_ENABLED | WIN_STATUS_TAB_STOP,
		x, y, w, h,
		&inst,
		&cbTemplate,
		font,
		FALSE);
	if (combo)
	{
		GadgetComboBoxSetColors(
			combo,
			GameMakeColor(10, 22, 40, 235), GameMakeColor(72, 148, 220, 210),
			GameMakeColor(28, 86, 148, 240), GameMakeColor(112, 206, 255, 240),
			GameMakeColor(14, 22, 34, 185), GameMakeColor(64, 82, 104, 130),
			GameMakeColor(24, 42, 64, 185), GameMakeColor(76, 98, 120, 140),
			GameMakeColor(16, 36, 64, 240), GameMakeColor(110, 206, 255, 245),
			GameMakeColor(34, 106, 178, 245), GameMakeColor(136, 224, 255, 255));
		GadgetComboBoxSetFont(combo, font);
		placeComboBox(combo, x, y, w, h);
	}
	return combo;
}

static void showZhrWidget(GameWindow *win, Bool show)
{
	if (win)
		win->winHide(!show);
}

static void selectZhrOptionsTab(Int tabIndex)
{
	if (tabIndex < 0 || tabIndex >= ZHR_TAB_COUNT)
		tabIndex = ZHR_TAB_GRAPHICS;
	s_zhrActiveTab = tabIndex;

	// Close any open dropdown list before switching tabs
	if (comboBoxResolution) GadgetComboBoxHideList(comboBoxResolution);
	if (comboBoxDetail) GadgetComboBoxHideList(comboBoxDetail);
	if (s_zhrComboMaxFps) GadgetComboBoxHideList(s_zhrComboMaxFps);
	if (s_zhrComboUiScale) GadgetComboBoxHideList(s_zhrComboUiScale);
	if (s_zhrComboVisualMode) GadgetComboBoxHideList(s_zhrComboVisualMode);
	if (comboBoxOnlineIP) GadgetComboBoxHideList(comboBoxOnlineIP);
	if (comboBoxLANIP) GadgetComboBoxHideList(comboBoxLANIP);

	for (Int i = 0; i < ZHR_TAB_COUNT; ++i)
	{
		if (s_zhrTabButtons[i])
		{
			WinInstanceData *inst = s_zhrTabButtons[i]->winGetInstanceData();
			if (inst)
			{
				if (i == tabIndex)
					BitSet(inst->m_state, WIN_STATE_SELECTED);
				else
					BitClear(inst->m_state, WIN_STATE_SELECTED);
			}
		}
	}

	const Bool isGfx = (tabIndex == ZHR_TAB_GRAPHICS);
	const Bool isAud = (tabIndex == ZHR_TAB_AUDIO);
	const Bool isCtl = (tabIndex == ZHR_TAB_CONTROLS);
	const Bool isNet = (tabIndex == ZHR_TAB_NETWORK);

	// Graphics tab
	showZhrWidget(s_zhrVideoGroup, isGfx);
	showZhrWidget(s_zhrCardDisplay, isGfx);
	showZhrWidget(s_zhrCardVulkan, isGfx);
	showZhrWidget(s_zhrLblDisplayHdr, isGfx);
	showZhrWidget(s_zhrLblResolution, isGfx);
	showZhrWidget(comboBoxResolution, isGfx);
	showZhrWidget(s_zhrLblDetail, isGfx);
	showZhrWidget(comboBoxDetail, isGfx);
	showZhrWidget(s_zhrBtnCustomDetail, isGfx);
	showZhrWidget(s_zhrLblMaxFps, isGfx);
	showZhrWidget(s_zhrComboMaxFps, isGfx);
	showZhrWidget(s_zhrLblUiScale, isGfx);
	showZhrWidget(s_zhrComboUiScale, isGfx);
	showZhrWidget(s_zhrLblBrightness, isGfx);
	showZhrWidget(sliderGamma, isGfx);

	showZhrWidget(s_zhrLblVulkanHdr, isGfx);
	showZhrWidget(s_zhrLblVisualMode, isGfx);
	showZhrWidget(s_zhrComboVisualMode, isGfx && s_zhrHasVulkan);
	showZhrWidget(s_zhrCheckPbr, isGfx && s_zhrHasVulkan);
	showZhrWidget(s_zhrCheckPostFx, isGfx && s_zhrHasVulkan);
	showZhrWidget(s_zhrCheckRemasterTex, isGfx && s_zhrHasVulkan);
	showZhrWidget(s_zhrCheckMeshSmooth, isGfx && s_zhrHasVulkan);
	showZhrWidget(s_zhrLblVulkanHint, isGfx);

	// Audio tab
	showZhrWidget(s_zhrAudioGroup, isAud);
	showZhrWidget(s_zhrCardAudio, isAud);
	showZhrWidget(s_zhrLblAudioHdr, isAud);
	showZhrWidget(s_zhrLblMusicVol, isAud);
	showZhrWidget(sliderMusicVolume, isAud);
	showZhrWidget(s_zhrLblSfxVol, isAud);
	showZhrWidget(sliderSFXVolume, isAud);
	showZhrWidget(s_zhrLblVoiceVol, isAud);
	showZhrWidget(sliderVoiceVolume, isAud);

	// Controls tab
	showZhrWidget(s_zhrControlsGroup, isCtl);
	showZhrWidget(s_zhrCardControls, isCtl);
	showZhrWidget(s_zhrLblControlsHdr, isCtl);
	showZhrWidget(checkAlternateMouse, isCtl);
	showZhrWidget(checkDoubleClickAttackMove, isCtl);
	showZhrWidget(checkRetaliation, isCtl);
	showZhrWidget(s_zhrLblScrollSpeed, isCtl);
	showZhrWidget(sliderScrollSpeed, isCtl);

	// Network tab
	showZhrWidget(s_zhrNetworkGroup, isNet);
	showZhrWidget(s_zhrCardNetwork, isNet);
	showZhrWidget(s_zhrLblNetworkHdr, isNet);
	showZhrWidget(s_zhrLblOnlineIP, isNet);
	showZhrWidget(comboBoxOnlineIP, isNet);
	showZhrWidget(s_zhrLblLanIP, isNet);
	showZhrWidget(comboBoxLANIP, isNet);
	showZhrWidget(s_zhrLblFirewallPort, isNet);
	showZhrWidget(s_zhrTextEntryFirewallPort, isNet);
	showZhrWidget(buttonFirewallRefresh, isNet);
}

static void buildModernTabbedOptionsLayout(GameWindow *parent)
{
	if (!parent || !TheDisplay || !TheWindowManager || !TheFontLibrary)
		return;

	const Int screenW = static_cast<Int>(TheDisplay->getWidth());
	const Int screenH = static_cast<Int>(TheDisplay->getHeight());

	// Center a clean DPI-aware proportional modal card aligned with ZeroHourReloaded's UI scale
	Real uiSx = 1.0f, uiSy = 1.0f, uiOx = 0.0f, uiOy = 0.0f;
	TheDisplay->getUiTransform(
		DEFAULT_DISPLAY_WIDTH,
		DEFAULT_DISPLAY_HEIGHT,
		Display::UI_ANCHOR_CENTER,
		uiSx, uiSy, uiOx, uiOy);
	const Int scaledBaseW = REAL_TO_INT_FLOOR(DEFAULT_DISPLAY_WIDTH * uiSx + 0.5f);
	const Int scaledBaseH = REAL_TO_INT_FLOOR(DEFAULT_DISPLAY_HEIGHT * uiSy + 0.5f);

	Int cardW = TheDisplay->isUiScaleClassicStretched()
		? static_cast<Int>(screenW * 0.84f)
		: static_cast<Int>(scaledBaseW * 1.14f);
	const Int maxAspectW = static_cast<Int>(scaledBaseH * 1.56f);
	if (cardW > maxAspectW) cardW = maxAspectW;
	if (cardW > static_cast<Int>(screenW * 0.92f)) cardW = static_cast<Int>(screenW * 0.92f);
	if (cardW < 760) cardW = (screenW > 760) ? 760 : (screenW - 24);

	Int cardH = TheDisplay->isUiScaleClassicStretched()
		? static_cast<Int>(screenH * 0.82f)
		: static_cast<Int>(scaledBaseH * 0.90f);
	if (cardH > static_cast<Int>(screenH * 0.90f)) cardH = static_cast<Int>(screenH * 0.90f);
	if (cardH < 500) cardH = (screenH > 500) ? 500 : (screenH - 24);

	const Int cardX = (screenW - cardW) / 2;
	const Int cardY = (screenH - cardH) / 2;

	// Fetch footer buttons, version label, and intermediate .wnd containers
	GameWindow *btnDefaults = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:ButtonDefaults"));
	GameWindow *btnAccept = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:ButtonAccept"));
	GameWindow *btnBack = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:ButtonBack"));
	GameWindow *lblVersion = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:LabelVersion"));
	s_zhrTextEntryFirewallPort = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:TextEntryFirewallPortOverride"));

	// OptionsMenuParentOld is the real modal container inside OptionsMenuParent
	s_zhrModalBox = TheWindowManager->winGetWindowFromId(nullptr, TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:OptionsMenuParentOld"));
	if (!s_zhrModalBox)
		s_zhrModalBox = (btnAccept && btnAccept->winGetParent()) ? btnAccept->winGetParent() : parent;

	s_zhrVideoGroup = comboBoxResolution ? comboBoxResolution->winGetParent() : nullptr;
	s_zhrAudioGroup = sliderVoiceVolume ? sliderVoiceVolume->winGetParent() : nullptr;
	s_zhrControlsGroup = sliderScrollSpeed ? sliderScrollSpeed->winGetParent() : nullptr;
	s_zhrNetworkGroup = comboBoxLANIP ? comboBoxLANIP->winGetParent() : nullptr;

	// Never treat the modal container itself as a tab-toggleable sub-group!
	if (s_zhrVideoGroup == s_zhrModalBox || s_zhrVideoGroup == parent) s_zhrVideoGroup = nullptr;
	if (s_zhrAudioGroup == s_zhrModalBox || s_zhrAudioGroup == parent) s_zhrAudioGroup = nullptr;
	if (s_zhrControlsGroup == s_zhrModalBox || s_zhrControlsGroup == parent) s_zhrControlsGroup = nullptr;
	if (s_zhrNetworkGroup == s_zhrModalBox || s_zhrNetworkGroup == parent) s_zhrNetworkGroup = nullptr;

	// If OptionsMenuParent is nested inside a full-screen .wnd root backdrop, pin that root to (0,0,screenW,screenH)
	// and mark it SEE_THRU so cardX/cardY are exact screen coordinates and no duplicate glass frame appears behind the modal.
	for (GameWindow *anc = parent->winGetParent(); anc != nullptr; anc = anc->winGetParent())
	{
		anc->winSetPosition(0, 0);
		anc->winSetSize(screenW, screenH);
		anc->winSetStatus(WIN_STATUS_SEE_THRU);
	}

	// Align OptionsMenuParent (outer) and OptionsMenuParentOld (frosted modal card)
	parent->winSetPosition(cardX, cardY);
	parent->winSetSize(cardW, cardH);

	if (s_zhrModalBox && s_zhrModalBox != parent)
	{
		s_zhrModalBox->winSetPosition(0, 0);
		s_zhrModalBox->winSetSize(cardW, cardH);
		s_zhrModalBox->winSetEnabledColor(0, GameMakeColor(8, 17, 30, 232));
		s_zhrModalBox->winSetEnabledBorderColor(0, GameMakeColor(72, 148, 220, 210));
		s_zhrModalBox->winHide(FALSE);
	}

	GameWindow *host = s_zhrModalBox ? s_zhrModalBox : parent;

	// Make the 4 legacy group containers full-card transparent coordinate-aligned overlays so
	// every existing .wnd control inside them aligns cleanly to (0, 0) of `host`!
	auto alignGroupContainer = [&](GameWindow *grp)
	{
		if (!grp || grp == host || grp == parent)
			return;
		grp->winSetPosition(0, 0);
		grp->winSetSize(cardW, cardH);
		grp->winSetStatus(WIN_STATUS_SEE_THRU);
		grp->winHide(FALSE);
	};
	alignGroupContainer(s_zhrVideoGroup);
	alignGroupContainer(s_zhrAudioGroup);
	alignGroupContainer(s_zhrControlsGroup);
	alignGroupContainer(s_zhrNetworkGroup);

	// Borrow already-DPI-scaled fonts directly from loaded .wnd gadgets so text scales identically at 720p..4K
	GameFont *bodyFont = checkAlternateMouse ? checkAlternateMouse->winGetFont() : nullptr;
	GameFont *buttonFont = btnAccept ? btnAccept->winGetFont() : bodyFont;
	GameFont *smallFont = lblVersion ? lblVersion->winGetFont() : bodyFont;
	GameFont *titleFont = buttonFont;
	GameFont *sectionFont = bodyFont;
	if (buttonFont)
	{
		GameFont *fTitle = TheFontLibrary->getFont(buttonFont->nameString, buttonFont->pointSize + 4, TRUE);
		if (fTitle) titleFont = fTitle;
	}
	if (bodyFont)
	{
		GameFont *fSec = TheFontLibrary->getFont(bodyFont->nameString, bodyFont->pointSize, TRUE);
		if (fSec) sectionFont = fSec;
	}

	// Helper: hide legacy unnamed/unused decorative siblings inside a container while preserving active widgets
	auto pruneLegacyChildren = [&](GameWindow *container)
	{
		if (!container)
			return;
		for (GameWindow *child = container->winGetChild(); child != nullptr; child = child->winGetNext())
		{
			const Bool keep =
				child == s_zhrModalBox ||
				child == s_zhrVideoGroup ||
				child == s_zhrAudioGroup ||
				child == s_zhrControlsGroup ||
				child == s_zhrNetworkGroup ||
				child == comboBoxResolution ||
				child == comboBoxDetail ||
				child == sliderGamma ||
				child == sliderMusicVolume ||
				child == sliderSFXVolume ||
				child == sliderVoiceVolume ||
				child == checkAlternateMouse ||
				child == checkDoubleClickAttackMove ||
				child == checkRetaliation ||
				child == sliderScrollSpeed ||
				child == comboBoxOnlineIP ||
				child == comboBoxLANIP ||
				child == s_zhrTextEntryFirewallPort ||
				child == buttonFirewallRefresh ||
				child == btnDefaults ||
				child == btnAccept ||
				child == btnBack ||
				child == lblVersion ||
				child == WinAdvancedDisplay;
			if (!keep)
			{
				child->winHide(TRUE);
			}
		}
	};
	pruneLegacyChildren(host);
	if (s_zhrVideoGroup && s_zhrVideoGroup != host) pruneLegacyChildren(s_zhrVideoGroup);
	if (s_zhrAudioGroup && s_zhrAudioGroup != host) pruneLegacyChildren(s_zhrAudioGroup);
	if (s_zhrControlsGroup && s_zhrControlsGroup != host) pruneLegacyChildren(s_zhrControlsGroup);
	if (s_zhrNetworkGroup && s_zhrNetworkGroup != host) pruneLegacyChildren(s_zhrNetworkGroup);

	// Layout metrics derived proportionally from modal card size
	const Int padX = static_cast<Int>(cardW * 0.032f);
	const Int padY = static_cast<Int>(cardH * 0.028f);
	const Int innerW = cardW - padX * 2;

	const Int headerH = static_cast<Int>(cardH * 0.072f);
	const Int tabH = static_cast<Int>(cardH * 0.070f);
	const Int footerH = static_cast<Int>(cardH * 0.080f);

	const Int headerY = padY;
	const Int tabY = headerY + headerH + static_cast<Int>(cardH * 0.014f);
	const Int headerDivY = tabY + tabH + static_cast<Int>(cardH * 0.014f);
	const Int footerY = cardH - padY - footerH;
	const Int footerDivY = footerY - static_cast<Int>(cardH * 0.020f);
	const Int bodyY = headerDivY + static_cast<Int>(cardH * 0.022f);
	const Int bodyH = footerDivY - bodyY - static_cast<Int>(cardH * 0.016f);

	const Color colTitle = GameMakeColor(242, 250, 255, 255);
	const Color colAccent = GameMakeColor(102, 214, 255, 255);
	const Color colLabel = GameMakeColor(208, 228, 246, 245);
	const Color colMuted = GameMakeColor(148, 184, 214, 220);

	// 1. Header bar: "OPTIONS" left, compact version badge right (inside card — never clipped!)
	s_zhrTitleLabel = createZhrLabel(
		host,
		"OptionsMenu.wnd:ZhrTitleOptions",
		UnicodeString(L"OPTIONS"),
		padX, headerY, static_cast<Int>(innerW * 0.28f), headerH,
		titleFont,
		colTitle,
		FALSE);

	if (lblVersion)
	{
		const Int verX = padX + static_cast<Int>(innerW * 0.30f);
		const Int verW = innerW - static_cast<Int>(innerW * 0.30f);
		lblVersion->winSetPosition(verX, headerY);
		lblVersion->winSetSize(verW, headerH);
		lblVersion->winSetEnabledTextColors(colMuted, GameMakeColor(0, 8, 18, 200));
		TextData *vData = static_cast<TextData *>(lblVersion->winGetUserData());
		if (vData)
		{
			vData->centered = FALSE;
			vData->centeredVertically = TRUE;
		}
		lblVersion->winHide(FALSE);
	}

	// 2. Tab Bar: GRAPHICS | AUDIO | CONTROLS | NETWORK
	const char *tabNames[ZHR_TAB_COUNT] = {
		"OptionsMenu.wnd:ZhrTabGraphics",
		"OptionsMenu.wnd:ZhrTabAudio",
		"OptionsMenu.wnd:ZhrTabControls",
		"OptionsMenu.wnd:ZhrTabNetwork"
	};
	const WideChar *tabLabels[ZHR_TAB_COUNT] = {
		L"GRAPHICS",
		L"AUDIO",
		L"CONTROLS",
		L"NETWORK"
	};
	const Int tabGap = static_cast<Int>(cardW * 0.010f);
	const Int tabW = (innerW - tabGap * (ZHR_TAB_COUNT - 1)) / ZHR_TAB_COUNT;
	for (Int i = 0; i < ZHR_TAB_COUNT; ++i)
	{
		s_zhrTabButtons[i] = createZhrButton(
			host,
			tabNames[i],
			UnicodeString(tabLabels[i]),
			padX + i * (tabW + tabGap),
			tabY,
			tabW,
			tabH,
			buttonFont,
			s_zhrTabIDs[i]);
	}

	// Horizontal dividers
	s_zhrHeaderDivider = createZhrPanel(host, "OptionsMenu.wnd:ZhrDividerTop", padX, headerDivY, innerW, 2);
	s_zhrFooterDivider = createZhrPanel(host, "OptionsMenu.wnd:ZhrDividerBottom", padX, footerDivY, innerW, 2);

	// 3. TAB 0 — GRAPHICS (2 balanced cards: DISPLAY | VULKAN 1.3 REMASTER)
	const Int colGap = static_cast<Int>(cardW * 0.022f);
	const Int colW = (innerW - colGap) / 2;
	const Int leftCardX = padX;
	const Int rightCardX = padX + colW + colGap;
	const Int cPadX = static_cast<Int>(colW * 0.050f);
	const Int cPadY = static_cast<Int>(bodyH * 0.048f);
	const Int ctrlW = colW - cPadX * 2;
	const Int secHdrH = static_cast<Int>(bodyH * 0.075f);
	const Int lblH = static_cast<Int>(bodyH * 0.060f);
	const Int comboH = static_cast<Int>(bodyH * 0.090f);
	const Int sliderH = static_cast<Int>(bodyH * 0.082f);
	const Int checkH = static_cast<Int>(bodyH * 0.092f);
	const Int rowGap = static_cast<Int>(bodyH * 0.034f);

	// Sub-panel background cards created as children of `host` BEFORE the group containers in z-order
	s_zhrCardDisplay = createZhrPanel(host, "OptionsMenu.wnd:ZhrSubPanelDisplay", leftCardX, bodyY, colW, bodyH);
	s_zhrCardVulkan = createZhrPanel(host, "OptionsMenu.wnd:ZhrSubPanelVulkan", rightCardX, bodyY, colW, bodyH);

	GameWindow *gfxParent = s_zhrVideoGroup ? s_zhrVideoGroup : host;
	GameWindow *audParent = s_zhrAudioGroup ? s_zhrAudioGroup : host;
	GameWindow *ctlParent = s_zhrControlsGroup ? s_zhrControlsGroup : host;
	GameWindow *netParent = s_zhrNetworkGroup ? s_zhrNetworkGroup : host;

	// Left card: DISPLAY (Resolution, Detail Preset + Custom button, Max Render FPS, Brightness)
	const Int leftRowGap = static_cast<Int>(bodyH * 0.024f);
	const Int leftComboH = static_cast<Int>(bodyH * 0.084f);
	Int cy = bodyY + cPadY;
	s_zhrLblDisplayHdr = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblDisplayHdr", UnicodeString(L"DISPLAY"),
		leftCardX + cPadX, cy, ctrlW, secHdrH, sectionFont, colAccent);
	cy += secHdrH + leftRowGap;

	s_zhrLblResolution = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblResolution", UnicodeString(L"Resolution"),
		leftCardX + cPadX, cy, ctrlW, lblH, bodyFont, colLabel);
	cy += lblH + 2;
	placeComboBox(comboBoxResolution, leftCardX + cPadX, cy, ctrlW, leftComboH);
	cy += leftComboH + leftRowGap;

	s_zhrLblDetail = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblDetail", UnicodeString(L"Detail Preset"),
		leftCardX + cPadX, cy, ctrlW, lblH, bodyFont, colLabel);
	cy += lblH + 2;
	if (comboBoxDetail)
		GadgetComboBoxSetMaxDisplay(comboBoxDetail, 5);
	const Int customBtnW = static_cast<Int>(ctrlW * 0.36f);
	const Int detailComboW = ctrlW - customBtnW - 8;
	placeComboBox(comboBoxDetail, leftCardX + cPadX, cy, detailComboW, leftComboH);
	s_zhrBtnCustomDetail = createZhrButton(
		gfxParent, "OptionsMenu.wnd:ZhrButtonCustomDetail", UnicodeString(L"Custom..."),
		leftCardX + cPadX + ctrlW - customBtnW, cy, customBtnW, leftComboH, bodyFont, s_zhrBtnCustomDetailID);
	if (s_zhrBtnCustomDetail && comboBoxDetail && !comboBoxDetail->winGetEnabled())
		s_zhrBtnCustomDetail->winEnable(FALSE);
	cy += leftComboH + leftRowGap;

	const Int halfGap = 10;
	const Int halfW = (ctrlW - halfGap) / 2;
	const Int rightHalfX = leftCardX + cPadX + halfW + halfGap;

	s_zhrLblMaxFps = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblMaxFps", UnicodeString(L"Max Render FPS"),
		leftCardX + cPadX, cy, halfW, lblH, bodyFont, colLabel);
	s_zhrLblUiScale = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblUiScale", UnicodeString(L"UI Scale (DPI)"),
		rightHalfX, cy, ctrlW - halfW - halfGap, lblH, bodyFont, colLabel);
	cy += lblH + 2;
	s_zhrComboMaxFps = createZhrComboBox(
		gfxParent, "OptionsMenu.wnd:ZhrComboMaxFps",
		leftCardX + cPadX, cy, halfW, leftComboH, bodyFont, s_zhrComboMaxFpsID);
	if (s_zhrComboMaxFps)
	{
		const Color entryCol = GameMakeColor(236, 246, 255, 255);
		GadgetComboBoxReset(s_zhrComboMaxFps);
		GadgetComboBoxSetMaxDisplay(s_zhrComboMaxFps, 6);
		GadgetComboBoxAddEntry(s_zhrComboMaxFps, UnicodeString(L"30 FPS (Classic)"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboMaxFps, UnicodeString(L"60 FPS"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboMaxFps, UnicodeString(L"120 FPS"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboMaxFps, UnicodeString(L"144 FPS"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboMaxFps, UnicodeString(L"240 FPS"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboMaxFps, UnicodeString(L"Uncapped"), entryCol);
		GadgetComboBoxSetSelectedPos(s_zhrComboMaxFps, zhrFpsToComboIdx(s_zhrCurMaxFps));
	}
	s_zhrComboUiScale = createZhrComboBox(
		gfxParent, "OptionsMenu.wnd:ZhrComboUiScale",
		rightHalfX, cy, ctrlW - halfW - halfGap, leftComboH, bodyFont, s_zhrComboUiScaleID);
	if (s_zhrComboUiScale)
	{
		const Color entryCol = GameMakeColor(236, 246, 255, 255);
		GadgetComboBoxReset(s_zhrComboUiScale);
		GadgetComboBoxSetMaxDisplay(s_zhrComboUiScale, 6);
		GadgetComboBoxAddEntry(s_zhrComboUiScale, UnicodeString(L"Auto (DPI)"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboUiScale, UnicodeString(L"65% Compact"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboUiScale, UnicodeString(L"75%"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboUiScale, UnicodeString(L"85%"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboUiScale, UnicodeString(L"100% (4:3)"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboUiScale, UnicodeString(L"Classic Stretch"), entryCol);
		GadgetComboBoxSetSelectedPos(s_zhrComboUiScale, zhrUiScaleToComboIdx(s_zhrCurUiScale));
	}
	cy += leftComboH + leftRowGap;

	s_zhrLblBrightness = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblBrightness", UnicodeString(L"Brightness"),
		leftCardX + cPadX, cy, ctrlW, lblH, bodyFont, colLabel);
	cy += lblH + 2;
	placeSlider(sliderGamma, leftCardX + cPadX, cy, ctrlW, sliderH);

	// Right card: VULKAN 1.3 REMASTER
	cy = bodyY + cPadY;
	s_zhrLblVulkanHdr = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblVulkanHdr", UnicodeString(L"VULKAN 1.3 REMASTER"),
		rightCardX + cPadX, cy, ctrlW, secHdrH, sectionFont, colAccent);
	cy += secHdrH + rowGap;

	s_zhrLblVisualMode = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblVisualMode", UnicodeString(L"Visual Mode  [F9]"),
		rightCardX + cPadX, cy, ctrlW, lblH, bodyFont, colLabel);
	cy += lblH + 4;

	s_zhrComboVisualMode = createZhrComboBox(
		gfxParent, "OptionsMenu.wnd:ZhrComboVisualMode",
		rightCardX + cPadX, cy, ctrlW, comboH, bodyFont, s_zhrComboVisualModeID);
	if (s_zhrComboVisualMode)
	{
		const Color entryCol = GameMakeColor(236, 246, 255, 255);
		GadgetComboBoxReset(s_zhrComboVisualMode);
		GadgetComboBoxSetMaxDisplay(s_zhrComboVisualMode, 3);
		GadgetComboBoxAddEntry(s_zhrComboVisualMode, UnicodeString(L"Remastered (Vulkan 1.3)"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboVisualMode, UnicodeString(L"Split-Screen A/B Compare"), entryCol);
		GadgetComboBoxAddEntry(s_zhrComboVisualMode, UnicodeString(L"Classic 2003"), entryCol);
	}
	cy += comboH + rowGap;

	const Int checkGap = static_cast<Int>(bodyH * 0.022f);
	s_zhrCheckPbr = createZhrCheckBox(
		gfxParent, "OptionsMenu.wnd:ZhrCheckPbrWaterSsao",
		UnicodeString(L"PBR, Coastal Water & SSAO  [F10]"),
		rightCardX + cPadX, cy, ctrlW, checkH, bodyFont, s_zhrCheckPbrID);
	cy += checkH + checkGap;

	s_zhrCheckPostFx = createZhrCheckBox(
		gfxParent, "OptionsMenu.wnd:ZhrCheckHdrPostFx",
		UnicodeString(L"HDR Bloom, ACES & CAS  [F11]"),
		rightCardX + cPadX, cy, ctrlW, checkH, bodyFont, s_zhrCheckPostFxID);
	cy += checkH + checkGap;

	s_zhrCheckRemasterTex = createZhrCheckBox(
		gfxParent, "OptionsMenu.wnd:ZhrCheckRemasterTex",
		UnicodeString(L"2x Remastered 3D Textures  [F8]"),
		rightCardX + cPadX, cy, ctrlW, checkH, bodyFont, s_zhrCheckRemasterTexID);
	cy += checkH + checkGap;

	s_zhrCheckMeshSmooth = createZhrCheckBox(
		gfxParent, "OptionsMenu.wnd:ZhrCheckMeshSmoothing",
		UnicodeString(L"Phong .W3D Mesh Smoothing  [F7]"),
		rightCardX + cPadX, cy, ctrlW, checkH, bodyFont, s_zhrCheckMeshSmoothID);

	const Int hintH = static_cast<Int>(bodyH * 0.065f);
	s_zhrLblVulkanHint = createZhrLabel(
		gfxParent, "OptionsMenu.wnd:ZhrLblVulkanHint",
		s_zhrHasVulkan
			? UnicodeString(L"Live preview active \u2022 Accept saves to Options.ini")
			: UnicodeString(L"Vulkan 1.3 backend inactive in this build"),
		rightCardX + cPadX, bodyY + bodyH - cPadY - hintH, ctrlW, hintH, smallFont, colMuted);

	// 4. TAB 1 — AUDIO (centered spacious card)
	const Int singleCardW = static_cast<Int>(innerW * 0.72f);
	const Int singleCardX = padX + (innerW - singleCardW) / 2;
	const Int scPadX = static_cast<Int>(singleCardW * 0.06f);
	const Int scCtrlW = singleCardW - scPadX * 2;

	s_zhrCardAudio = createZhrPanel(host, "OptionsMenu.wnd:ZhrSubPanelAudio", singleCardX, bodyY, singleCardW, bodyH);
	cy = bodyY + cPadY;
	s_zhrLblAudioHdr = createZhrLabel(
		audParent, "OptionsMenu.wnd:ZhrLblAudioHdr", UnicodeString(L"AUDIO MIXER"),
		singleCardX + scPadX, cy, scCtrlW, secHdrH, sectionFont, colAccent);
	cy += secHdrH + rowGap * 2;

	s_zhrLblMusicVol = createZhrLabel(
		audParent, "OptionsMenu.wnd:ZhrLblMusicVol", UnicodeString(L"Music Volume"),
		singleCardX + scPadX, cy, scCtrlW, lblH, bodyFont, colLabel);
	cy += lblH + 6;
	placeSlider(sliderMusicVolume, singleCardX + scPadX, cy, scCtrlW, sliderH);
	cy += sliderH + rowGap * 2;

	s_zhrLblSfxVol = createZhrLabel(
		audParent, "OptionsMenu.wnd:ZhrLblSfxVol", UnicodeString(L"Sound FX Volume"),
		singleCardX + scPadX, cy, scCtrlW, lblH, bodyFont, colLabel);
	cy += lblH + 6;
	placeSlider(sliderSFXVolume, singleCardX + scPadX, cy, scCtrlW, sliderH);
	cy += sliderH + rowGap * 2;

	s_zhrLblVoiceVol = createZhrLabel(
		audParent, "OptionsMenu.wnd:ZhrLblVoiceVol", UnicodeString(L"Voice Volume"),
		singleCardX + scPadX, cy, scCtrlW, lblH, bodyFont, colLabel);
	cy += lblH + 6;
	placeSlider(sliderVoiceVolume, singleCardX + scPadX, cy, scCtrlW, sliderH);

	// 5. TAB 2 — CONTROLS (centered spacious card)
	s_zhrCardControls = createZhrPanel(host, "OptionsMenu.wnd:ZhrSubPanelControls", singleCardX, bodyY, singleCardW, bodyH);
	cy = bodyY + cPadY;
	s_zhrLblControlsHdr = createZhrLabel(
		ctlParent, "OptionsMenu.wnd:ZhrLblControlsHdr", UnicodeString(L"MOUSE & CAMERA CONTROLS"),
		singleCardX + scPadX, cy, scCtrlW, secHdrH, sectionFont, colAccent);
	cy += secHdrH + rowGap * 2;

	if (checkAlternateMouse)
	{
		checkAlternateMouse->winSetPosition(singleCardX + scPadX, cy);
		checkAlternateMouse->winSetSize(scCtrlW, checkH);
	}
	cy += checkH + rowGap;

	if (checkDoubleClickAttackMove)
	{
		checkDoubleClickAttackMove->winSetPosition(singleCardX + scPadX, cy);
		checkDoubleClickAttackMove->winSetSize(scCtrlW, checkH);
	}
	cy += checkH + rowGap;

	if (checkRetaliation)
	{
		checkRetaliation->winSetPosition(singleCardX + scPadX, cy);
		checkRetaliation->winSetSize(scCtrlW, checkH);
	}
	cy += checkH + rowGap * 2;

	s_zhrLblScrollSpeed = createZhrLabel(
		ctlParent, "OptionsMenu.wnd:ZhrLblScrollSpeed", UnicodeString(L"Scroll Speed"),
		singleCardX + scPadX, cy, scCtrlW, lblH, bodyFont, colLabel);
	cy += lblH + 6;
	placeSlider(sliderScrollSpeed, singleCardX + scPadX, cy, scCtrlW, sliderH);

	// 6. TAB 3 — NETWORK (centered spacious card)
	s_zhrCardNetwork = createZhrPanel(host, "OptionsMenu.wnd:ZhrSubPanelNetwork", singleCardX, bodyY, singleCardW, bodyH);
	cy = bodyY + cPadY;
	s_zhrLblNetworkHdr = createZhrLabel(
		netParent, "OptionsMenu.wnd:ZhrLblNetworkHdr", UnicodeString(L"MULTIPLAYER & NETWORK"),
		singleCardX + scPadX, cy, scCtrlW, secHdrH, sectionFont, colAccent);
	cy += secHdrH + rowGap * 2;

	s_zhrLblOnlineIP = createZhrLabel(
		netParent, "OptionsMenu.wnd:ZhrLblOnlineIP", UnicodeString(L"Online IP"),
		singleCardX + scPadX, cy, scCtrlW, lblH, bodyFont, colLabel);
	cy += lblH + 4;
	placeComboBox(comboBoxOnlineIP, singleCardX + scPadX, cy, scCtrlW, comboH);
	cy += comboH + rowGap;

	s_zhrLblLanIP = createZhrLabel(
		netParent, "OptionsMenu.wnd:ZhrLblLanIP", UnicodeString(L"LAN IP"),
		singleCardX + scPadX, cy, scCtrlW, lblH, bodyFont, colLabel);
	cy += lblH + 4;
	placeComboBox(comboBoxLANIP, singleCardX + scPadX, cy, scCtrlW, comboH);
	cy += comboH + rowGap;

	s_zhrLblFirewallPort = createZhrLabel(
		netParent, "OptionsMenu.wnd:ZhrLblFirewallPort", UnicodeString(L"Firewall Port Override"),
		singleCardX + scPadX, cy, scCtrlW, lblH, bodyFont, colLabel);
	cy += lblH + 4;
	const Int natBtnW = static_cast<Int>(scCtrlW * 0.34f);
	const Int portFieldW = scCtrlW - natBtnW - static_cast<Int>(scCtrlW * 0.04f);
	if (s_zhrTextEntryFirewallPort)
	{
		s_zhrTextEntryFirewallPort->winSetPosition(singleCardX + scPadX, cy);
		s_zhrTextEntryFirewallPort->winSetSize(portFieldW, comboH);
	}
	if (buttonFirewallRefresh)
	{
		buttonFirewallRefresh->winSetPosition(singleCardX + scPadX + scCtrlW - natBtnW, cy);
		buttonFirewallRefresh->winSetSize(natBtnW, comboH);
	}

	// 7. Footer bar: DEFAULTS left, ACCEPT + CANCEL right
	const Int btnGap = static_cast<Int>(cardW * 0.014f);
	const Int defBtnW = static_cast<Int>(innerW * 0.22f);
	const Int accBtnW = static_cast<Int>(innerW * 0.24f);
	const Int canBtnW = static_cast<Int>(innerW * 0.22f);

	if (btnDefaults)
	{
		btnDefaults->winSetPosition(padX, footerY);
		btnDefaults->winSetSize(defBtnW, footerH);
	}
	if (btnBack)
	{
		btnBack->winSetPosition(padX + innerW - canBtnW, footerY);
		btnBack->winSetSize(canBtnW, footerH);
	}
	if (btnAccept)
	{
		btnAccept->winSetPosition(padX + innerW - canBtnW - btnGap - accBtnW, footerY);
		btnAccept->winSetSize(accBtnW, footerH);
	}

	// Ensure group containers and interactive controls sit cleanly ABOVE the ZhrSubPanel cards (and dropdowns on top!)
	auto bringTop = [](GameWindow *w) { if (w) w->winBringToTop(); };
	bringTop(s_zhrVideoGroup);
	bringTop(s_zhrAudioGroup);
	bringTop(s_zhrControlsGroup);
	bringTop(s_zhrNetworkGroup);

	bringTop(sliderGamma);
	bringTop(s_zhrComboMaxFps);
	bringTop(s_zhrBtnCustomDetail);
	bringTop(comboBoxDetail);
	bringTop(comboBoxResolution);
	bringTop(s_zhrCheckPbr);
	bringTop(s_zhrCheckPostFx);
	bringTop(s_zhrCheckRemasterTex);
	bringTop(s_zhrCheckMeshSmooth);
	bringTop(s_zhrComboVisualMode);

	bringTop(sliderMusicVolume);
	bringTop(sliderSFXVolume);
	bringTop(sliderVoiceVolume);

	bringTop(checkAlternateMouse);
	bringTop(checkDoubleClickAttackMove);
	bringTop(checkRetaliation);
	bringTop(sliderScrollSpeed);

	bringTop(s_zhrTextEntryFirewallPort);
	bringTop(buttonFirewallRefresh);
	bringTop(comboBoxLANIP);
	bringTop(comboBoxOnlineIP);

	bringTop(btnDefaults);
	bringTop(btnAccept);
	bringTop(btnBack);
	bringTop(lblVersion);
	bringTop(WinAdvancedDisplay);

	// Sync Vulkan controls and choose starting tab (optionally driven by ZHR_AUTO_OPEN_OPTIONS)
	syncVulkanControlsFromState();

	Int initialTab = ZHR_TAB_GRAPHICS;
	const char *autoOpt = getenv("ZHR_AUTO_OPEN_OPTIONS");
	if (autoOpt)
	{
		if (_stricmp(autoOpt, "audio") == 0) initialTab = ZHR_TAB_AUDIO;
		else if (_stricmp(autoOpt, "controls") == 0) initialTab = ZHR_TAB_CONTROLS;
		else if (_stricmp(autoOpt, "network") == 0) initialTab = ZHR_TAB_NETWORK;
	}
	selectZhrOptionsTab(initialTab);
}

static void setDefaults()
{
	constexpr const Bool ModifyDisplaySettings = FALSE;

	//-------------------------------------------------------------------------------------------------
	// provider type
//	GadgetCheckBoxSetChecked(checkAudioHardware, FALSE);

	//-------------------------------------------------------------------------------------------------
	// speaker type
//	GadgetCheckBoxSetChecked(checkAudioSurround, FALSE);

	//-------------------------------------------------------------------------------------------------
	// language filter
	GadgetCheckBoxSetChecked( checkLanguageFilter, TRUE );

	if constexpr (ModifyDisplaySettings)
	{
	//-------------------------------------------------------------------------------------------------
	// LOD
	if ((TheGameLogic->isInGame() == FALSE) || (TheGameLogic->isInShellGame() == TRUE))
	{
		GadgetComboBoxSetSelectedPos(comboBoxDetail, (Int)TheGameLODManager->getRecommendedStaticLODLevel());
	}

	//-------------------------------------------------------------------------------------------------
	// Resolution
	//Find index of 800x600 mode.
	if ((TheGameLogic->isInGame() == FALSE || TheGameLogic->isInShellGame() == TRUE) && !TheGameSpyInfo) {
		Int numResolutions = TheDisplay->getDisplayModeCount();
		Int defaultResIndex=0;
		for( Int i = 0; i < numResolutions; ++i )
		{	Int xres,yres,bitDepth;
			TheDisplay->getDisplayModeDescription(i,&xres,&yres,&bitDepth);
			if (xres == DEFAULT_DISPLAY_WIDTH && yres == DEFAULT_DISPLAY_HEIGHT)	//keep track of default mode in case we need it.
			{	defaultResIndex=i;
				break;
			}
		}
		GadgetComboBoxSetSelectedPos( comboBoxResolution, defaultResIndex );	//should be 800x600 (our lowest supported mode)
	}
	}

	//-------------------------------------------------------------------------------------------------
	// Mouse Mode
	GadgetCheckBoxSetChecked(checkAlternateMouse, FALSE);
	GadgetCheckBoxSetChecked(checkRetaliation, TRUE );
	GadgetCheckBoxSetChecked( checkDoubleClickAttackMove, FALSE );

	//-------------------------------------------------------------------------------------------------
//	// scroll speed val
	Int scrollPos = (Int)(TheGlobalData->m_keyboardDefaultScrollFactor*100.0f);
	GadgetSliderSetPosition( sliderScrollSpeed, scrollPos );


	Int valMin, valMax;

	//-------------------------------------------------------------------------------------------------
	// slider music volume
	GadgetSliderGetMinMax(sliderMusicVolume,&valMin, &valMax);
	GadgetSliderSetPosition(sliderMusicVolume,REAL_TO_INT(TheAudio->getAudioSettings()->m_defaultMusicVolume * 100.0f));

	//-------------------------------------------------------------------------------------------------
	// slider SFX volume
	GadgetSliderGetMinMax(sliderSFXVolume,&valMin, &valMax);
	Real maxVolume = MAX( TheAudio->getAudioSettings()->m_defaultSoundVolume, TheAudio->getAudioSettings()->m_default3DSoundVolume );
	GadgetSliderSetPosition( sliderSFXVolume, REAL_TO_INT( maxVolume * 100.0f ) );

	//-------------------------------------------------------------------------------------------------
	// slider Voice volume
	GadgetSliderGetMinMax(sliderVoiceVolume,&valMin, &valMax);
	GadgetSliderSetPosition(sliderVoiceVolume, REAL_TO_INT(TheAudio->getAudioSettings()->m_defaultSpeechVolume * 100.0f));

	//-------------------------------------------------------------------------------------------------
 	// slider Gamma
 	GadgetSliderGetMinMax(sliderGamma,&valMin, &valMax);
 	GadgetSliderSetPosition(sliderGamma, ((valMax - valMin) / 2 + valMin));

	//-------------------------------------------------------------------------------------------------
	// ZeroHourReloaded Vulkan 1.3 Remaster, UI Scale & Frame Pacing defaults (Remastered all-ON, 144 FPS, Auto DPI UI)
	s_zhrCurMaxFps = 144u;
	s_zhrCurUiScale = 0.0f;
	{
		const Bool prevIgn = ignoreSelected;
		ignoreSelected = TRUE;
		if (s_zhrComboMaxFps)
			GadgetComboBoxSetSelectedPos(s_zhrComboMaxFps, zhrFpsToComboIdx(s_zhrCurMaxFps));
		if (s_zhrComboUiScale)
			GadgetComboBoxSetSelectedPos(s_zhrComboUiScale, zhrUiScaleToComboIdx(s_zhrCurUiScale));
		ignoreSelected = prevIgn;
	}
	applyLiveMaxRenderFps(s_zhrCurMaxFps);

	if (s_zhrHasVulkan)
	{
		s_zhrCurMode = 1u;
		s_zhrCurPbr = TRUE;
		s_zhrCurPostFx = TRUE;
		s_zhrCurRemasterTex = TRUE;
		s_zhrCurMeshSmooth = TRUE;
		syncVulkanControlsFromState();
		applyLiveVulkanRemasterSettings();
	}

	if constexpr (ModifyDisplaySettings)
	{
	if ((TheGameLogic->isInGame() == FALSE) || (TheGameLogic->isInShellGame() == TRUE))
	{
		//-------------------------------------------------------------------------------------------------
		// Texture resolution slider
		//
		Int	txtFact=TheGameLODManager->getRecommendedTextureReduction();

		GadgetSliderSetPosition( sliderTextureResolution, 2-txtFact);

		//-------------------------------------------------------------------------------------------------
 		// 3D Shadows checkbox
		//
		GadgetCheckBoxSetChecked( check3DShadows, TheGlobalData->m_useShadowVolumes);

		//-------------------------------------------------------------------------------------------------
 		// 2D Shadows checkbox
		//
		GadgetCheckBoxSetChecked( check2DShadows, TheGlobalData->m_useShadowDecals);

		//-------------------------------------------------------------------------------------------------
 		// Cloud Shadows checkbox
		//
		GadgetCheckBoxSetChecked( checkCloudShadows, TheGlobalData->m_useCloudMap);

		//-------------------------------------------------------------------------------------------------
 		// Ground Lighting (lightmap) checkbox
		//
		GadgetCheckBoxSetChecked( checkGroundLighting, TheGlobalData->m_useLightMap);

		//-------------------------------------------------------------------------------------------------
 		// Smooth Water Border checkbox
		//
		GadgetCheckBoxSetChecked( checkSmoothWater, TheGlobalData->m_showSoftWaterEdge);

		//-------------------------------------------------------------------------------------------------
 		// Extra Animations (tree sway and buildups) checkbox
		//
		GadgetCheckBoxSetChecked( checkExtraAnimations, !TheGlobalData->m_useDrawModuleLOD);

		//-------------------------------------------------------------------------------------------------
 		// DisableDynamicLOD
		//
		GadgetCheckBoxSetChecked( checkNoDynamicLod, !TheGlobalData->m_enableDynamicLOD);

		//-------------------------------------------------------------------------------------------------
 		// Disable FPS Limit
		//
		GadgetCheckBoxSetChecked( checkUnlockFps, !TheGlobalData->m_useFpsLimit);

		//-------------------------------------------------------------------------------------------------
 		// Heat Effects
		//
		GadgetCheckBoxSetChecked( checkHeatEffects, TheGlobalData->m_useHeatEffects);

		//-------------------------------------------------------------------------------------------------
 		// Building Occlusion checkbox
		//
		GadgetCheckBoxSetChecked( checkBuildingOcclusion, TheGlobalData->m_enableBehindBuildingMarkers);

		//-------------------------------------------------------------------------------------------------
 		// Particle Cap slider
		//
		GadgetSliderSetPosition( sliderParticleCap, TheGlobalData->m_maxParticleCount);

		//-------------------------------------------------------------------------------------------------
 		// Trees and Shrubs
		//
		GadgetCheckBoxSetChecked( checkProps, TheGlobalData->m_useTrees);
	}
	}
}

static void saveOptions()
{
	Int index;
	Int val;
	//-------------------------------------------------------------------------------------------------
//	// provider type
//	Bool isChecked = GadgetCheckBoxIsChecked(checkAudioHardware);
//	TheAudio->setHardwareAccelerated(isChecked);
//	(*pref)["3DAudioProvider"] = TheAudio->getProviderName(TheAudio->getSelectedProvider());
//
	//-------------------------------------------------------------------------------------------------
//	// speaker type
	//	isChecked = GadgetCheckBoxIsChecked(checkAudioSurround);
	//	TheAudio->setSpeakerSurround(isChecked);
	//	(*pref)["SpeakerType"] = TheAudio->translateUnsignedIntToSpeakerType(TheAudio->getSpeakerType());
	//
	//-------------------------------------------------------------------------------------------------
	// language filter
	if( GadgetCheckBoxIsChecked( checkLanguageFilter ) )
	{
			//GadgetCheckBoxSetChecked( checkLanguageFilter, true);
			TheWritableGlobalData->m_languageFilterPref = true;
			(*pref)["LanguageFilter"] = "true";
	}
	else
	{
			//GadgetCheckBoxSetChecked( checkLanguageFilter, false);
			TheWritableGlobalData->m_languageFilterPref = false;
			(*pref)["LanguageFilter"] = "false";
	}


	//-------------------------------------------------------------------------------------------------
	// Custom game detail settings.
	GadgetComboBoxGetSelectedPos( comboBoxDetail, &index );
	if (index == STATIC_GAME_LOD_CUSTOM)
	{
 		//-------------------------------------------------------------------------------------------------
 		// Texture resolution slider
		{
		 		val = 2 - GadgetSliderGetPosition(sliderTextureResolution);

				AsciiString prefString;
				prefString.format("%d",val);
				(*pref)["TextureReduction"] = prefString;

				TheWritableGlobalData->m_textureReductionFactor = val;
				TheGameClient->setTextureLOD(val);
		}

		TheWritableGlobalData->m_useShadowVolumes = GadgetCheckBoxIsChecked( check3DShadows );
		(*pref)["UseShadowVolumes"] = TheWritableGlobalData->m_useShadowVolumes ? "yes" : "no";

		TheWritableGlobalData->m_useShadowDecals = GadgetCheckBoxIsChecked( check2DShadows );
		(*pref)["UseShadowDecals"] = TheWritableGlobalData->m_useShadowDecals ? "yes" : "no";

		TheWritableGlobalData->m_useCloudMap = GadgetCheckBoxIsChecked( checkCloudShadows );
		(*pref)["UseCloudMap"] = TheGlobalData->m_useCloudMap ? "yes" : "no";

		TheWritableGlobalData->m_useLightMap = GadgetCheckBoxIsChecked( checkGroundLighting );
		(*pref)["UseLightMap"] = TheGlobalData->m_useLightMap ? "yes" : "no";

		TheWritableGlobalData->m_showSoftWaterEdge = GadgetCheckBoxIsChecked( checkSmoothWater );
		(*pref)["ShowSoftWaterEdge"] = TheGlobalData->m_showSoftWaterEdge ? "yes" : "no";

		TheWritableGlobalData->m_useDrawModuleLOD = !GadgetCheckBoxIsChecked( checkExtraAnimations );
		TheWritableGlobalData->m_useTreeSway = !TheWritableGlobalData->m_useDrawModuleLOD;	//borrow same setting.
		(*pref)["ExtraAnimations"] = TheGlobalData->m_useDrawModuleLOD ? "no" : "yes";

		TheWritableGlobalData->m_enableDynamicLOD = !GadgetCheckBoxIsChecked( checkNoDynamicLod );
		(*pref)["DynamicLOD"] = TheGlobalData->m_enableDynamicLOD ? "yes" : "no";

		TheWritableGlobalData->m_useHeatEffects = GadgetCheckBoxIsChecked( checkHeatEffects );
		(*pref)["HeatEffects"] = TheGlobalData->m_useHeatEffects ? "yes" : "no";

		// Never write this out
		//TheWritableGlobalData->m_useFpsLimit = !GadgetCheckBoxIsChecked( checkUnlockFps );
		//(*pref)["FPSLimit"] = TheGlobalData->m_useFpsLimit ? "yes" : "no";

		TheWritableGlobalData->m_enableBehindBuildingMarkers = GadgetCheckBoxIsChecked( checkBuildingOcclusion );
		(*pref)["BuildingOcclusion"] = TheWritableGlobalData->m_enableBehindBuildingMarkers ? "yes" : "no";

		TheWritableGlobalData->m_useTrees = GadgetCheckBoxIsChecked( checkProps);
		(*pref)["ShowTrees"] = TheWritableGlobalData->m_useTrees ? "yes" : "no";

 		//-------------------------------------------------------------------------------------------------
		// Particle Cap slider
		{
				AsciiString prefString;

		 		val = GadgetSliderGetPosition(sliderParticleCap);

				prefString.format("%d",val);
				(*pref)["MaxParticleCount"] = prefString;

				TheWritableGlobalData->m_maxParticleCount = val;
		}
	}

	//-------------------------------------------------------------------------------------------------
	// LOD
	if (comboBoxDetail && comboBoxDetail->winGetEnabled())
	{
		GadgetComboBoxGetSelectedPos( comboBoxDetail, &index );
		const Bool levelChanged = TheGameLODManager->setStaticLODLevel((StaticGameLODLevel)index);

		if (levelChanged)
			(*pref)["StaticGameLOD"] = TheGameLODManager->getStaticGameLODLevelName(TheGameLODManager->getStaticLODLevel());
	}

	//-------------------------------------------------------------------------------------------------
	// IP address
	if (comboBoxLANIP && comboBoxLANIP->winGetEnabled())
	{
		UnsignedInt ip;
		GadgetComboBoxGetSelectedPos(comboBoxLANIP, &index);
		if (index>=0 && TheGlobalData)
		{
			ip = (UnsignedInt)(uintptr_t)GadgetComboBoxGetItemData(comboBoxLANIP, index);
			TheWritableGlobalData->m_defaultIP = ip;
			pref->setLANIPAddress(ip);
		}
	}

	if (comboBoxOnlineIP && comboBoxOnlineIP->winGetEnabled())
	{
		UnsignedInt ip;
		GadgetComboBoxGetSelectedPos(comboBoxOnlineIP, &index);
		if (index>=0)
		{
			ip = (UnsignedInt)(uintptr_t)GadgetComboBoxGetItemData(comboBoxOnlineIP, index);
			pref->setOnlineIPAddress(ip);
		}
	}

	//-------------------------------------------------------------------------------------------------
	// Firewall Port Override
	GameWindow *textEntryFirewallPortOverride = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("OptionsMenu.wnd:TextEntryFirewallPortOverride"));
	if (textEntryFirewallPortOverride && textEntryFirewallPortOverride->winGetEnabled())
	{
		UnicodeString uStr = GadgetTextEntryGetText(textEntryFirewallPortOverride);
		AsciiString aStr;
		aStr.translate(uStr);
		Int portOverride = atoi(aStr.str());
		if (portOverride < 0 || portOverride > 65535)
			portOverride = 0;
		if (TheGlobalData->m_firewallPortOverride != portOverride)
		{	TheWritableGlobalData->m_firewallPortOverride = portOverride;
		    aStr.format("%d", portOverride);
			(*pref)["FirewallPortOverride"] = aStr;
		}
	}

	//-------------------------------------------------------------------------------------------------
	// antialiasing
  GadgetComboBoxGetSelectedPos(comboBoxAntiAliasing, &index);
  if( index >= 0 )
  {
		Int mode = WW3D::MULTISAMPLE_MODE_NONE;

		// TheSuperHackers @info We are converting comboBox entry position to MultiSampleModeEnum values
		index = clamp((int)OptionPreferences::AntiAliasingMode_OFF, index, (int)OptionPreferences::AntiAliasingMode_MSAA_8X);
		mode = (index > 0) ? 1 << index : 0;

		TheWritableGlobalData->m_antiAliasLevel = mode;
    AsciiString prefString;
		prefString.format("%d", mode);
		(*pref)["AntiAliasing"] = prefString;
  }

	//-------------------------------------------------------------------------------------------------
	// texture filter mode
	val = pref->getTextureFilterMode();
	if (val >= 0)
	{
		val = clamp((int)TextureFilterClass::TEXTURE_FILTER_NONE, val, (int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC);

		TheWritableGlobalData->m_textureFilteringMode = val;
		AsciiString prefString;
		prefString = TextureFilterClass::TextureFilterModeString[val];
		(*pref)["TextureFilter"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// anisotropy level
	val = pref->getTextureAnisotropyLevel();
	if (val >= 0)
	{
		val = clamp((int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_2X, val, (int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_16X);

		TheWritableGlobalData->m_textureAnisotropyLevel = val;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["AnisotropyLevel"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// mouse mode
	TheWritableGlobalData->m_useAlternateMouse = GadgetCheckBoxIsChecked(checkAlternateMouse);
	(*pref)["UseAlternateMouse"] = TheWritableGlobalData->m_useAlternateMouse ? "yes" : "no";

	// TheSuperHackers @todo Add check box ?
	{
		Bool useRightMouseScrollWithAlternateMouse = pref->getRightMouseScrollWithAlternateMouseEnabled();
		(*pref)["UseRightMouseScrollWithAlternateMouse"] = useRightMouseScrollWithAlternateMouse ? "yes" : "no";
		TheWritableGlobalData->m_useRightMouseScrollWithAlternateMouse = useRightMouseScrollWithAlternateMouse;
	}

	TheWritableGlobalData->m_clientRetaliationModeEnabled = GadgetCheckBoxIsChecked(checkRetaliation);
	(*pref)["Retaliation"] = TheWritableGlobalData->m_clientRetaliationModeEnabled? "yes" : "no";

	TheWritableGlobalData->m_doubleClickAttackMove = GadgetCheckBoxIsChecked( checkDoubleClickAttackMove );
	(*pref)["UseDoubleClickAttackMove"] = TheWritableGlobalData->m_doubleClickAttackMove ? "yes" : "no";

	// TheSuperHackers @todo Add combo box ?
	{
		CursorCaptureMode mode = pref->getCursorCaptureMode();
		(*pref)["CursorCaptureEnabledInWindowedGame"] = (mode & CursorCaptureMode_EnabledInWindowedGame) ? "yes" : "no";
		(*pref)["CursorCaptureEnabledInWindowedMenu"] = (mode & CursorCaptureMode_EnabledInWindowedMenu) ? "yes" : "no";
		(*pref)["CursorCaptureEnabledInFullscreenGame"] = (mode & CursorCaptureMode_EnabledInFullscreenGame) ? "yes" : "no";
		(*pref)["CursorCaptureEnabledInFullscreenMenu"] = (mode & CursorCaptureMode_EnabledInFullscreenMenu) ? "yes" : "no";
		TheMouse->setCursorCaptureMode(mode);
	}

	// TheSuperHackers @todo Add combo box ?
	{
		ScreenEdgeScrollMode mode = pref->getScreenEdgeScrollMode();
		(*pref)["ScreenEdgeScrollEnabledInWindowedApp"] = (mode & ScreenEdgeScrollMode_EnabledInWindowedApp) ? "yes" : "no";
		(*pref)["ScreenEdgeScrollEnabledInFullscreenApp"] = (mode & ScreenEdgeScrollMode_EnabledInFullscreenApp) ? "yes" : "no";
		TheLookAtTranslator->setScreenEdgeScrollMode(mode);
	}

	// TheSuperHackers @todo Add checkbox ?
	{
		Bool enabled = pref->getPlayerObserverEnabled();
		(*pref)["PlayerObserverEnabled"] = enabled ? "yes" : "no";
		TheWritableGlobalData->m_enablePlayerObserver = enabled;
	}

	// TheSuperHackers @todo Add checkbox ?
	{
		Bool enabled = pref->getArchiveReplaysEnabled();
		(*pref)["ArchiveReplays"] = enabled ? "yes" : "no";
		TheRecorder->setArchiveEnabled(enabled);
	}

	//-------------------------------------------------------------------------------------------------
	// scroll speed val
	val = GadgetSliderGetPosition(sliderScrollSpeed);
	if(val > 0)
	{
		TheWritableGlobalData->m_keyboardScrollFactor = val/100.0f;
		DEBUG_LOG(("Scroll Speed val %d, keyboard scroll factor %f", val, TheGlobalData->m_keyboardScrollFactor));
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["ScrollFactor"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// draw scroll anchor
	{
		if( TheInGameUI->getDrawRMBScrollAnchor() )
				(*pref)["DrawScrollAnchor"] = "yes";
		else
				(*pref)["DrawScrollAnchor"] = "no";
	}

	//-------------------------------------------------------------------------------------------------
	// move scroll anchor
	{
		if( TheInGameUI->getMoveRMBScrollAnchor() )
				(*pref)["MoveScrollAnchor"] = "yes";
		else
				(*pref)["MoveScrollAnchor"] = "no";
	}

	//-------------------------------------------------------------------------------------------------
	// slider music volume
	val = GadgetSliderGetPosition(sliderMusicVolume);
	if(val != -1)
	{
    AsciiString prefString;
    prefString.format("%d", val);
    (*pref)["MusicVolume"] = prefString;
    TheAudio->setVolume(val / 100.0f, (AudioAffect) (AudioAffect_Music | AudioAffect_SystemSetting));
	}

	//-------------------------------------------------------------------------------------------------
	// slider SFX volume
	val = GadgetSliderGetPosition(sliderSFXVolume);
	if(val != -1)
	{
		//Both 2D and 3D sound effects are sharing the same slider. However, there is a
		//relative slider that gets applied to one of these values to lower that sound volume.
		Real sound2DVolume = val / 100.0f;
		Real sound3DVolume = val / 100.0f;
		Real relative2DVolume = TheAudio->getAudioSettings()->m_relative2DVolume;
		relative2DVolume = MIN( 1.0f, MAX( -1.0, relative2DVolume ) );
		if( relative2DVolume < 0.0f )
		{
			//Lower the 2D volume
			sound2DVolume *= 1.0f + relative2DVolume;
		}
		else
		{
			//Lower the 3D volume
			sound3DVolume *= 1.0f - relative2DVolume;
		}

		//Apply the sound volumes in the audio system now.
    TheAudio->setVolume( sound2DVolume, (AudioAffect) (AudioAffect_Sound | AudioAffect_SystemSetting) );
		TheAudio->setVolume( sound3DVolume, (AudioAffect) (AudioAffect_Sound3D | AudioAffect_SystemSetting) );

		//Save the settings in the options.ini.
    AsciiString prefString;
    prefString.format("%d", REAL_TO_INT( sound2DVolume * 100.0f ) );
    (*pref)["SFXVolume"] = prefString;
    prefString.format("%d", REAL_TO_INT( sound3DVolume * 100.0f ) );
		(*pref)["SFX3DVolume"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// slider Voice volume
	val = GadgetSliderGetPosition(sliderVoiceVolume);
	if(val != -1)
	{
    AsciiString prefString;
    prefString.format("%d", val);
    (*pref)["VoiceVolume"] = prefString;
    TheAudio->setVolume(val / 100.0f, (AudioAffect) (AudioAffect_Speech | AudioAffect_SystemSetting));
	}

	//-------------------------------------------------------------------------------------------------
	// Money tick volume
	// TheSuperHackers @todo Add options slider ?
	{
		val = pref->getMoneyTransactionVolume();
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["MoneyTransactionVolume"] = prefString;
		TheAudio->friend_getAudioSettings()->m_preferredMoneyTransactionVolume = val / 100.0f;
	}

 	//-------------------------------------------------------------------------------------------------
 	// slider Gamma
 	val = GadgetSliderGetPosition(sliderGamma);
 	if(val != -1)
 	{
		Real gammaval=1.0f;
		//generate a value between 0.6 and 2.0.
		if (val < 50)
		{
			//darker gamma
			if (val <= 0)
				gammaval = 0.6f;
			else
				gammaval=1.0f-(0.4f) * (Real)(50-val)/50.0f;
		}
		else
		if (val > 50)
			gammaval=1.0f+(1.0f) * (Real)(val-50)/50.0f;

 		AsciiString prefString;
 		prefString.format("%d", val);
 		(*pref)["Gamma"] = prefString;

		if (TheGlobalData->m_displayGamma != gammaval)
		{	TheWritableGlobalData->m_displayGamma = gammaval;
			TheDisplay->setGamma(TheGlobalData->m_displayGamma,0.0f, 1.0f, FALSE);
		}
 	}

	//-------------------------------------------------------------------------------------------------
	// Set Network Latency Font Size
	val = pref->getNetworkLatencyFontSize();
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["NetworkLatencyFontSize"] = prefString;
		TheInGameUI->refreshNetworkLatencyResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set Render FPS Font Size
	val = pref->getRenderFpsFontSize();
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["RenderFpsFontSize"] = prefString;
		TheInGameUI->refreshRenderFpsResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set System Time Font Size
	val = pref->getSystemTimeFontSize(); // TheSuperHackers @todo replace with options input when applicable
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["SystemTimeFontSize"] = prefString;
		TheInGameUI->refreshSystemTimeResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set Game Time Font Size
	val = pref->getGameTimeFontSize(); // TheSuperHackers @todo replace with options input when applicable
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["GameTimeFontSize"] = prefString;
		TheInGameUI->refreshGameTimeResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set Player Info List Font Size
	val = pref->getPlayerInfoListFontSize();
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["PlayerInfoListFontSize"] = prefString;
		TheInGameUI->refreshPlayerInfoListResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set User Font Scaling Percentage
	val = pref->getResolutionFontAdjustment() * 100.0f; // TheSuperHackers @todo replace with options input when applicable
	if (val >= 0 || val == -100)
	{
		AsciiString prefString;
		prefString.format("%d", REAL_TO_INT( val ) );
		(*pref)["ResolutionFontAdjustment"] = prefString;
		TheGlobalLanguageData->m_userResolutionFontSizeAdjustment = (Real)val / 100.0f;
	}

	//-------------------------------------------------------------------------------------------------
	// Set Money Per Minute
	{
		Bool show = pref->getShowMoneyPerMinute();
		AsciiString prefString;
		prefString = show ? "yes" : "no";
		(*pref)["ShowMoneyPerMinute"] = prefString;
		TheWritableGlobalData->m_showMoneyPerMinute = show;
	}

	//-------------------------------------------------------------------------------------------------
	// Set Game Window Transition Speed Multiplier
	{
		Real speed = pref->getGameWindowTransitionSpeedMultiplier();
		AsciiString prefString;
		prefString.format("%g", speed);
		(*pref)["GameWindowTransitionSpeedMultiplier"] = prefString;
		TheWritableGlobalData->m_gameWindowTransitionSpeedMultiplier = speed;
	}

	//-------------------------------------------------------------------------------------------------
	// Set JPEG screenshot quality
	{
		Int quality = pref->getJpegQuality();
		AsciiString prefString;
		prefString.format("%d", quality);
		(*pref)["JpegQuality"] = prefString;
		TheWritableGlobalData->m_jpegQuality = quality;
	}

	//-------------------------------------------------------------------------------------------------
	// ZeroHourReloaded Max Render FPS, DPI-Aware UI Scale & Vulkan 1.3 Remaster settings
	Bool zhrUiScaleChanged = FALSE;
	{
		if (s_zhrCurMaxFps >= RenderFpsPreset::UncappedFpsValue)
		{
			(*pref)["ZhrMaxFps"] = "uncapped";
		}
		else
		{
			AsciiString fpsStr;
			fpsStr.format("%u", s_zhrCurMaxFps);
			(*pref)["ZhrMaxFps"] = fpsStr;
		}
		applyLiveMaxRenderFps(s_zhrCurMaxFps);
		s_zhrSavedMaxFps = s_zhrCurMaxFps;

		if (s_zhrCurUiScale < -0.5f)
		{
			(*pref)["ZhrUiScale"] = "classic";
		}
		else if (s_zhrCurUiScale <= 0.01f)
		{
			(*pref)["ZhrUiScale"] = "auto";
		}
		else
		{
			AsciiString scaleStr;
			scaleStr.format("%d", REAL_TO_INT_FLOOR(s_zhrCurUiScale * 100.0f + 0.5f));
			(*pref)["ZhrUiScale"] = scaleStr;
		}
		if (TheDisplay && fabsf(s_zhrCurUiScale - s_zhrSavedUiScale) > 0.005f)
		{
			TheDisplay->setUiScalePreference(s_zhrCurUiScale);
			s_zhrSavedUiScale = s_zhrCurUiScale;
			zhrUiScaleChanged = TRUE;
		}
	}

	if (s_zhrHasVulkan && TheDisplay)
	{
		const char *modeStr =
			(s_zhrCurMode == 0u) ? "classic" :
			(s_zhrCurMode == 2u) ? "split" : "remastered";
		(*pref)["ZhrGraphicsMode"] = modeStr;
		(*pref)["ZhrPbrWaterSsao"] = s_zhrCurPbr ? "yes" : "no";
		(*pref)["ZhrHdrPostFx"] = s_zhrCurPostFx ? "yes" : "no";
		(*pref)["ZhrRemasterTextures"] = s_zhrCurRemasterTex ? "yes" : "no";
		(*pref)["ZhrMeshSmoothing"] = s_zhrCurMeshSmooth ? "yes" : "no";

		applyLiveVulkanRemasterSettings();
		s_zhrSavedMode = s_zhrCurMode;
		s_zhrSavedPbr = s_zhrCurPbr;
		s_zhrSavedPostFx = s_zhrCurPostFx;
		s_zhrSavedRemasterTex = s_zhrCurRemasterTex;
		s_zhrSavedMeshSmooth = s_zhrCurMeshSmooth;
	}

	//-------------------------------------------------------------------------------------------------
	// Resolution
	//
	// TheSuperHackers @bugfix xezon 12/06/2025 Now performs the resolution change at the very end of
	// processing all the options. This is necessary, because recreating the Shell will destroy the
	// Options Menu and therefore prevent any further ui gadget interactions afterwards.

	GadgetComboBoxGetSelectedPos( comboBoxResolution, &index );
	Int xres, yres, bitDepth;

	oldDispSettings.xRes = TheDisplay->getWidth();
	oldDispSettings.yRes = TheDisplay->getHeight();
	oldDispSettings.bitDepth = TheDisplay->getBitDepth();
	oldDispSettings.windowed = TheDisplay->getWindowed();

	if (comboBoxResolution && comboBoxResolution->winGetEnabled() && index < TheDisplay->getDisplayModeCount() && index >= 0)
	{
		TheDisplay->getDisplayModeDescription(index,&xres,&yres,&bitDepth);
		if (TheGlobalData->m_xResolution != xres || TheGlobalData->m_yResolution != yres)
		{
			if (TheDisplay->setDisplayMode(xres,yres,bitDepth,TheDisplay->getWindowed()))
			{
				dispChanged = TRUE;
				TheWritableGlobalData->m_xResolution = xres;
				TheWritableGlobalData->m_yResolution = yres;

				TheHeaderTemplateManager->onResolutionChanged();
				TheMouse->onResolutionChanged();

				//Save new settings for a dialog box confirmation after options are accepted
				newDispSettings.xRes = xres;
				newDispSettings.yRes = yres;
				newDispSettings.bitDepth = bitDepth;
				newDispSettings.windowed = TheDisplay->getWindowed();

				AsciiString prefString;
				prefString.format("%d %d", xres, yres );
				(*pref)["Resolution"] = prefString;

				TheShell->recreateWindowLayouts();

				TheInGameUI->recreateControlBar();
				TheInGameUI->refreshCustomUiResources();
			}
		}
	}

	if (!dispChanged && zhrUiScaleChanged)
	{
		if (TheDisplay)
			TheDisplay->flushFontGlyphCaches();
		if (TheHeaderTemplateManager)
			TheHeaderTemplateManager->onResolutionChanged();
		if (TheShell)
			TheShell->recreateWindowLayouts();
		if (TheInGameUI)
		{
			TheInGameUI->recreateControlBar();
			TheInGameUI->refreshCustomUiResources();
		}
		if (TheControlBar && ThePlayerList && ThePlayerList->getLocalPlayer())
		{
			TheControlBar->setControlBarSchemeByPlayer(ThePlayerList->getLocalPlayer());
			TheControlBar->markUIDirty();
		}
	}

	// MUST NEVER ADD ANOTHER OPTION HERE AT THE END !
}

static void DestroyOptionsLayout() {

	SignalUIInteraction(SHELL_SCRIPT_HOOK_OPTIONS_CLOSED);

	clearZhrOptionsWidgets();
	TheShell->destroyOptionsLayout();
	OptionsLayout = nullptr;
}

static void showAdvancedOptions()
{
	if (!WinAdvancedDisplay)
		return;
	GameWindow *parent = WinAdvancedDisplay->winGetParent();
	if (parent)
	{
		ICoord2D pSize, aSize;
		parent->winGetSize(&pSize.x, &pSize.y);
		WinAdvancedDisplay->winGetSize(&aSize.x, &aSize.y);
		WinAdvancedDisplay->winSetPosition((pSize.x - aSize.x) / 2, (pSize.y - aSize.y) / 2);
	}
	WinAdvancedDisplay->winHide(FALSE);
	WinAdvancedDisplay->winBringToTop();
}

static void acceptAdvancedOptions()
{
	WinAdvancedDisplay->winHide(TRUE);
}

static void cancelAdvancedOptions()
{
	//restore the detail selection back to initial state
	GadgetComboBoxSetSelectedPos(comboBoxDetail, (Int)TheGameLODManager->getStaticLODLevel());

	WinAdvancedDisplay->winHide(TRUE);
}

// TheSuperHackers @tweak Now prints additional version information in the version label.
static void initLabelVersion()
{
	NameKeyType versionID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:LabelVersion" );
	GameWindow *labelVersion = TheWindowManager->winGetWindowFromId( nullptr, versionID );

	if (labelVersion)
	{
		if (TheVersion && TheGlobalData)
		{
			UnicodeString text = TheVersion->getUnicodeProductVersionHashString();
			GadgetStaticTextSetText( labelVersion, text );
		}
		else
		{
			labelVersion->winHide( TRUE );
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the options menu */
//-------------------------------------------------------------------------------------------------
void OptionsMenuInit( WindowLayout *layout, void *userData )
{
	ignoreSelected = TRUE;
	clearZhrOptionsWidgets();
	if (TheGameEngine->getQuitting())
		return;
	OptionsLayout = layout;
	if (!pref)
	{
		pref = NEW OptionPreferences;
	}

	// Snapshot live Max Render FPS + Vulkan 1.3 Remaster settings for instant preview + Cancel revert
	if (TheFramePacer)
	{
		s_zhrSavedMaxFps = TheFramePacer->isActualFramesPerSecondLimitEnabled()
			? static_cast<UnsignedInt>(TheFramePacer->getFramesPerSecondLimit())
			: RenderFpsPreset::UncappedFpsValue;
	}
	else
	{
		s_zhrSavedMaxFps = pref->getZhrMaxRenderFps();
	}
	s_zhrCurMaxFps = s_zhrSavedMaxFps;
	s_zhrSavedUiScale = TheDisplay ? TheDisplay->getUiScalePreference() : pref->getZhrUiScale();
	s_zhrCurUiScale = s_zhrSavedUiScale;

	s_zhrHasVulkan = (TheDisplay && TheDisplay->hasVulkanRemasterBackend()) ? TRUE : FALSE;
	if (s_zhrHasVulkan)
	{
		TheDisplay->getVulkanRemasterSettings(
			s_zhrSavedMode,
			s_zhrSavedPbr,
			s_zhrSavedPostFx,
			s_zhrSavedRemasterTex,
			s_zhrSavedMeshSmooth);
		// If currently in Classic 2003, initialize feature toggle checkboxes from saved preferences
		// so switching back to Remastered preserves the user's feature choices.
		if (s_zhrSavedMode == 0u)
		{
			s_zhrSavedPbr = pref->getZhrPbrWaterSsaoEnabled();
			s_zhrSavedPostFx = pref->getZhrHdrPostFxEnabled();
			s_zhrSavedRemasterTex = pref->getZhrRemasterTexturesEnabled();
			s_zhrSavedMeshSmooth = pref->getZhrMeshSmoothingEnabled();
		}
		s_zhrCurMode = s_zhrSavedMode;
		s_zhrCurPbr = s_zhrSavedPbr;
		s_zhrCurPostFx = s_zhrSavedPostFx;
		s_zhrCurRemasterTex = s_zhrSavedRemasterTex;
		s_zhrCurMeshSmooth = s_zhrSavedMeshSmooth;
	}

	SignalUIInteraction(SHELL_SCRIPT_HOOK_OPTIONS_OPENED);

	comboBoxLANIPID				 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxIP" );
	comboBoxLANIP					 = TheWindowManager->winGetWindowFromId( nullptr,  comboBoxLANIPID);
	comboBoxOnlineIPID		 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxOnlineIP" );
	comboBoxOnlineIP			 = TheWindowManager->winGetWindowFromId( nullptr,  comboBoxOnlineIPID);
	checkAlternateMouseID  = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckAlternateMouse" );
	checkAlternateMouse	   = TheWindowManager->winGetWindowFromId( nullptr, checkAlternateMouseID);
	checkRetaliationID		 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:Retaliation" );
	checkRetaliation	     = TheWindowManager->winGetWindowFromId( nullptr, checkRetaliationID);
	checkDoubleClickAttackMoveID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckDoubleClickAttackMove" );
	checkDoubleClickAttackMove   = TheWindowManager->winGetWindowFromId( nullptr, checkDoubleClickAttackMoveID );
	sliderScrollSpeedID	   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderScrollSpeed" );
	sliderScrollSpeed		   = TheWindowManager->winGetWindowFromId( nullptr,  sliderScrollSpeedID);
	comboBoxAntiAliasingID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxAntiAliasing" );
	comboBoxAntiAliasing   = TheWindowManager->winGetWindowFromId( nullptr, comboBoxAntiAliasingID );
	comboBoxResolutionID   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxResolution" );
	comboBoxResolution     = TheWindowManager->winGetWindowFromId( nullptr, comboBoxResolutionID );
	comboBoxDetailID			 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxDetail" );
	comboBoxDetail		   = TheWindowManager->winGetWindowFromId( nullptr, comboBoxDetailID );

	checkLanguageFilterID  = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckLanguageFilter" );
	checkLanguageFilter    = TheWindowManager->winGetWindowFromId( nullptr, checkLanguageFilterID );
	buttonFirewallRefreshID	= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonFirewallRefresh" );
	buttonFirewallRefresh		= TheWindowManager->winGetWindowFromId( nullptr, buttonFirewallRefreshID);

#if ENABLE_GUI_HACKS
	// TheSuperHackers @tweak 26/07/2026 The Send Delay feature was obsoleted because it only worked around
	// a source port remapping bug in early 2000s Netgear firewalls. Hide the obsoleted UI element accordingly.
	GameWindow *checkSendDelay = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("OptionsMenu.wnd:CheckSendDelay"));
	if (checkSendDelay)
		checkSendDelay->winHide(TRUE);
#endif

	checkDrawAnchorID       = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxDrawAnchor" );
	checkDrawAnchor				 = TheWindowManager->winGetWindowFromId( nullptr, checkDrawAnchorID);
	checkMoveAnchorID       = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxMoveAnchor" );
	checkMoveAnchor				 = TheWindowManager->winGetWindowFromId( nullptr, checkMoveAnchorID);

	// Replay camera
	checkSaveCameraID      = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxSaveCamera" );
	checkSaveCamera        = TheWindowManager->winGetWindowFromId( nullptr, checkSaveCameraID );
	checkUseCameraID       = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxUseCamera" );
	checkUseCamera         = TheWindowManager->winGetWindowFromId( nullptr, checkUseCameraID );

//	// Speakers and 3-D Audio
//	checkAudioSurroundID   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckAudioSurround" );
//	checkAudioSurround     = TheWindowManager->winGetWindowFromId( nullptr, checkAudioSurroundID );
//	checkAudioHardwareID   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckAudioHardware" );
//	checkAudioHardware     = TheWindowManager->winGetWindowFromId( nullptr, checkAudioHardwareID );
//
	// Volume Controls
	sliderMusicVolumeID    = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderMusicVolume" );
	sliderMusicVolume      = TheWindowManager->winGetWindowFromId( nullptr, sliderMusicVolumeID );
	sliderSFXVolumeID      = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderSFXVolume" );
	sliderSFXVolume        = TheWindowManager->winGetWindowFromId( nullptr, sliderSFXVolumeID );
	sliderVoiceVolumeID    = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderVoiceVolume" );
	sliderVoiceVolume      = TheWindowManager->winGetWindowFromId( nullptr, sliderVoiceVolumeID );
 	sliderGammaID    = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderGamma" );
 	sliderGamma      = TheWindowManager->winGetWindowFromId( nullptr, sliderGammaID );

//	checkBoxLowTextureDetailID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckLowTextureDetail" );
//	checkBoxLowTextureDetail      = TheWindowManager->winGetWindowFromId( nullptr, checkBoxLowTextureDetailID );

	WinAdvancedDisplayID		= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:WinAdvancedDisplayOptions" );
	WinAdvancedDisplay      = TheWindowManager->winGetWindowFromId( nullptr, WinAdvancedDisplayID );

	ButtonAdvancedAcceptID		= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonAdvanceAccept" );
	ButtonAdvancedAccept      = TheWindowManager->winGetWindowFromId( nullptr, ButtonAdvancedAcceptID );

	ButtonAdvancedCancelID		= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonAdvanceBack" );
	ButtonAdvancedCancel      = TheWindowManager->winGetWindowFromId( nullptr, ButtonAdvancedCancelID );

	sliderTextureResolutionID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:LowResSlider" );
	sliderTextureResolution = TheWindowManager->winGetWindowFromId( nullptr, sliderTextureResolutionID );

	check3DShadowsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:Check3DShadows" );
	check3DShadows   = TheWindowManager->winGetWindowFromId( nullptr, check3DShadowsID);

	check2DShadowsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:Check2DShadows" );
	check2DShadows   = TheWindowManager->winGetWindowFromId( nullptr, check2DShadowsID);

	checkCloudShadowsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckCloudShadows" );
	checkCloudShadows   = TheWindowManager->winGetWindowFromId( nullptr, checkCloudShadowsID);

	checkGroundLightingID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckGroundLighting" );
	checkGroundLighting   = TheWindowManager->winGetWindowFromId( nullptr, checkGroundLightingID);

	checkSmoothWaterID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckSmoothWater" );
	checkSmoothWater   = TheWindowManager->winGetWindowFromId( nullptr, checkSmoothWaterID);

	checkExtraAnimationsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckExtraAnimations" );
	checkExtraAnimations   = TheWindowManager->winGetWindowFromId( nullptr, checkExtraAnimationsID);

	checkNoDynamicLodID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckNoDynamicLOD" );
	checkNoDynamicLod   = TheWindowManager->winGetWindowFromId( nullptr, checkNoDynamicLodID);

	checkHeatEffectsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckHeatEffects" );
	checkHeatEffects   = TheWindowManager->winGetWindowFromId( nullptr, checkHeatEffectsID);

	checkUnlockFpsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckUnlockFPS" );
	checkUnlockFps   = TheWindowManager->winGetWindowFromId( nullptr, checkUnlockFpsID);

	checkBuildingOcclusionID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBehindBuilding" );
	checkBuildingOcclusion   = TheWindowManager->winGetWindowFromId( nullptr, checkBuildingOcclusionID);

	checkPropsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckShowProps" );
	checkProps   = TheWindowManager->winGetWindowFromId( nullptr, checkPropsID);

	sliderParticleCapID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ParticleCapSlider" );
  sliderParticleCap = TheWindowManager->winGetWindowFromId( nullptr, sliderParticleCapID );

	WinAdvancedDisplay->winHide(TRUE);

	Color color =  GameMakeColor(255,255,255,255);

	initLabelVersion();

	// Choose an IP address, then initialize the IP combo box
	UnsignedInt selectedIP = pref->getLANIPAddress();

	UnicodeString str;
	IPEnumeration IPs;
	EnumeratedIP *IPlist = IPs.getAddresses();
	Int index;
	Int selectedIndex = -1;
	Int count = 0;
	GadgetComboBoxReset(comboBoxLANIP);
	while (IPlist)
	{
		count++;
		str.translate(IPlist->getIPstring());
		index = GadgetComboBoxAddEntry(comboBoxLANIP, str, color);
		GadgetComboBoxSetItemData(comboBoxLANIP, index, (void *)(IPlist->getIP()));
		if (selectedIP == IPlist->getIP())
		{
			selectedIndex = index;
		}
		IPlist = IPlist->getNext();
	}
	if (selectedIndex >= 0)
	{
		GadgetComboBoxSetSelectedPos(comboBoxLANIP, selectedIndex);
	}
	else
	{
		GadgetComboBoxSetSelectedPos(comboBoxLANIP, 0);
		if (IPs.getAddresses())
		{
			pref->setLANIPAddress(IPs.getAddresses()->getIPstring());
		}
	}

	// And now the GameSpy one
	if (comboBoxOnlineIP)
	{
		UnsignedInt selectedIP = pref->getOnlineIPAddress();
		UnicodeString str;
		IPEnumeration IPs;
		EnumeratedIP *IPlist = IPs.getAddresses();
		Int index;
		Int selectedIndex = -1;
		Int count = 0;
		GadgetComboBoxReset(comboBoxOnlineIP);
		while (IPlist)
		{
			count++;
			str.translate(IPlist->getIPstring());
			index = GadgetComboBoxAddEntry(comboBoxOnlineIP, str, color);
			GadgetComboBoxSetItemData(comboBoxOnlineIP, index, (void *)(IPlist->getIP()));
			if (selectedIP == IPlist->getIP())
			{
				selectedIndex = index;
			}
			IPlist = IPlist->getNext();
		}
		if (selectedIndex >= 0)
		{
			GadgetComboBoxSetSelectedPos(comboBoxOnlineIP, selectedIndex);
		}
		else
		{
			GadgetComboBoxSetSelectedPos(comboBoxOnlineIP, 0);
			if (IPs.getAddresses())
			{
				pref->setOnlineIPAddress(IPs.getAddresses()->getIPstring());
			}
		}
	}

#if ENABLE_GUI_HACKS
	// TheSuperHackers @tweak 26/07/2026 The http proxy feature was obsoleted because it did nothing for the UDP game traffic or match sockets.
	// Hide the relevant obsoleted UI elements accordingly.
	NameKeyType textEntryHTTPProxyID = TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:TextEntryHTTPProxy");
	GameWindow *textEntryHTTPProxy = TheWindowManager->winGetWindowFromId(nullptr, textEntryHTTPProxyID);
	if (textEntryHTTPProxy)
		textEntryHTTPProxy->winHide(TRUE);

	NameKeyType staticTextHTTPProxyID = TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:StaticTextHTTPProxy");
	GameWindow *staticTextHTTPProxy = TheWindowManager->winGetWindowFromId(nullptr, staticTextHTTPProxyID);
	if (staticTextHTTPProxy)
		staticTextHTTPProxy->winHide(TRUE);
#endif

	// Firewall Port Override
	GameWindow *textEntryFirewallPortOverride = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("OptionsMenu.wnd:TextEntryFirewallPortOverride"));
	if (textEntryFirewallPortOverride)
	{
			UnicodeString uStr;
			if (TheGlobalData->m_firewallPortOverride != 0)
			{	AsciiString aStr;
				aStr.format("%d",TheGlobalData->m_firewallPortOverride);
				uStr.translate(aStr);
			}
			GadgetTextEntrySetText(textEntryFirewallPortOverride,uStr);
	}

	// populate anti aliasing modes
	AsciiString selectedAliasingMode = (*pref)["AntiAliasing"];
	GadgetComboBoxReset(comboBoxAntiAliasing);
	AsciiString temp;
	Int i=0;
	for (; i < OptionPreferences::AntiAliasingMode_Count; ++i)
	{
		temp.format("GUI:AntiAliasing%d", i);
		str = TheGameText->fetch( temp );
		index = GadgetComboBoxAddEntry(comboBoxAntiAliasing, str, color);
	}
	Int val = atoi(selectedAliasingMode.str());
	Int pos = 0;

	// TheSuperHackers @info We are converting from human readable value to comboBox entry position
	val = highestBit(val);

	if (val == WW3D::MULTISAMPLE_MODE_NONE)
		pos = OptionPreferences::AntiAliasingMode_OFF;
	else if (val == WW3D::MULTISAMPLE_MODE_2X)
		pos = OptionPreferences::AntiAliasingMode_MSAA_2X;
	else if (val == WW3D::MULTISAMPLE_MODE_4X)
		pos = OptionPreferences::AntiAliasingMode_MSAA_4X;
	else if (val == WW3D::MULTISAMPLE_MODE_8X)
		pos = OptionPreferences::AntiAliasingMode_MSAA_8X;

	if (val < 0 || val > WW3D::MULTISAMPLE_MODE_8X)
	{
		TheWritableGlobalData->m_antiAliasLevel = pos = 0;
	}
	GadgetComboBoxSetSelectedPos(comboBoxAntiAliasing, pos);

	// get resolution from saved preferences file
	AsciiString selectedResolution = (*pref) ["Resolution"];
	Int selectedXRes=DEFAULT_DISPLAY_WIDTH;
	Int selectedYRes=DEFAULT_DISPLAY_HEIGHT;
	Int selectedResIndex=-1;
	if (!selectedResolution.isEmpty())
	{
		//try to parse 2 integers out of string
		if (sscanf(selectedResolution.str(),"%d%d", &selectedXRes, &selectedYRes) != 2)
		{
			selectedXRes=DEFAULT_DISPLAY_WIDTH;
			selectedYRes=DEFAULT_DISPLAY_HEIGHT;
		}
	}

	// populate resolution modes
	GadgetComboBoxReset(comboBoxResolution);
	Int numResolutions = TheDisplay->getDisplayModeCount();
	UnsignedInt displayWidth = TheDisplay->getWidth();
	UnsignedInt displayHeight = TheDisplay->getHeight();

	for( i = 0; i < numResolutions; ++i )
	{	Int xres,yres,bitDepth;
		TheDisplay->getDisplayModeDescription(i,&xres,&yres,&bitDepth);
		str.format(L"%d x %d",xres,yres);
		GadgetComboBoxAddEntry( comboBoxResolution, str, color);
		// TheSuperHackers @bugfix xezon 12/06/2025 Now makes a selection with the active display resolution
		// instead of the resolution read from the Option Preferences, because the active display resolution
		// is the most relevant to make a selection with and the Option Preferences could be wrong.
		if ( xres == displayWidth && yres == displayHeight )
			selectedResIndex=i;
	}

	if (selectedResIndex == -1)
	{
		// TheSuperHackers @bugfix xezon 08/06/2025 Now adds the current resolution instead of defaulting to 800 x 600.
		// This avoids force changing the resolution when the user has set a custom resolution in the Option Preferences.
		Int xres = displayWidth;
		Int yres = displayHeight;
		str.format(L"%d x %d",xres,yres);
		GadgetComboBoxAddEntry( comboBoxResolution, str, color );
		selectedResIndex = GadgetComboBoxGetLength( comboBoxResolution ) - 1;
	}

	GadgetComboBoxSetSelectedPos( comboBoxResolution, selectedResIndex );

	// set the display detail
	// TheSuperHackers @tweak xezon 24/09/2025 The Detail Combo Box now has the same value order as StaticGameLODLevel for simplicity.
	// TheSuperHackers @feature xezon 24/09/2025 The Detail Combo Box now has a new options for "Very High".
	GadgetComboBoxReset(comboBoxDetail);
#if ENABLE_GUI_HACKS
	// TheSuperHackers @tweak xezon 24/09/2025 Show max 4 rows because with the original layout it cannot possibly show 5.
	GadgetComboBoxSetMaxDisplay(comboBoxDetail, 4);
#endif
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:Low"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:Medium"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:High"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->FETCH_OR_SUBSTITUTE("GUI:VeryHigh", L"Very High"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:Custom"), color);
	static_assert(STATIC_GAME_LOD_COUNT == 5, "Wrong combo box count");

	//Check if level was never set and default to setting most suitable for system.
	if (TheGameLODManager->getStaticLODLevel() == STATIC_GAME_LOD_UNKNOWN)
	{
		TheGameLODManager->setStaticLODLevel(TheGameLODManager->getRecommendedStaticLODLevel());
	}

	GadgetComboBoxSetSelectedPos(comboBoxDetail, (Int)TheGameLODManager->getStaticLODLevel());

	GadgetSliderSetPosition( sliderTextureResolution, 2-WW3D::Get_Texture_Reduction());

	GadgetCheckBoxSetChecked( check3DShadows, TheGlobalData->m_useShadowVolumes);

	GadgetCheckBoxSetChecked( check2DShadows, TheGlobalData->m_useShadowDecals);

	GadgetCheckBoxSetChecked( checkCloudShadows, TheGlobalData->m_useCloudMap);

	GadgetCheckBoxSetChecked( checkGroundLighting, TheGlobalData->m_useLightMap);

	GadgetCheckBoxSetChecked( checkSmoothWater, TheGlobalData->m_showSoftWaterEdge);

	GadgetCheckBoxSetChecked( checkExtraAnimations, !TheGlobalData->m_useDrawModuleLOD);

	GadgetCheckBoxSetChecked( checkNoDynamicLod, !TheGlobalData->m_enableDynamicLOD);

	GadgetCheckBoxSetChecked( checkHeatEffects, TheGlobalData->m_useHeatEffects);

	GadgetCheckBoxSetChecked( checkUnlockFps, !TheGlobalData->m_useFpsLimit);

	GadgetCheckBoxSetChecked( checkBuildingOcclusion, TheGlobalData->m_enableBehindBuildingMarkers);

	GadgetCheckBoxSetChecked( checkProps, TheGlobalData->m_useTrees);
	//checkProps->winEnable(false);	//gray out the option for now.

	GadgetSliderSetPosition( sliderParticleCap, TheGlobalData->m_maxParticleCount );

	//set language filter
	AsciiString languageFilter = (*pref)["LanguageFilter"];
	if (languageFilter == "true" )
	{
		GadgetCheckBoxSetChecked( checkLanguageFilter, true);
		TheWritableGlobalData->m_languageFilterPref = true;
	}
	else
	{
		GadgetCheckBoxSetChecked( checkLanguageFilter, false);
		TheWritableGlobalData->m_languageFilterPref = false;
	}

	//set replay camera
	if (pref->saveCameraInReplays())
	{
		GadgetCheckBoxSetChecked( checkSaveCamera, true);
		TheWritableGlobalData->m_saveCameraInReplay = true;
	}
	else
	{
		GadgetCheckBoxSetChecked( checkSaveCamera, false);
		TheWritableGlobalData->m_saveCameraInReplay = false;
	}
	if (pref->useCameraInReplays())
	{
		GadgetCheckBoxSetChecked( checkUseCamera, true);
		TheWritableGlobalData->m_useCameraInReplay = true;
	}
	else
	{
		GadgetCheckBoxSetChecked( checkUseCamera, false);
		TheWritableGlobalData->m_useCameraInReplay = false;
	}

	//set scroll options
	AsciiString test = (*pref)["DrawScrollAnchor"];
	DEBUG_LOG(("DrawScrollAnchor == [%s]", test.str()));
	if (test == "yes" || (test.isEmpty() && TheInGameUI->getDrawRMBScrollAnchor()))
	{
		GadgetCheckBoxSetChecked( checkDrawAnchor, true);
		TheInGameUI->setDrawRMBScrollAnchor(true);
	}
	else
	{
		GadgetCheckBoxSetChecked( checkDrawAnchor, false);
		TheInGameUI->setDrawRMBScrollAnchor(false);
	}
	test = (*pref)["MoveScrollAnchor"];
	DEBUG_LOG(("MoveScrollAnchor == [%s]", test.str()));
	if (test == "yes" || (test.isEmpty() && TheInGameUI->getMoveRMBScrollAnchor()))
	{
		GadgetCheckBoxSetChecked( checkMoveAnchor, true);
		TheInGameUI->setMoveRMBScrollAnchor(true);
	}
	else
	{
		GadgetCheckBoxSetChecked( checkMoveAnchor, false);
		TheInGameUI->setMoveRMBScrollAnchor(false);
	}

//	// Audio Init shiznat
//	GadgetCheckBoxSetChecked(checkAudioHardware, TheAudio->getHardwareAccelerated());
//	GadgetCheckBoxSetChecked(checkAudioSurround, TheAudio->getSpeakerSurround());

	// set the mouse mode
	GadgetCheckBoxSetChecked(checkAlternateMouse, TheGlobalData->m_useAlternateMouse);
	GadgetCheckBoxSetChecked(checkRetaliation, TheGlobalData->m_clientRetaliationModeEnabled);
	GadgetCheckBoxSetChecked( checkDoubleClickAttackMove, TheGlobalData->m_doubleClickAttackMove );

	// set scroll speed slider
	// TheSuperHackers @tweak xezon 11/07/2025 No longer sets the slider position if the user setting
	// is set beyond the slider limits. This gives the user more freedom to customize the scroll
	// speed. The slider value remains 0.
	Int scrollPos = (Int)(TheGlobalData->m_keyboardScrollFactor*100.0f);
	Int scrollMin, scrollMax;
	GadgetSliderGetMinMax( sliderScrollSpeed, &scrollMin, &scrollMax );
	if (scrollPos >= scrollMin && scrollPos <= scrollMax)
	{
		GadgetSliderSetPosition( sliderScrollSpeed, scrollPos );
	}
	DEBUG_LOG(("Scroll Speed %d", scrollPos));

 	// set volume sliders

	// set music volume slider
	GadgetSliderSetPosition( sliderMusicVolume, REAL_TO_INT(pref->getMusicVolume()) );

	//set SFX volume slider
	Real maxVolume = MAX( pref->getSoundVolume(), pref->get3DSoundVolume() );
	GadgetSliderSetPosition( sliderSFXVolume, REAL_TO_INT( maxVolume ) );

	//set voice volume slider
	GadgetSliderSetPosition( sliderVoiceVolume, REAL_TO_INT(pref->getSpeechVolume()) );

	// set the gamma slider
 	GadgetSliderSetPosition( sliderGamma, REAL_TO_INT(pref->getGammaValue()) );

	// show menu
	layout->hide( FALSE );

	// set keyboard focus to main parent
	NameKeyType parentID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:OptionsMenuParent" );
	GameWindow *parent = TheWindowManager->winGetWindowFromId( nullptr, parentID );
	TheWindowManager->winSetFocus( parent );

	if( (TheGameLogic->isInGame() && TheGameLogic->getGameMode() != GAME_SHELL) || TheGameSpyInfo )
	{
		// disable controls that you can't change the options for in game
		comboBoxLANIP->winEnable(FALSE);

		if (comboBoxOnlineIP)
			comboBoxOnlineIP->winEnable(FALSE);

		buttonFirewallRefresh->winEnable(FALSE);

		if (comboBoxDetail)
			comboBoxDetail->winEnable(FALSE);

		if (comboBoxResolution)
			comboBoxResolution->winEnable(FALSE);

		if (textEntryFirewallPortOverride)
			textEntryFirewallPortOverride->winEnable(FALSE);

//		if (checkAudioSurround)
//			checkAudioSurround->winEnable(FALSE);
//
//		if (checkAudioHardware)
//			checkAudioHardware->winEnable(FALSE);
	}

	buildModernTabbedOptionsLayout(parent);

	TheWindowManager->winSetModal(parent);
	ignoreSelected = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** options menu shutdown method */
//-------------------------------------------------------------------------------------------------
void OptionsMenuShutdown( WindowLayout *layout, void *userData )
{
/* moved into the back button stuff
	if (pref)
	{
		pref->write();
		delete pref;
		pref = nullptr;
	}

	comboBoxIP = nullptr;

	// hide menu
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout );
*/

}

//-------------------------------------------------------------------------------------------------
/** options menu update method */
//-------------------------------------------------------------------------------------------------
void OptionsMenuUpdate( WindowLayout *layout, void *userData )
{

}

//-------------------------------------------------------------------------------------------------
/** Options menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType OptionsMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonBack" );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonID );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
/** options menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType OptionsMenuSystem( GameWindow *window, UnsignedInt msg,
																				WindowMsgData mData1, WindowMsgData mData2 )
{
	static NameKeyType buttonBack = NAMEKEY_INVALID;
	static NameKeyType buttonDefaults = NAMEKEY_INVALID;
	static NameKeyType buttonAccept = NAMEKEY_INVALID;
	static NameKeyType buttonReplayMenu = NAMEKEY_INVALID;
	static NameKeyType buttonKeyboardOptionsMenu = NAMEKEY_INVALID;

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{

			// get ids for our children controls
			buttonBack = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonBack" );
			buttonDefaults = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonDefaults" );
			buttonAccept = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonAccept" );
			buttonKeyboardOptionsMenu = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonKeyboardOptions" );

			break;

		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{
			clearZhrOptionsWidgets();
			break;

		}

		// --------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			return MSG_HANDLED;

		}

		//---------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				if(ignoreSelected)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (controlID == comboBoxDetailID)
				{
					Int index;
					GadgetComboBoxGetSelectedPos( comboBoxDetail, &index );
					if(index != STATIC_GAME_LOD_CUSTOM)
						break;

					showAdvancedOptions();
				}
				else if (controlID == s_zhrComboMaxFpsID)
				{
					Int index = 3;
					GadgetComboBoxGetSelectedPos(s_zhrComboMaxFps, &index);
					s_zhrCurMaxFps = zhrComboIdxToFps(index);
					applyLiveMaxRenderFps(s_zhrCurMaxFps);
				}
				else if (controlID == s_zhrComboUiScaleID)
				{
					Int index = 0;
					GadgetComboBoxGetSelectedPos(s_zhrComboUiScale, &index);
					s_zhrCurUiScale = zhrComboIdxToUiScale(index);
				}
				else if (controlID == s_zhrComboVisualModeID && s_zhrHasVulkan)
				{
					Int index = 0;
					GadgetComboBoxGetSelectedPos(s_zhrComboVisualMode, &index);
					s_zhrCurMode = zhrComboIdxToMode(index);
					applyLiveVulkanRemasterSettings();
				}
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			if(ignoreSelected)
				break;
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if( controlID == buttonBack )
			{
				// Revert live Max Render FPS & Vulkan 1.3 Remaster preview back to the snapshot taken on open
				applyLiveMaxRenderFps(s_zhrSavedMaxFps);
				if (s_zhrHasVulkan && TheDisplay)
				{
					TheDisplay->setVulkanRemasterSettings(
						s_zhrSavedMode,
						s_zhrSavedPbr,
						s_zhrSavedPostFx,
						s_zhrSavedRemasterTex,
						s_zhrSavedMeshSmooth);
				}

				delete pref;
				pref = nullptr;

				comboBoxLANIP = nullptr;
				comboBoxOnlineIP = nullptr;

				if(GameSpyIsOverlayOpen(GSOVERLAY_OPTIONS))
					GameSpyCloseOverlay(GSOVERLAY_OPTIONS);
				else
				{
					DestroyOptionsLayout();
				}

			}
			else if (controlID == buttonAccept )
			{
				saveOptions();

				if (pref)
				{
					pref->write();
					delete pref;
					pref = nullptr;
				}

				comboBoxLANIP = nullptr;
				comboBoxOnlineIP = nullptr;

				if(!TheGameLogic->isInGame() || TheGameLogic->isInShellGame())
					destroyQuitMenu(); // if we're in a game, the change res then enter the same kind of game, we nee the quit menu to be gone.


				if(GameSpyIsOverlayOpen(GSOVERLAY_OPTIONS))
					GameSpyCloseOverlay(GSOVERLAY_OPTIONS);
				else
				{
					DestroyOptionsLayout();
					if (dispChanged)
					{
						DoResolutionDialog();
					}
				}

			}
			else if (controlID == buttonDefaults )
			{
				setDefaults();
			}
			else if (controlID == s_zhrTabIDs[ZHR_TAB_GRAPHICS])
			{
				selectZhrOptionsTab(ZHR_TAB_GRAPHICS);
			}
			else if (controlID == s_zhrTabIDs[ZHR_TAB_AUDIO])
			{
				selectZhrOptionsTab(ZHR_TAB_AUDIO);
			}
			else if (controlID == s_zhrTabIDs[ZHR_TAB_CONTROLS])
			{
				selectZhrOptionsTab(ZHR_TAB_CONTROLS);
			}
			else if (controlID == s_zhrTabIDs[ZHR_TAB_NETWORK])
			{
				selectZhrOptionsTab(ZHR_TAB_NETWORK);
			}
			else if (controlID == s_zhrBtnCustomDetailID)
			{
				if (comboBoxDetail && comboBoxDetail->winGetEnabled())
				{
					ignoreSelected = TRUE;
					GadgetComboBoxSetSelectedPos(comboBoxDetail, STATIC_GAME_LOD_CUSTOM);
					ignoreSelected = FALSE;
					showAdvancedOptions();
				}
			}
			else if (controlID == s_zhrCheckPbrID && s_zhrHasVulkan)
			{
				s_zhrCurPbr = GadgetCheckBoxIsChecked(control);
				if (s_zhrCurPbr && s_zhrCurMode == 0u)
				{
					s_zhrCurMode = 1u;
					syncVulkanControlsFromState();
				}
				applyLiveVulkanRemasterSettings();
			}
			else if (controlID == s_zhrCheckPostFxID && s_zhrHasVulkan)
			{
				s_zhrCurPostFx = GadgetCheckBoxIsChecked(control);
				if (s_zhrCurPostFx && s_zhrCurMode == 0u)
				{
					s_zhrCurMode = 1u;
					syncVulkanControlsFromState();
				}
				applyLiveVulkanRemasterSettings();
			}
			else if (controlID == s_zhrCheckRemasterTexID && s_zhrHasVulkan)
			{
				s_zhrCurRemasterTex = GadgetCheckBoxIsChecked(control);
				if (s_zhrCurRemasterTex && s_zhrCurMode == 0u)
				{
					s_zhrCurMode = 1u;
					syncVulkanControlsFromState();
				}
				applyLiveVulkanRemasterSettings();
			}
			else if (controlID == s_zhrCheckMeshSmoothID && s_zhrHasVulkan)
			{
				s_zhrCurMeshSmooth = GadgetCheckBoxIsChecked(control);
				if (s_zhrCurMeshSmooth && s_zhrCurMode == 0u)
				{
					s_zhrCurMode = 1u;
					syncVulkanControlsFromState();
				}
				applyLiveVulkanRemasterSettings();
			}
			else if (controlID == ButtonAdvancedAcceptID )
			{
				acceptAdvancedOptions();

			}
			else if (controlID == ButtonAdvancedCancelID )
			{
				cancelAdvancedOptions();
			}
			else if ( controlID == buttonKeyboardOptionsMenu )
			{
				TheShell->push( "Menus/KeyboardOptionsMenu.wnd" );
			}
			else if(controlID == checkDrawAnchorID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheInGameUI->setDrawRMBScrollAnchor(true);
          	(*pref)["DrawScrollAnchor"] = "yes";
        }
				else
        {
          	TheInGameUI->setDrawRMBScrollAnchor(false);
          	(*pref)["DrawScrollAnchor"] = "no";
        }
      }
			else if(controlID == checkMoveAnchorID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheInGameUI->setMoveRMBScrollAnchor(true);
          	(*pref)["MoveScrollAnchor"] = "yes";
        }
				else
        {
          	TheInGameUI->setMoveRMBScrollAnchor(false);
          	(*pref)["MoveScrollAnchor"] = "no";
        }
      }
			else if(controlID == checkSaveCameraID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheWritableGlobalData->m_saveCameraInReplay = true;
          	(*pref)["SaveCameraInReplays"] = "yes";
        }
				else
        {
          	TheWritableGlobalData->m_saveCameraInReplay = false;
          	(*pref)["SaveCameraInReplays"] = "no";
        }
      }
			else if(controlID == checkUseCameraID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheWritableGlobalData->m_useCameraInReplay = true;
          	(*pref)["UseCameraInReplays"] = "yes";
        }
				else
        {
          	TheWritableGlobalData->m_useCameraInReplay = false;
          	(*pref)["UseCameraInReplays"] = "no";
        }
      }
			else if (controlID == buttonFirewallRefreshID)
			{
				// setting the behavior to unknown will force the firewall helper to detect the firewall behavior
				// the next time we log into gamespy/WOL/whatever.
				char num[16];
				num[0] = 0;
				TheWritableGlobalData->m_firewallBehavior = FirewallHelperClass::FIREWALL_TYPE_UNKNOWN;
				itoa(TheGlobalData->m_firewallBehavior, num, 10);
				AsciiString numstr;
				numstr = num;
				(*pref)["FirewallBehavior"] = numstr;
			}
			break;

		}

		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}
