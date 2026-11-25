/*
**	ZeroHourReloaded — Cross-Platform SDL3 Keyboard (replaces DirectInput8)
*/

#pragma once

#include "GameClient/Keyboard.h"
#include <cstdint>

class SDL3Keyboard : public Keyboard
{
public:
	SDL3Keyboard();
	virtual ~SDL3Keyboard() override;

	virtual void init() override;
	virtual void reset() override;
	virtual void update() override;
	virtual Bool getCapsState() override;

	/// Enqueues an SDL3 key transition (`sdlScancode` = SDL_Scancode).
	static void pushSDLEvent(uint32_t sdlScancode, Bool isDown, Bool isRepeat);

protected:
	virtual void getKey(KeyboardIO* key) override;
};
