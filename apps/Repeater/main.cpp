// Open3DStream Repeater: relays what senders push to listen-addr to every receiver subscribed at
// broadcast-addr (relay.h). Usage and options: run it without arguments.
#include "relay.h"

#include "o3ds/o3ds_version.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace
{
	std::atomic<bool> gStopRequested{false};

	void OnSignal(int)
	{
		gStopRequested.store(true); // a lock-free atomic store is signal-safe
	}

	void PrintUsage(const char* program)
	{
		std::printf("O3DS Repeater - %s\n", O3DS::getVersion());
		std::printf("Usage: %s listen-addr broadcast-addr [options]\n", program);
		std::printf("  listen-addr      NNG URL senders push to (pull), e.g. tcp://0.0.0.0:7000\n");
		std::printf("  broadcast-addr   NNG URL receivers subscribe to (pub), e.g. tcp://0.0.0.0:7001\n");
		std::printf("Options:\n");
		std::printf("  --max-message-mb N   largest message a sender may push (default 64; 0 = unlimited)\n");
		std::printf("  --send-buffer N      messages queued per subscriber (default 256)\n");
		std::printf("  --stats-seconds N    seconds between stats lines (default 10; 0 = none)\n");
	}

	bool ParseInt(const char* text, long& out)
	{
		char* end = nullptr;
		out = std::strtol(text, &end, 10);
		return end != text && *end == '\0' && out >= 0;
	}
}

int main(int argc, char* argv[])
{
	if (argc < 3)
	{
		PrintUsage(argv[0]);
		return 1;
	}

	O3DS::Repeater::RelayOptions options;
	options.listenUrl = argv[1];
	options.publishUrl = argv[2];
	long statsSeconds = 10;
	for (int i = 3; i < argc; ++i)
	{
		long value = 0;
		const bool hasValue = i + 1 < argc && ParseInt(argv[i + 1], value);
		if (std::strcmp(argv[i], "--max-message-mb") == 0 && hasValue)
			options.maxMessageBytes = static_cast<size_t>(value) * 1024u * 1024u;
		else if (std::strcmp(argv[i], "--send-buffer") == 0 && hasValue && value > 0)
			options.sendBufferMessages = static_cast<int>(value);
		else if (std::strcmp(argv[i], "--stats-seconds") == 0 && hasValue)
			statsSeconds = value;
		else
		{
			std::printf("Unknown or invalid option: %s\n", argv[i]);
			PrintUsage(argv[0]);
			return 1;
		}
		++i;
	}

	std::signal(SIGINT, OnSignal);
	std::signal(SIGTERM, OnSignal);

	O3DS::Repeater::Relay relay;
	std::string error;
	if (!relay.Start(options, error))
	{
		std::printf("Could not start: %s\n", error.c_str());
		return 2;
	}
	std::printf("O3DS Repeater %s: senders push to %s, receivers subscribe to %s\n",
		O3DS::getVersion(), options.listenUrl.c_str(), options.publishUrl.c_str());
	std::fflush(stdout);

	// The relay runs on its own thread; this one watches for a signal and prints stats.
	std::thread relayThread([&relay]() { relay.Run(); });
	auto nextStats = std::chrono::steady_clock::now() + std::chrono::seconds(statsSeconds);
	while (!gStopRequested.load())
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		if (statsSeconds > 0 && std::chrono::steady_clock::now() >= nextStats)
		{
			std::printf("%s\n", relay.StatsLine().c_str());
			std::fflush(stdout);
			nextStats += std::chrono::seconds(statsSeconds);
		}
	}

	std::printf("Stopping\n");
	relay.Stop();
	relayThread.join();
	std::printf("%s\n", relay.StatsLine().c_str());
	return 0;
}
