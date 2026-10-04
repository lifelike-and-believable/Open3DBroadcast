// The Repeater's relay (apps/Repeater/relay.h) with real NNG sockets on 127.0.0.1 and ephemeral
// ports: what senders push arrives at every subscriber byte for byte and in order per sender;
// messages are counted by kind and the ones the relay does not understand are still relayed; a
// message above the size limit closes only that sender's connection; Stop ends Run while
// traffic flows. Waits poll a condition against a deadline, never a fixed sleep.
#include "test_framework.h"

#include "relay.h"

#include "o3ds/model.h"
#include "o3ds/stream_writer.h"
#include "o3ds/wire_format.h"

#include <nng/nng.h>
#include <nng/protocol/pipeline0/push.h>
#include <nng/protocol/pubsub0/sub.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace O3DS;

namespace
{
	bool WaitFor(const std::function<bool()>& condition, int timeoutMs = 5000)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		while (std::chrono::steady_clock::now() < deadline)
		{
			if (condition())
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		return condition();
	}

	std::string Url(int port)
	{
		return "tcp://127.0.0.1:" + std::to_string(port);
	}

	//! A relay on ephemeral ports, running on its own thread.
	struct RunningRelay
	{
		Repeater::Relay relay;
		std::thread thread;
		bool started = false;

		explicit RunningRelay(size_t maxMessageBytes = 64u * 1024u * 1024u)
		{
			Repeater::RelayOptions options;
			options.listenUrl = "tcp://127.0.0.1:0";
			options.publishUrl = "tcp://127.0.0.1:0";
			options.maxMessageBytes = maxMessageBytes;
			options.receiveTimeoutMs = 20;
			std::string error;
			started = relay.Start(options, error);
			if (started)
				thread = std::thread([this]() { relay.Run(); });
		}
		~RunningRelay()
		{
			relay.Stop();
			if (thread.joinable())
				thread.join();
		}
	};

	struct Socket
	{
		nng_socket socket = NNG_SOCKET_INITIALIZER;
		bool open = false;
		~Socket()
		{
			if (open)
				nng_close(socket);
		}
	};

	bool OpenPusher(Socket& s, int port)
	{
		if (nng_push0_open(&s.socket) != 0)
			return false;
		s.open = true;
		return nng_dial(s.socket, Url(port).c_str(), nullptr, 0) == 0;
	}

	bool OpenSubscriber(Socket& s, int port)
	{
		if (nng_sub0_open(&s.socket) != 0)
			return false;
		s.open = true;
		nng_socket_set_ms(s.socket, NNG_OPT_RECVTIMEO, 2000);
		return nng_socket_set(s.socket, NNG_OPT_SUB_SUBSCRIBE, "", 0) == 0
			&& nng_dial(s.socket, Url(port).c_str(), nullptr, 0) == 0;
	}

	bool Push(Socket& s, const std::vector<char>& bytes)
	{
		return nng_send(s.socket, const_cast<char*>(bytes.data()), bytes.size(), 0) == 0;
	}

	bool Receive(Socket& s, std::vector<char>& out)
	{
		void* buffer = nullptr;
		size_t size = 0;
		if (nng_recv(s.socket, &buffer, &size, NNG_FLAG_ALLOC) != 0)
			return false;
		out.assign(static_cast<char*>(buffer), static_cast<char*>(buffer) + size);
		nng_free(buffer, size);
		return true;
	}

	//! "<sender>:<index>" padded to size bytes: not a frame and not an envelope.
	std::vector<char> Tagged(int sender, int index, size_t size = 64)
	{
		std::string text = std::to_string(sender) + ":" + std::to_string(index);
		std::vector<char> bytes(text.begin(), text.end());
		bytes.resize(size, '.');
		return bytes;
	}

	std::vector<char> MocapFrame()
	{
		SubjectList list;
		Subject* subject = list.addSubject("Actor");
		Transform* root = subject->addTransform("Root", -1);
		root->transformOrder.push_back(O3DS::TTranslation);
		root->transformOrder.push_back(O3DS::TRotation);
		StreamWriter writer;
		std::vector<char> frame;
		writer.WriteFull(*subject, frame, 1.0);
		return frame;
	}

	std::vector<char> Envelope(Wire::EnvelopeKind kind, Wire::EnvelopeCodec codec, size_t payloadSize)
	{
		std::vector<char> bytes(Wire::kEnvelopeV2HeaderSize + payloadSize, 0);
		Wire::WriteEnvelopeHeaderV2(bytes.data(), kind, codec, 0, static_cast<uint32_t>(payloadSize), 0);
		return bytes;
	}
}

