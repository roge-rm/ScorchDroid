#include <ClientSync.hpp>
#include <SDL_thread_compat.h>

ClientSync::ClientSync() : waitingEventTime_(0)
{
}

ClientSync::~ClientSync()
{
}

bool ClientSync::continueToSimulate()
{
	return currentTime_ < waitingEventTime_;
}

void ClientSync::addComsSimulateMessage(ComsSimulateMessage &message)
{
	waitingEventTime_ = message.getEventTime();

	std::list<SimAction *>::iterator itor;
	for (itor = message.getActions().begin();
		itor != message.getActions().end();
		++itor)
	{
		simActions_.push_back(new SimActionContainer(*itor, waitingEventTime_));
	}
}

void ClientSync::newLevel()
{
	while (!simActions_.empty())
	{
		SimActionContainer *container = simActions_.front();
		delete container;
		simActions_.pop_front();
	}

	Simulator::newLevel();
	lastTickTime_ = SDL_GetTicks() - 1;
}

void ClientSync::setSimulationTime(fixed actualTime)
{
	lastTickTime_ = SDL_GetTicks();
	actualTime_ = actualTime;
	simulateTime();
}
