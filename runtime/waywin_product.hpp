// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <SDL3/SDL.h>
#include <freerdp/freerdp.h>
#include <atomic>
#include "waywin_wire.hpp"
class SdlContext;
extern std::atomic<bool> waywinRecovering;
extern std::atomic<unsigned> waywinEpoch;
void waywinStartCommands();
void waywinRegister();
void waywinTick(SdlContext*);
bool waywinInput(const SDL_Event&);
bool waywinReconnect(freerdp*);
void waywinTrack(SDL_Window*,SDL_Renderer*,SDL_Texture*);
void waywinForget(SDL_Renderer*);
void waywinEmit(WayWinWire::Message);
