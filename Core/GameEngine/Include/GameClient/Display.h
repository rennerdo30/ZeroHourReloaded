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

// FILE: Display.h ////////////////////////////////////////////////////////////
// The graphics display singleton
// Author: Michael S. Booth, March 2001

#pragma once

#include "Common/SubsystemInterface.h"
#include "GameClient/Color.h"
#include "GameClient/GameFont.h"
#include "GameClient/View.h"

enum ScreenshotFormat
{
	SCREENSHOT_JPEG,
	SCREENSHOT_PNG,

	SCREENSHOT_FORMAT_COUNT
};

constexpr const Int DEFAULT_JPEG_QUALITY = 80;

struct ShroudLevel
{
	Short m_currentShroud;		///< A Value of 1 means shrouded.  0 is not.  Negative is the count of people looking.
	Short m_activeShroudLevel;///< A Value of 0 means passive shroud.  Positive is the count of people shrouding.
};

class VideoBuffer;
class VideoStreamInterface;
class DebugDisplayInterface;
class Radar;
class Image;
class DisplayString;
enum StaticGameLODLevel CPP_11(: Int);
/**
 * The Display class implements the Display interface
 */
class Display : public SubsystemInterface
{

public:
	enum DrawImageMode
	{
		DRAW_IMAGE_SOLID,
		DRAW_IMAGE_GRAYSCALE,		//draw image without blending and ignoring alpha
		DRAW_IMAGE_ALPHA,		//alpha blend the image into frame buffer
		DRAW_IMAGE_ADDITIVE	//additive blend the image into frame buffer
	};

	typedef void (DebugDisplayCallback)( DebugDisplayInterface *debugDisplay, void *userData, FILE *fp );

	Display();
	virtual ~Display() override;

	virtual void init() override { };																///< Initialize
	virtual void reset() override;																		///< Reset system
	virtual void update() override;																	///< Update system

	//---------------------------------------------------------------------------------------
	// Display attribute methods
	virtual void setWidth( UnsignedInt width );										///< Sets the width of the display
	virtual void setHeight( UnsignedInt height );									///< Sets the height of the display
	virtual UnsignedInt getWidth() { return m_width; }			///< Returns the width of the display
	virtual UnsignedInt getHeight() { return m_height; }		///< Returns the height of the display
	virtual void setBitDepth( UnsignedInt bitDepth ) { m_bitDepth = bitDepth; }
	virtual UnsignedInt getBitDepth() { return m_bitDepth; }
	virtual void setWindowed( Bool windowed ) { m_windowed = windowed; }  ///< set windowed/fullscreen flag
	virtual Bool getWindowed() { return m_windowed; }				///< return widowed/fullscreen flag
	virtual Bool setDisplayMode( UnsignedInt xres, UnsignedInt yres, UnsignedInt bitdepth, Bool windowed );	///<sets screen resolution/mode
	virtual Int getDisplayModeCount() {return 0;}	///<return number of display modes/resolutions supported by video card.
	virtual void getDisplayModeDescription(Int modeIndex, Int *xres, Int *yres, Int *bitDepth) {}	///<return description of mode
 	virtual void setGamma(Real gamma, Real bright, Real contrast, Bool calibrate) {};
	virtual Bool testMinSpecRequirements(Bool *videoPassed, Bool *cpuPassed, Bool *memPassed,StaticGameLODLevel *idealVideoLevel=nullptr, Real *cpuTime=nullptr) {*videoPassed=*cpuPassed=*memPassed=true; return true;}
	virtual void doSmartAssetPurgeAndPreload(const char* usageFileName) = 0;
#if defined(RTS_DEBUG)
	virtual void dumpAssetUsage(const char* mapname) = 0;
#endif

	//---------------------------------------------------------------------------------------
	// View management
	virtual void attachView( View *view );												///< Attach the given view to the world
	virtual View *getFirstView() { return m_viewList; }				///< Return the first view of the world
	virtual View *getNextView( View *view )
	{
		if( view )
			return view->getNextView();
		return nullptr;
	}

	virtual void drawViews();																///< Render all views of the world
	virtual void updateViews ();															///< Updates state of world views
	virtual void stepViews(); ///< Update views for every fixed time step

	virtual VideoBuffer*	createVideoBuffer() = 0;							///< Create a video buffer that can be used for this display

	//---------------------------------------------------------------------------------------
	// Drawing management
	virtual void setClipRegion( IRegion2D *region ) = 0;	///< Set clip rectangle for 2D draw operations.
	virtual	Bool isClippingEnabled() = 0;
	virtual	void enableClipping( Bool onoff ) = 0;

