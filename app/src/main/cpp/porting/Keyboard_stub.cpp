// Android build: minimal stub for common/Keyboard's instance-based API.
// common/Keyboard.cpp itself (a live SDL_Event-polling keyboard-state
// tracker) is deliberately not compiled - see the CMakeLists.txt comment
// and the porting plan - since Android input needs its own touch-driven
// design, not a port of desktop keyboard polling. But engine/GameState.cpp
// calls Keyboard::instance() unconditionally (not S3D_SERVER-guarded) as
// part of its menu-state key-binding transitions, and --whole-archive
// linking (needed for weapon/accessory self-registration, see
// CMakeLists.txt) pulls that code in regardless of whether it's ever
// exercised at runtime. These stubs report "no keys/history" so that game
// states with key-based transitions simply never match one, until real
// Android input replaces this.
#include <common/Keyboard.hpp>

Keyboard *Keyboard::instance_ = nullptr;

Keyboard *Keyboard::instance()
{
	if (!instance_) instance_ = new Keyboard();
	return instance_;
}

Keyboard::Keyboard() : keybHistCnt_(0), mHighSurrogate(0)
{
}

Keyboard::~Keyboard()
{
}

char *Keyboard::getkeyboardbuffer(unsigned int &bufCnt)
{
	bufCnt = 0;
	return nullptr;
}

unsigned int Keyboard::getKeyboardState()
{
	return 0;
}

KeyboardHistory::HistoryElement *Keyboard::getkeyboardhistory(unsigned int &histCnt)
{
	histCnt = 0;
	return keybHist_;
}
