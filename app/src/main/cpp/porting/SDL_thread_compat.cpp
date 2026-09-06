// See SDL_thread_compat.h for what this is and why.

#include <SDL_thread_compat.h>

#include <pthread.h>
#include <time.h>

struct SDL_Thread {
	pthread_t handle;
	int (*fn)(void *);
	void *data;
	int   result;
};

static void *threadTrampoline(void *arg)
{
	SDL_Thread *thread = (SDL_Thread *) arg;
	thread->result     = thread->fn(thread->data);
	return nullptr;
}

SDL_Thread *SDL_CreateThread(int (*fn)(void *), void *data)
{
	SDL_Thread *thread = new SDL_Thread();
	thread->fn         = fn;
	thread->data       = data;
	thread->result     = 0;

	if (pthread_create(&thread->handle, nullptr, threadTrampoline, thread) != 0)
	{
		delete thread;
		return nullptr;
	}
	return thread;
}

void SDL_WaitThread(SDL_Thread *thread, int *status)
{
	if (!thread) return;
	pthread_join(thread->handle, nullptr);
	if (status) *status = thread->result;
	delete thread;
}

void SDL_Delay(Uint32 ms)
{
	struct timespec ts;
	ts.tv_sec  = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000;
	nanosleep(&ts, nullptr);
}

Uint32 SDL_GetTicks()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (Uint32) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

struct SDL_mutex {
	pthread_mutex_t handle;
};

SDL_mutex *SDL_CreateMutex()
{
	SDL_mutex *mutex = new SDL_mutex();
	pthread_mutex_init(&mutex->handle, nullptr);
	return mutex;
}

void SDL_DestroyMutex(SDL_mutex *mutex)
{
	if (!mutex) return;
	pthread_mutex_destroy(&mutex->handle);
	delete mutex;
}

int SDL_LockMutex(SDL_mutex *mutex)
{
	return pthread_mutex_lock(&mutex->handle);
}

int SDL_UnlockMutex(SDL_mutex *mutex)
{
	return pthread_mutex_unlock(&mutex->handle);
}