O3DS_TEST(Repeater_RelaysEveryMessageInOrderPerSender)
{
	RunningRelay r;
	O3DS_CHECK(r.started);
	O3DS_CHECK(r.relay.ListenPort() > 0 && r.relay.PublishPort() > 0);

	Socket subscriber;
	O3DS_CHECK(OpenSubscriber(subscriber, r.relay.PublishPort()));
	Socket a;
	Socket b;
	O3DS_CHECK(OpenPusher(a, r.relay.ListenPort()));
	O3DS_CHECK(OpenPusher(b, r.relay.ListenPort()));
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().senders.load() == 2 && r.relay.Stats().subscribers.load() == 1; }));
	// pub/sub: a subscription takes effect a moment after the pipe is up.
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	// The subscriber reads while the senders push: pub drops for a subscriber whose queue is full
	// (RelayOptions::sendBufferMessages), which is pub's behaviour, not what this test checks.
	constexpr int kPerSender = 200;
	std::vector<std::vector<char>> received;
	std::thread reader([&]()
	{
		std::vector<char> message;
		while (received.size() < static_cast<size_t>(2 * kPerSender) && Receive(subscriber, message))
			received.push_back(message);
	});
	for (int i = 0; i < kPerSender; ++i)
	{
		O3DS_CHECK(Push(a, Tagged(1, i)));
		O3DS_CHECK(Push(b, Tagged(2, i, 3000)));
	}
	reader.join();

	std::map<int, int> next; // sender -> next expected index
	for (const std::vector<char>& message : received)
	{
		const std::string text(message.begin(), message.end());
		const int sender = std::stoi(text.substr(0, text.find(':')));
		const int index = std::stoi(text.substr(text.find(':') + 1));
		O3DS_CHECK(index == next[sender]); // in order per sender
		next[sender] = index + 1;
		O3DS_CHECK(message == Tagged(sender, index, sender == 1 ? 64 : 3000)); // byte for byte
	}
	O3DS_CHECK(received.size() == static_cast<size_t>(2 * kPerSender));
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().messagesRelayed.load() == static_cast<uint64_t>(2 * kPerSender); }));
	O3DS_CHECK(r.relay.Stats().otherMessages.load() == static_cast<uint64_t>(2 * kPerSender));
	O3DS_CHECK(r.relay.Stats().receiveErrors.load() == 0);
}

O3DS_TEST(Repeater_CountsKindsAndRelaysWhatItDoesNotUnderstand)
{
	RunningRelay r;
	O3DS_CHECK(r.started);
	Socket subscriber;
	O3DS_CHECK(OpenSubscriber(subscriber, r.relay.PublishPort()));
	Socket sender;
	O3DS_CHECK(OpenPusher(sender, r.relay.ListenPort()));
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().senders.load() == 1 && r.relay.Stats().subscribers.load() == 1; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	const std::vector<char> frame = MocapFrame();
	std::vector<char> corrupt = frame;
	corrupt.back() ^= 0x5A; // fails the CRC: not counted as mocap, still relayed
	const std::vector<std::vector<char>> messages = {
		frame,
		Envelope(Wire::EnvelopeKind::Audio, Wire::EnvelopeCodec::PCM16, 32),
		Envelope(Wire::EnvelopeKind::Control, Wire::EnvelopeCodec::O3DControl, 16),
		corrupt,
		Tagged(9, 0),
	};
	for (const std::vector<char>& message : messages)
		O3DS_CHECK(Push(sender, message));

	for (const std::vector<char>& expected : messages)
	{
		std::vector<char> message;
		O3DS_CHECK(Receive(subscriber, message));
		O3DS_CHECK(message == expected);
	}
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().messagesRelayed.load() == messages.size(); }));
	O3DS_CHECK(r.relay.Stats().mocapFrames.load() == 1);
	O3DS_CHECK(r.relay.Stats().audioEnvelopes.load() == 1);
	O3DS_CHECK(r.relay.Stats().controlEnvelopes.load() == 1);
	O3DS_CHECK(r.relay.Stats().otherMessages.load() == 2);
	O3DS_CHECK(r.relay.StatsLine().find("relayed 5 messages") != std::string::npos);
}

O3DS_TEST(Repeater_OversizeMessageClosesOnlyThatSender)
{
	constexpr size_t kLimit = 4096;
	RunningRelay r(kLimit);
	O3DS_CHECK(r.started);
	Socket subscriber;
	O3DS_CHECK(OpenSubscriber(subscriber, r.relay.PublishPort()));
	Socket big;
	Socket small;
	O3DS_CHECK(OpenPusher(big, r.relay.ListenPort()));
	O3DS_CHECK(OpenPusher(small, r.relay.ListenPort()));
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().senders.load() == 2 && r.relay.Stats().subscribers.load() == 1; }));
	std::this_thread::sleep_for(std::chrono::milliseconds(50));

	O3DS_CHECK(Push(big, Tagged(1, 0, kLimit * 2)));
	// The relay closes the oversize sender's connection; the push socket reconnects on its own.
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().senders.load() < 2; }));

	O3DS_CHECK(Push(small, Tagged(2, 0)));
	std::vector<char> message;
	O3DS_CHECK(Receive(subscriber, message));
	O3DS_CHECK(message == Tagged(2, 0)); // the oversize message never arrives; the other sender's does
	O3DS_CHECK(r.relay.Stats().messagesRelayed.load() == 1);
}

O3DS_TEST(Repeater_StopWhileTrafficFlows)
{
	RunningRelay r;
	O3DS_CHECK(r.started);
	Socket subscriber;
	O3DS_CHECK(OpenSubscriber(subscriber, r.relay.PublishPort()));
	Socket sender;
	O3DS_CHECK(OpenPusher(sender, r.relay.ListenPort()));
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().senders.load() == 1; }));

	std::atomic<bool> pushing{true};
	std::thread pusher([&]()
	{
		int i = 0;
		while (pushing.load())
			nng_send(sender.socket, Tagged(1, i++).data(), 64, NNG_FLAG_NONBLOCK);
	});
	O3DS_CHECK(WaitFor([&]() { return r.relay.Stats().messagesRelayed.load() > 100; }));

	const auto before = std::chrono::steady_clock::now();
	r.relay.Stop();
	r.thread.join();
	const auto stopMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - before).count();
	pushing.store(false);
	pusher.join();
	O3DS_CHECK(stopMs < 2000); // Run returns promptly, mid-traffic
	r.relay.Stop(); // idempotent
}

O3DS_TEST(Repeater_StartFailsCleanlyOnABadUrl)
{
	Repeater::Relay relay;
	Repeater::RelayOptions options;
	options.listenUrl = "not-a-url";
	options.publishUrl = "tcp://127.0.0.1:0";
	std::string error;
	O3DS_CHECK(!relay.Start(options, error));
	O3DS_CHECK(!error.empty());
	relay.Stop();
}
