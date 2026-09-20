#include "LogRing.hpp"

#include <cstdio>

LogRing::LogRing(std::size_t capacity) : capacity_(capacity ? capacity : 1) {}

void LogRing::logMessage(LoggerInfo &info)
{
	const char *time    = info.getTime();
	const char *message = info.getMessage();

	bool echo = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		echo = echo_;
		lines_.push_back(Line{nextSeq_++, time ? time : "", message ? message : ""});
		while (lines_.size() > capacity_) lines_.pop_front();
	}

	if (echo)
	{
		std::printf("[%s] %s\n", time ? time : "", message ? message : "");
		std::fflush(stdout);
	}
}

std::vector<LogRing::Line> LogRing::since(unsigned long long afterSeq, std::size_t limit) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<Line> result;
	for (const Line &line : lines_)
	{
		if (line.seq <= afterSeq) continue;
		result.push_back(line);
	}
	// Keep the newest when there are more than the caller wants - a tail is
	// about what just happened, not where the backlog starts.
	if (limit && result.size() > limit) result.erase(result.begin(), result.end() - (long) limit);
	return result;
}

unsigned long long LogRing::lastSeq() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return nextSeq_ - 1;
}

void LogRing::setEcho(bool echo)
{
	std::lock_guard<std::mutex> lock(mutex_);
	echo_ = echo;
}