	// TheSuperHackers @performance Batching 2D draw operations to reduce state changes and draw call overhead.
	virtual void beginBatch(); 									///< start batching 2D draw operations.
	virtual void endBatch();   									///< stop batching and flush pending 2D draw operations.
	virtual void flush();      									///< flush pending 2D draw operations without ending the batch.
	virtual Bool isBatching() const { return m_isBatching; }	///< returns true if currently batching 2D draw operations.

	virtual void step() {}; ///< Do one fixed time step
	virtual void draw() override;																		///< Redraw the entire display
	virtual void setTimeOfDay( TimeOfDay tod ) = 0;								///< Set the time of day for this display
	virtual void createLightPulse( const Coord3D *pos, const RGBColor *color, Real innerRadius,Real attenuationWidth,
																 UnsignedInt increaseFrameTime, UnsignedInt decayFrameTime//, Bool donut = FALSE
																 ) = 0;

	/// draw a line on the display in pixel coordinates with the specified color
	virtual void drawLine( Int startX, Int startY, Int endX, Int endY,
												 Real lineWidth, UnsignedInt lineColor ) = 0;
	/// draw a line on the display in pixel coordinates with the specified 2 colors
	virtual void drawLine( Int startX, Int startY, Int endX, Int endY,
												 Real lineWidth, UnsignedInt lineColor1, UnsignedInt lineColor2 ) = 0;
	/// draw a rect border on the display in pixel coordinates with the specified color
	virtual void drawOpenRect( Int startX, Int startY, Int width, Int height,
														 Real lineWidth, UnsignedInt lineColor ) = 0;
	/// draw a filled rect on the display in pixel coords with the specified color
	virtual void drawFillRect( Int startX, Int startY, Int width, Int height,
														 UnsignedInt color ) = 0;

	/// Draw a percentage of a rectangle, much like a clock
	virtual void drawRectClock(Int startX, Int startY, Int width, Int height, Int percent, UnsignedInt color) = 0;
	virtual void drawRemainingRectClock(Int startX, Int startY, Int width, Int height, Int percent, UnsignedInt color) = 0;

	/// draw an image fit within the screen coordinates
	virtual void drawImage( const Image *image, Int startX, Int startY,
													Int endX, Int endY, Color color = 0xFFFFFFFF, DrawImageMode mode=DRAW_IMAGE_ALPHA) = 0;

	/// draw a video buffer fit within the screen coordinates
	virtual void drawScaledVideoBuffer( VideoBuffer *buffer, VideoStreamInterface *stream ) = 0;
	virtual void drawVideoBuffer( VideoBuffer *buffer, Int startX, Int startY,
													Int endX, Int endY ) = 0;

	/// FullScreen video playback
	virtual void playMovie( AsciiString movieName );
	virtual void stopMovie();
	virtual Bool isMoviePlaying();

	/// Register debug display callback
	virtual void setDebugDisplayCallback( DebugDisplayCallback *callback, void *userData = nullptr  );
	virtual DebugDisplayCallback *getDebugDisplayCallback();

	virtual void setShroudLevel(Int x, Int y, CellShroudStatus setting ) = 0;	  ///< set shroud
	virtual void clearShroud() = 0;														///< empty the entire shroud
	virtual void setBorderShroudLevel(UnsignedByte level) = 0;	///<color that will appear in unused border terrain.

#if defined(RTS_DEBUG)
	virtual void dumpModelAssets(const char *path) = 0;	///< dump all used models/textures to a file.
#endif
	virtual void preloadModelAssets( AsciiString model ) = 0;	///< preload model asset
	virtual void preloadTextureAssets( AsciiString texture ) = 0;	///< preload texture asset

	virtual void takeScreenShot(ScreenshotFormat format, Int jpegQuality = DEFAULT_JPEG_QUALITY) = 0;			///< saves screenshot in specified format
	virtual void toggleMovieCapture() = 0;							///< starts saving frames to an avi or frame sequence
	virtual void toggleLetterBox() = 0;										///< enabled letter-boxed display
	virtual void enableLetterBox(Bool enable) = 0;						///< forces letter-boxed display on/off
	virtual Bool isLetterBoxFading() { return FALSE; }	///< returns true while letterbox fades in/out
	virtual Bool isLetterBoxed() { return FALSE; }	//WST 10/2/2002. Added query interface

	virtual void setCinematicText( AsciiString string ) { m_cinematicText = string; }
	virtual void setCinematicFont( GameFont *font ) { m_cinematicFont = font; }
	virtual void setCinematicTextFrames( Int frames ) { m_cinematicTextFrames = frames; }

	virtual Real getAverageFPS() = 0;	///< returns the average FPS.
	virtual Real getLow1PercentFPS() = 0;	///< returns the 1% low FPS.
	virtual Real getCurrentFPS() = 0;	///< returns the current FPS.
	virtual Int getLastFrameDrawCalls() = 0;  ///< returns the number of draw calls issued in the previous frame

