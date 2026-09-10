#include "ChatStore.h"
#include "SoundEventQueue.h"

#include <common/Defines.hpp>
#include <deque>
#include <mutex>

namespace ScorchDroidChat
{
	namespace
	{
		std::mutex storeMutex;
		std::deque<Line> lines;
		unsigned int nextId = 1;
		unsigned int storeVersion = 0;

		// Deliberately more than the transient HUD stack ever shows at
		// once: the stack is a few seconds of the most recent traffic, but
		// the log dialog scrolls back, and upstream's own server-side
		// rolling buffer is 25. A hundred short strings is nothing.
		const size_t kMaxLines = 100;

		// Upstream's own notification sound, and upstream's own choice of
		// when: GLWChannelView::channelText plays its window's <textsound>
		// for every line on a channel that window shows. Both of the chat
		// windows in windows.xml name the same file and between them cover
		// every channel - combat in one, announce/info/general/team/whisper
		// in the other - so in practice every line makes this sound.
		//
		// Raised here rather than at either polling site because this is
		// where the two roles meet: a hosted game finds its lines in
		// ServerChannelManager's log, a joined one gets them as
		// ComsChannelTextMessage, and both end up calling push().
		const char *kChatSoundFile = "data/wav/misc/text.wav";
	}

	void push(const Line &line)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		Line stored = line;
		stored.id = nextId++;
		lines.push_back(stored);
		while (lines.size() > kMaxLines) lines.pop_front();
		storeVersion++;

		// eText, as upstream raises it - the same band as the turn
		// countdown, and well below eAction, so a round's explosions take
		// the channels ahead of a chat notification rather than the other
		// way round.
		ScorchDroidAudio::pushSoundEvent(
			S3D::getModFile(kChatSoundFile), ScorchDroidAudio::kPriorityText);
	}

	std::vector<Line> snapshot()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		return std::vector<Line>(lines.begin(), lines.end());
	}

	std::vector<Line> since(unsigned int afterId)
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		std::vector<Line> out;
		for (std::deque<Line>::iterator itor = lines.begin(); itor != lines.end(); ++itor) {
			if (itor->id > afterId) out.push_back(*itor);
		}
		return out;
	}

	unsigned int version()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		return storeVersion;
	}

	void clear()
	{
		std::lock_guard<std::mutex> lock(storeMutex);
		lines.clear();
		// Ids keep counting rather than restarting: a HUD that has just
		// shown line 7 must not be handed a different line 7 next round.
		storeVersion++;
	}
}
