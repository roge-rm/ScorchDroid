#ifndef SCORCHDROID_SDL_THREAD_COMPAT_H
#define SCORCHDROID_SDL_THREAD_COMPAT_H

// Android build: pthread-backed replacement for the small subset of SDL
// 1.2's SDL_thread/SDL_timer API that src/common/net uses (SDL_CreateThread,
// SDL_WaitThread, SDL_Delay, SDL_GetTicks) - SDL is not part of this build,
// see the porting plan.

#include <SDL_types_compat.h>

struct SDL_Thread;

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data);
void        SDL_WaitThread(SDL_Thread *thread, int *status);

void   SDL_Delay(Uint32 ms);
Uint32 SDL_GetTicks();

struct SDL_mutex;

SDL_mutex *SDL_CreateMutex();
void       SDL_DestroyMutex(SDL_mutex *mutex);
int        SDL_LockMutex(SDL_mutex *mutex);
int        SDL_UnlockMutex(SDL_mutex *mutex);

#endif  // SCORCHDROID_SDL_THREAD_COMPAT_H
