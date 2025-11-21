// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#include "HAL/CriticalSection.h"

#include "Containers/Map.h"
#include "Containers/Queue.h"
#include "MoQ/MoQProtocol.h"
#include "Shared/QuicHelpers.h"

#include "msquic.h"

class FO3DQuicSender;

namespace O3DMoQ
{
	enum class EMoQReliabilityMode : uint8;
}

namespace O3DQuic
{
	class IQuicConnectionListener
	{
	public:
		virtual ~IQuicConnectionListener() = default;
		virtual void OnQuicSubscriberConnected(uint32 SubscriberId) = 0;
		virtual void OnQuicSubscriberDisconnected(uint32 SubscriberId, const FString& Reason) = 0;
		virtual void OnQuicSubscriberControlMessage(uint32 SubscriberId, const TArray<uint8>& Payload) = 0;
	};

	enum class EQuicFrameType : uint8
	{
		Control = 1,
		Object = 2
	};

	/** Lightweight RAII helper around MsQuic handles for publisher (server) role. */
	class OPEN3DTRANSPORTQUIC_API FQuicConnection
	{
	public:
		FQuicConnection();
		~FQuicConnection();

		/** Initialize MsQuic registration/configuration and start a listener for inbound subscribers. */
		bool InitializeServer(const FQuicSenderOptions& Options, IQuicConnectionListener* InListener, FString& OutError);

		/** Release all MsQuic handles. Safe to call multiple times. */
		void Shutdown();

		bool IsInitialized() const;

		bool SendControlMessage(uint32 SubscriberId, const uint8* Data, uint32 Length, FString& OutError);
		bool SendObjectMessage(uint32 SubscriberId, const uint8* Data, uint32 Length, O3DMoQ::EMoQReliabilityMode Reliability, FString& OutError);
		int32 GetActiveSubscriberCount() const;

	private:
		struct FSubscriberContext : public TSharedFromThis<FSubscriberContext>
		{
			uint32 SubscriberId = 0;
			HQUIC ConnectionHandle = nullptr;
			bool bConnected = false;
		};

		struct FStreamContext
		{
			FQuicConnection* Owner = nullptr;
			TWeakPtr<FSubscriberContext> Subscriber;
			EQuicFrameType FrameType = EQuicFrameType::Control;
			bool bHeaderConsumed = false;
			bool bOverflowed = false;
			HQUIC StreamHandle = nullptr;
			TArray<uint8> ReceiveBuffer;
			TArray<uint8> SendBuffer;
		};

		struct FDatagramSendState
		{
			TArray<uint8> Buffer;
		};

	private:
		bool OpenRegistration(FString& OutError);
		bool OpenConfiguration(const FQuicSenderOptions& Options, FString& OutError);
		bool StartListener(const FQuicSenderOptions& Options, FString& OutError);

		void CloseListener();
		void CloseConfiguration();
		void CloseRegistration();

		TSharedPtr<FSubscriberContext> RegisterSubscriber(HQUIC ConnectionHandle);
		TSharedPtr<FSubscriberContext> GetSubscriberByConnection(HQUIC ConnectionHandle) const;
		TSharedPtr<FSubscriberContext> GetSubscriberById(uint32 SubscriberId) const;
		void RemoveSubscriber(HQUIC ConnectionHandle, const FString& Reason);
		bool SendFrameInternal(const TSharedPtr<FSubscriberContext>& Subscriber, EQuicFrameType FrameType, const uint8* Data, uint32 Length, FString& OutError);
		bool SendDatagramInternal(const TSharedPtr<FSubscriberContext>& Subscriber, const uint8* Data, uint32 Length, FString& OutError);
		void HandlePeerStreamStarted(const TSharedPtr<FSubscriberContext>& Subscriber, HQUIC StreamHandle);
		void HandleStreamReceive(FStreamContext* StreamContext, const QUIC_STREAM_EVENT* Event);
		void DispatchControlFrame(const TSharedPtr<FSubscriberContext>& Subscriber, TArray<uint8>&& Payload);

		static QUIC_STATUS QUIC_API ListenerCallback(HQUIC Listener, void* Context, QUIC_LISTENER_EVENT* Event);
		static QUIC_STATUS QUIC_API ConnectionCallback(HQUIC Connection, void* Context, QUIC_CONNECTION_EVENT* Event);
		static QUIC_STATUS QUIC_API StreamCallback(HQUIC Stream, void* Context, QUIC_STREAM_EVENT* Event);
		static void DestroyStreamContext(const QUIC_API_TABLE* Api, HQUIC Stream, FStreamContext* Context);

	private:
		const QUIC_API_TABLE* Api = nullptr;
		HQUIC RegistrationHandle = nullptr;
		HQUIC ConfigurationHandle = nullptr;
		HQUIC ListenerHandle = nullptr;

		FQuicSenderOptions ActiveOptions;
		bool bInitialized = false;
		uint32 NextSubscriberId = 1;
		IQuicConnectionListener* Listener = nullptr;
		bool bWarnedUnreliableFallback = false;

		TMap<HQUIC, TSharedPtr<FSubscriberContext>> SubscribersByConnection;
		TMap<uint32, TSharedPtr<FSubscriberContext>> SubscribersById;

		mutable FCriticalSection Guard;
	};
}
