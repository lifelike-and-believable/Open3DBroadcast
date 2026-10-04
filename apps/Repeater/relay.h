// The Open3DStream Repeater's relay: messages pushed to one NNG pull socket are published, byte
// for byte and in order per sender, on one NNG pub socket. The UE NNG sender pushes (mode push,
// role client) and receivers subscribe (mode sub, role client); see docs/wire-format.md for what
// the messages are. The relay looks at a message only to count it (mocap frame, envelope kind,
// or other) and never drops one it does not understand.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <nng/nng.h>

namespace O3DS
{
namespace Repeater
{
	struct RelayOptions
	{
		std::string listenUrl;  //!< pull socket, where senders push (e.g. tcp://0.0.0.0:7000)
		std::string publishUrl; //!< pub socket, where receivers subscribe (e.g. tcp://0.0.0.0:7001)
		//! Largest message a sender may push. A larger one closes that sender's connection (NNG
		//! TCP transport, NNG_OPT_RECVMAXSZ); the sender reconnects. 0 is unlimited.
		size_t maxMessageBytes = 64u * 1024u * 1024u;
		//! Messages queued per subscriber before pub drops for that subscriber (NNG_OPT_SENDBUF).
		int sendBufferMessages = 256;
		//! How long one receive waits before the loop checks for Stop and stats.
		int receiveTimeoutMs = 100;
	};

	//! Counters since Start. Any thread may read them while the relay runs.
	struct RelayStats
	{
		std::atomic<uint64_t> messagesRelayed{0};
		std::atomic<uint64_t> bytesRelayed{0};
		std::atomic<uint64_t> mocapFrames{0};     //!< verified mocap frames (O3DS::PeekPacketMeta)
		std::atomic<uint64_t> audioEnvelopes{0};
		std::atomic<uint64_t> controlEnvelopes{0};
		std::atomic<uint64_t> otherMessages{0};   //!< anything else, still relayed
		std::atomic<uint64_t> receiveErrors{0};   //!< errors other than timeout and close
		std::atomic<uint64_t> sendErrors{0};
		std::atomic<int> senders{0};              //!< connected pushers
		std::atomic<int> subscribers{0};          //!< connected subscribers
	};

	class Relay
	{
	public:
		Relay();
		~Relay();
		Relay(const Relay&) = delete;
		Relay& operator=(const Relay&) = delete;

		//! Opens and listens on both sockets. False, with error set, when either fails.
		bool Start(const RelayOptions& options, std::string& error);

		//! Relays until Stop() or until the sockets close. Backs off (10 ms, doubling to 1 s) on
		//! receive errors other than a timeout, so a failing socket never spins. Call from one thread.
		void Run();

		//! Ends Run and closes the sockets. Any thread; idempotent.
		void Stop();

		const RelayStats& Stats() const { return mStats; }

		//! The bound TCP ports (useful with port 0), or 0 when not a TCP listener.
		int ListenPort() const;
		int PublishPort() const;

		//! One line: totals since Start and the current connection counts.
		std::string StatsLine() const;

	private:
		void Count(nng_msg* msg);
		static void OnPipeEvent(nng_pipe pipe, nng_pipe_ev event, void* arg);

		nng_socket mPull = NNG_SOCKET_INITIALIZER;
		nng_socket mPub = NNG_SOCKET_INITIALIZER;
		nng_listener mPullListener = NNG_LISTENER_INITIALIZER;
		nng_listener mPubListener = NNG_LISTENER_INITIALIZER;
		std::atomic<bool> mOpen{false};
		std::atomic<bool> mStop{false};
		RelayStats mStats;
	};
}
}
