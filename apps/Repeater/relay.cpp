#include "relay.h"

#include "o3ds/receiver_streams.h"
#include "o3ds/wire_format.h"

#include <nng/protocol/pipeline0/pull.h>
#include <nng/protocol/pubsub0/pub.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace O3DS
{
namespace Repeater
{
	namespace
	{
		constexpr int kMinBackoffMs = 10;
		constexpr int kMaxBackoffMs = 1000;

		int BoundPort(nng_listener listener)
		{
			int port = 0;
			if (nng_listener_id(listener) <= 0 || nng_listener_get_int(listener, NNG_OPT_TCP_BOUND_PORT, &port) != 0)
				return 0;
			return port;
		}
	}

	Relay::Relay() = default;

	Relay::~Relay()
	{
		Stop();
	}

	bool Relay::Start(const RelayOptions& options, std::string& error)
	{
		Stop();
		mStop.store(false);
		mPull = NNG_SOCKET_INITIALIZER;
		mPub = NNG_SOCKET_INITIALIZER;
		mPullListener = NNG_LISTENER_INITIALIZER;
		mPubListener = NNG_LISTENER_INITIALIZER;

		auto fail = [&](const char* what, int rv)
		{
			error = std::string(what) + ": " + nng_strerror(rv);
			Stop();
			return false;
		};

		int rv = nng_pull0_open(&mPull);
		if (rv != 0)
			return fail("open pull socket", rv);
		if ((rv = nng_pub0_open(&mPub)) != 0)
		{
			nng_close(mPull);
			return fail("open pub socket", rv);
		}
		mOpen.store(true);

		// Pipe counts for the stats; the callbacks run on NNG threads and only touch atomics.
		nng_pipe_notify(mPull, NNG_PIPE_EV_ADD_POST, &Relay::OnPipeEvent, &mStats.senders);
		nng_pipe_notify(mPull, NNG_PIPE_EV_REM_POST, &Relay::OnPipeEvent, &mStats.senders);
		nng_pipe_notify(mPub, NNG_PIPE_EV_ADD_POST, &Relay::OnPipeEvent, &mStats.subscribers);
		nng_pipe_notify(mPub, NNG_PIPE_EV_REM_POST, &Relay::OnPipeEvent, &mStats.subscribers);

		if ((rv = nng_socket_set_size(mPull, NNG_OPT_RECVMAXSZ, options.maxMessageBytes)) != 0)
			return fail("set receive size limit", rv);
		if ((rv = nng_socket_set_ms(mPull, NNG_OPT_RECVTIMEO, options.receiveTimeoutMs)) != 0)
			return fail("set receive timeout", rv);
		if ((rv = nng_socket_set_int(mPub, NNG_OPT_SENDBUF, std::max(1, options.sendBufferMessages))) != 0)
			return fail("set send buffer", rv);

		if ((rv = nng_listen(mPull, options.listenUrl.c_str(), &mPullListener, 0)) != 0)
			return fail(("listen on " + options.listenUrl).c_str(), rv);
		if ((rv = nng_listen(mPub, options.publishUrl.c_str(), &mPubListener, 0)) != 0)
			return fail(("publish on " + options.publishUrl).c_str(), rv);
		return true;
	}

	void Relay::Run()
	{
		int backoffMs = kMinBackoffMs;
		while (!mStop.load())
		{
			nng_msg* msg = nullptr;
			const int rv = nng_recvmsg(mPull, &msg, 0);
			if (rv == NNG_ETIMEDOUT)
				continue; // the loop checks Stop
			if (rv == NNG_ECLOSED)
				break;
			if (rv != 0)
			{
				mStats.receiveErrors.fetch_add(1);
				std::this_thread::sleep_for(std::chrono::milliseconds(backoffMs));
				backoffMs = std::min(backoffMs * 2, kMaxBackoffMs);
				continue;
			}
			backoffMs = kMinBackoffMs;

			const size_t len = nng_msg_len(msg);
			Count(msg);
			// pub never blocks: a subscriber whose queue is full misses this message.
			if (nng_sendmsg(mPub, msg, 0) != 0)
			{
				nng_msg_free(msg);
				mStats.sendErrors.fetch_add(1);
				continue;
			}
			mStats.messagesRelayed.fetch_add(1);
			mStats.bytesRelayed.fetch_add(len);
		}
	}

	void Relay::Stop()
	{
		mStop.store(true);
		// The handles are not reset: Run may still be reading them on another thread. Closing
		// ends a blocked receive there with NNG_ECLOSED.
		if (mOpen.exchange(false))
		{
			nng_close(mPull);
			nng_close(mPub);
		}
	}

	int Relay::ListenPort() const
	{
		return BoundPort(mPullListener);
	}

	int Relay::PublishPort() const
	{
		return BoundPort(mPubListener);
	}

	std::string Relay::StatsLine() const
	{
		char line[320];
		std::snprintf(line, sizeof(line),
			"relayed %llu messages (%llu bytes): %llu mocap, %llu audio, %llu control, %llu other; "
			"errors: %llu receive, %llu send; senders %d, subscribers %d",
			static_cast<unsigned long long>(mStats.messagesRelayed.load()),
			static_cast<unsigned long long>(mStats.bytesRelayed.load()),
			static_cast<unsigned long long>(mStats.mocapFrames.load()),
			static_cast<unsigned long long>(mStats.audioEnvelopes.load()),
			static_cast<unsigned long long>(mStats.controlEnvelopes.load()),
			static_cast<unsigned long long>(mStats.otherMessages.load()),
			static_cast<unsigned long long>(mStats.receiveErrors.load()),
			static_cast<unsigned long long>(mStats.sendErrors.load()),
			mStats.senders.load(), mStats.subscribers.load());
		return line;
	}

	void Relay::Count(nng_msg* msg)
	{
		const char* data = static_cast<const char*>(nng_msg_body(msg));
		const size_t len = nng_msg_len(msg);
		Wire::EnvelopeHeader header;
		if (Wire::ReadEnvelopeHeader(data, len, header))
		{
			if (header.kind == static_cast<uint8_t>(Wire::EnvelopeKind::Audio))
				mStats.audioEnvelopes.fetch_add(1);
			else if (header.kind == static_cast<uint8_t>(Wire::EnvelopeKind::Control))
				mStats.controlEnvelopes.fetch_add(1);
			else
				mStats.otherMessages.fetch_add(1);
			return;
		}
		PacketMeta meta;
		if (PeekPacketMeta(data, len, meta))
			mStats.mocapFrames.fetch_add(1);
		else
			mStats.otherMessages.fetch_add(1);
	}

	void Relay::OnPipeEvent(nng_pipe, nng_pipe_ev event, void* arg)
	{
		std::atomic<int>* count = static_cast<std::atomic<int>*>(arg);
		if (event == NNG_PIPE_EV_ADD_POST)
			count->fetch_add(1);
		else if (event == NNG_PIPE_EV_REM_POST)
			count->fetch_sub(1);
	}
}
}
