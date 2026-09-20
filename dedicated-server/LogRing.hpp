#ifndef SCORCHDROID_LOG_RING_HPP
#define SCORCHDROID_LOG_RING_HPP

#include <common/LoggerI.hpp>

#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

// The dedicated server's logger: prints to stdout exactly as the original
// StdoutLogger did, and additionally keeps the last few thousand lines so
// the control channel can hand them to a web browser.
//
// Every line gets a monotonically increasing sequence number, which is what
// makes an incremental tail possible: a reader asks for everything after the
// sequence it last saw, rather than re-reading the whole buffer or trying to
// match on timestamps (several lines routinely share a second).
//
// Upstream's Logger is documented as needing to be thread safe, and
// Logger::processLogEntries() is what actually calls logMessage() - on the
// engine thread, in this process. The mutex is still here because the
// control channel reads the ring, and nothing promises that stays on the
// same thread forever.
class LogRing : public LoggerI
{
public:
	struct Line
	{
		unsigned long long seq;
		std::string        time;
		std::string        message;
	};

	explicit LogRing(std::size_t capacity = 2000);

	void logMessage(LoggerInfo &info) override;

	// Every line with a sequence strictly greater than [afterSeq], oldest
	// first, capped at [limit] (0 for no cap). A reader that has fallen
	// further behind than the ring is deep simply misses the difference,
	// which is what a tail is.
	std::vector<Line> since(unsigned long long afterSeq, std::size_t limit = 0) const;

	unsigned long long lastSeq() const;

	// Whether new lines should also go to stdout. On by default: a
	// container's logs are how most people will look at this.
	void setEcho(bool echo);

private:
	mutable std::mutex mutex_;
	std::deque<Line>   lines_;
	std::size_t        capacity_;
	unsigned long long nextSeq_ = 1;
	bool               echo_    = true;
};

#endif  // SCORCHDROID_LOG_RING_HPP