	// ZeroHourReloaded Vulkan 1.3 Remaster & DPI-Aware UI Scaling bridge
	enum UiScaleAnchor
	{
		UI_ANCHOR_STRETCH = 0,       ///< Full-screen stretch (loading screens / Classic Stretched mode)
		UI_ANCHOR_CENTER,            ///< Aspect-correct uniform scale, centered on screen
		UI_ANCHOR_BOTTOM_CENTER,     ///< Aspect-correct uniform scale, anchored bottom-center (ControlBar)
		UI_ANCHOR_RIGHT_CENTER,      ///< Aspect-correct uniform scale, anchored right-center (MainMenu / Shortcuts)
	};

	void setDpiScale(Real scale);
	Real getDpiScale() const { return m_dpiScale; }
	/// 0.0f = Auto (DPI-Aware), -1.0f = Classic 2003 stretched, [0.55..1.00] = explicit height fraction.
	void setUiScalePreference(Real pref);
	Real getUiScalePreference() const { return m_uiScalePref; }
	Bool isUiScaleClassicStretched() const { return m_uiScalePref < -0.5f; }
	/// Effective virtual 4:3 UI height fraction of the display (0.55..1.00).
	Real getEffectiveUiScale() const;
	/// Computes screen-space scale + origin offset mapping a (createW x createH) UI canvas onto the display.
	void getUiTransform(
		Int createW,
		Int createH,
		UiScaleAnchor anchor,
		Real &outScaleX,
		Real &outScaleY,
		Real &outOffsetX,
		Real &outOffsetY) const;
	virtual void flushFontGlyphCaches() {}

	virtual Bool hasVulkanRemasterBackend() const { return FALSE; }
	virtual void getVulkanRemasterSettings(UnsignedInt &mode, Bool &pbrWaterSsao, Bool &hdrPostFx, Bool &remasterTextures, Bool &meshSmoothing) const
	{
		mode = 0;
		pbrWaterSsao = FALSE;
		hdrPostFx = FALSE;
		remasterTextures = FALSE;
		meshSmoothing = FALSE;
	}
	virtual void setVulkanRemasterSettings(UnsignedInt mode, Bool pbrWaterSsao, Bool hdrPostFx, Bool remasterTextures, Bool meshSmoothing)
	{
		(void)mode; (void)pbrWaterSsao; (void)hdrPostFx; (void)remasterTextures; (void)meshSmoothing;
	}

protected:
	virtual void onBeginBatch() { }
	virtual void onEndBatch() { }
	virtual void onFlush() { }

	virtual void deleteViews();   ///< delete all views
	UnsignedInt m_width, m_height;			///< Dimensions of the display
	UnsignedInt m_bitDepth;							///< bit depth of the display
	Bool m_windowed;										///< TRUE when windowed, FALSE when fullscreen
	Bool m_isBatching;
	Real m_dpiScale;										///< OS display DPI scale factor (1.0, 1.25, 1.5, 2.0...)
	Real m_uiScalePref;									///< 0.0=Auto DPI-Aware, -1.0=Classic Stretched, else [0.55..1.00]
	View *m_viewList;										///< All of the views into the world

	// Cinematic text data
	AsciiString m_cinematicText;        ///< string of the cinematic text that should be displayed
	GameFont *m_cinematicFont;           ///< font for cinematic text
	Int m_cinematicTextFrames;          ///< count of how long the cinematic text should be displayed

	// Video playback data
	VideoBuffer						*m_videoBuffer;						///< Video playback buffer
	VideoStreamInterface	*m_videoStream;						///< Video stream;
	AsciiString						 m_currentlyPlayingMovie;	///< The currently playing video. Used to notify TheScriptEngine of completed videos.

	// Debug display data
	DebugDisplayInterface *m_debugDisplay;					///< Actual debug display
	DebugDisplayCallback	*m_debugDisplayCallback;	///< Code to update the debug display
	void									*m_debugDisplayUserData;	///< Data for debug display update handler
	Real	m_letterBoxFadeLevel;	///<tracks the current alpha level for fading letter-boxed mode in/out.
	Bool	m_letterBoxEnabled;		///<current state of letterbox
	UnsignedInt	m_letterBoxFadeStartTime;		///< time of letterbox fade start
};

// the singleton
extern Display *TheDisplay;

extern void StatDebugDisplay( DebugDisplayInterface *dd, void *, FILE *fp = nullptr );

//Necessary for display resolution confirmation dialog box
//Holds the previous and current display settings
typedef struct _DisplaySettings
{
	Int xRes;  //Resolution width
	Int yRes;  //Resolution height
	Int bitDepth; //Color Depth
	Bool windowed; //Window mode TRUE: we're windowed, FALSE: we're not windowed
} DisplaySettings;
