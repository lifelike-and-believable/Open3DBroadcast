#include "UOpen3DServer.h"
#include "o3ds/async_pair.h"
#include "o3ds/async_subscriber.h"
#include "IWebRTCConnector.h"
#include "O3DSUnifiedMessage.h"
#include "O3DSAudioBus.h"
#include "Open3DStreamSourceSettings.h"
#include "SocketSubsystem.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Common/UdpSocketBuilder.h"
#include "HAL/PlatformTime.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "Open3DStream"

// Debug: one-shot dump of first received non-TCP packet (WebRTC/UDP/NNG)
static TAutoConsoleVariable<int32> CVarO3DSDumpFirstPacket(
    TEXT("o3ds.Debug.DumpFirstPacket"),
    1,
    TEXT("Dump the first received non-TCP O3DS packet bytes (0=off,1=on)."),
    ECVF_Default);
static bool GDumpedFirstNonTcpPacket = false;

void InDataFunc(void* ptr, void* data, size_t msg)
{
	static_cast<O3DSServer*>(ptr)->inData((const uint8*)data, msg);
}

O3DSServer::O3DSServer()
	: mServer(nullptr)
	, mWebRTCConnector(nullptr)
	, mTcp(nullptr)
	, mUdp(nullptr)
	, mUdpReceiver(nullptr)
	, mBuffer(nullptr)
	, mBufferSize(0)
	, mPtr(0)
	, mGoodTime(0.0)
	, mNoDataFlag(false)
	, mState(eState::SYNC)
	, mTcpIp()
	, mTcpPort(0)
	, mLastTcpConnectAttempt(0.0)
	, mTcpBackoffAttempt(0)
	, mTcpAnnouncedConnected(false)
{
	// Initialize mGoodTime to current time to avoid immediate "No Data" warning
	mGoodTime = FPlatformTime::Seconds();
	
    // Lazy-create Opus decoder on first use; keep null until an Opus frame arrives
    OpusDec.Reset();
}

O3DSServer::~O3DSServer()
{
	stop();
}

static bool ParseTcpUrl(const FString& In, FString& OutIp, int32& OutPort)
{
	FString Work = In;
	if (Work.StartsWith(TEXT("tcp://")))
	{
		   Work.RightChopInline(6, EAllowShrinking::No);
	}
	int32 Colon = INDEX_NONE;
	if (!Work.FindChar(':', Colon))
	{
		return false;
	}
	OutIp = Work.Left(Colon);
	OutPort = FCString::Atoi(*Work.Mid(Colon + 1));
	return !OutIp.IsEmpty() && OutPort > 0;
}

bool O3DSServer::start(FText Url, FText Protocol, const FOpen3DStreamSettings* Settings)
{
	stop();

	const char* surl = TCHAR_TO_ANSI(*Url.ToString());
	const char* sprotocol = TCHAR_TO_ANSI(*Protocol.ToString());

	// Reset one-shot dump per start()
	GDumpedFirstNonTcpPacket = false;

	// NNG
	if (strncmp(sprotocol, "NNG Subscribe", 13) == 0)
	{
		mServer = new O3DS::AsyncSubscriber();
	}
	if (strncmp(sprotocol, "NNG Client", 10) == 0)
	{
		mServer = new O3DS::AsyncPairClient();
	}
	if (strncmp(sprotocol, "NNG Server", 10) == 0)
	{
		mServer = new O3DS::AsyncPairServer();
	}

	if (strncmp(sprotocol, "WebRTC Client", 13) == 0)
	{
		// Determine backend from settings (default to LibDataChannel)
		EO3DSWebRtcBackendReceiver Backend = EO3DSWebRtcBackendReceiver::LibDataChannel;
		if (Settings)
		{
			Backend = Settings->WebRtcBackend;
		}

		// Create connector using factory
		mWebRTCConnector = CreateWebRTCConnector(Backend);
		if (!mWebRTCConnector)
		{
			OnState.ExecuteIfBound(LOCTEXT("WebRTCBackendNotSupported", "WebRTC backend not supported"), true);
			return false;
		}

		mWebRTCConnector->SetDataReceivedCallback([this](const uint8* Data, int32 Size)
		{
			this->inData(Data, Size);
		});
		UE_LOG(LogTemp, Log, TEXT("O3DS RX: DataReceivedCallback bound in UOpen3DServer (WebRTC Client)"));
		
		if (mWebRTCConnector->Start(Url.ToString(), false))
		{
			OnState.ExecuteIfBound(LOCTEXT("WebRTCClientStarted", "WebRTC Client started."), false);
			return true;
		}
		else
		{
			OnState.ExecuteIfBound(FText::FromString(mWebRTCConnector->GetLastError()), true);
			mWebRTCConnector.Reset();
			return false;
		}
	}
	
	if (strncmp(sprotocol, "WebRTC Server", 13) == 0)
	{
		// Determine backend from settings (default to LibDataChannel)
		EO3DSWebRtcBackendReceiver Backend = EO3DSWebRtcBackendReceiver::LibDataChannel;
		if (Settings)
		{
			Backend = Settings->WebRtcBackend;
		}

		// Create connector using factory
		mWebRTCConnector = CreateWebRTCConnector(Backend);
		if (!mWebRTCConnector)
		{
			OnState.ExecuteIfBound(LOCTEXT("WebRTCBackendNotSupported", "WebRTC backend not supported"), true);
			return false;
		}

		mWebRTCConnector->SetDataReceivedCallback([this](const uint8* Data, int32 Size)
		{
			this->inData(Data, Size);
		});
		UE_LOG(LogTemp, Log, TEXT("O3DS RX: DataReceivedCallback bound in UOpen3DServer (WebRTC Server)"));
		
		if (mWebRTCConnector->Start(Url.ToString(), true))
		{
			OnState.ExecuteIfBound(LOCTEXT("WebRTCServerStarted", "WebRTC Server started."), false);
			return true;
		}
		else
		{
			OnState.ExecuteIfBound(FText::FromString(mWebRTCConnector->GetLastError()), true);
			mWebRTCConnector.Reset();
			return false;
		}
	}

	if (mServer)
	{
		mServer->setFunc(this, InDataFunc);
		if (mServer->start(surl)) {
			return true;
		}
		else {
			std::string err = mServer->getError();
			OnState.ExecuteIfBound(FText::FromString(ANSI_TO_TCHAR(err.c_str())), true);
			stop();
			return false;
		}
	}

	// UDP

	if (strcmp(sprotocol, "UDP Server") == 0)
	{
		FString parseme(surl);

		if (parseme.StartsWith("udp://")) {
			parseme = parseme.Right(parseme.Len() - 6);
		}

		if (parseme.StartsWith("tcp://")) {
			OnState.ExecuteIfBound(LOCTEXT("InvalidAddressUdpPrefix", "Invalid Address (TCP)"), true);
			return false;
		}

		int32 pos;
		if (parseme.FindChar(':', pos))
		{
			FString ip = parseme.Left(pos);
			FString port = parseme.Right(parseme.Len() - pos - 1);

			FIPv4Address address;

			// Support shorthands: "*" => any, "0.0.0.0" => any, "localhost" => loopback
			if (ip == TEXT("*") || ip == TEXT("0.0.0.0"))
			{
				address = FIPv4Address::Any;
			}
			else if (ip.Equals(TEXT("localhost"), ESearchCase::IgnoreCase))
			{
				address = FIPv4Address::InternalLoopback;
			}
			else
			{
				FIPv4Address::Parse(ip, address);
			}

			FIPv4Endpoint Endpoint(address, FCString::Atoi(*port));

			mUdp = FUdpSocketBuilder(TEXT("O3dsUdp"))
				.AsNonBlocking()
				.AsReusable()
				.BoundToEndpoint(Endpoint)
				.WithReceiveBufferSize(65507u);

			if (mUdp == nullptr) {
				OnState.ExecuteIfBound(LOCTEXT("InvalidUDP", "Could not create udp socket"), true);
				return false;
			}

			FTimespan ThreadWaitTime = FTimespan::FromMilliseconds(100);
			FString ThreadName = FString::Printf(TEXT("O3DS UDP Receiver"));

			if (mUdpReceiver) delete mUdpReceiver;
			mUdpReceiver = new FUdpSocketReceiver(mUdp, ThreadWaitTime, *ThreadName);

			mUdpReceiver->OnDataReceived().BindLambda([this](const FArrayReaderPtr& DataPtr, const FIPv4Endpoint& Endpoint)
				{
					TArray<uint8> Data;
					Data.AddUninitialized(DataPtr->TotalSize());
					DataPtr->Serialize(Data.GetData(), DataPtr->TotalSize());
					// Forward raw datagram as a complete O3DS message; broadcaster ensures one-message-per-datagram
					if (OnData.IsBound()) { OnData.Execute(Data); }
					mGoodTime = FPlatformTime::Seconds();
					mNoDataFlag = false;
				});

			mUdpReceiver->Start();

		}
	}

	// TCP

	if (strcmp(sprotocol, "TCP Client") == 0)
	{
		mState = eState::SYNC;
		mTcpAnnouncedConnected = false;

		FString parseme(surl);

		if (parseme.StartsWith("tcp://")) {
			parseme = parseme.Right(parseme.Len() - 6);
		}

		// Cache URL parts for retries and use non-blocking connect
		if (!ParseTcpUrl(parseme, mTcpIp, mTcpPort))
		{
			OnState.ExecuteIfBound(LOCTEXT("InvalidAddress", "Invalid Address"), true);
			return false;
		}

		mTcp = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_CLIENT"), false);
		if (!mTcp)
		{
			OnState.ExecuteIfBound(LOCTEXT("SocketCreateFailed", "TCP Socket Create Failed"), true);
			return false;
		}
		mTcp->SetNonBlocking();

		FIPv4Address address;
		FIPv4Address::Parse(mTcpIp, address);
		TSharedRef<FInternetAddr> addr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		addr->SetIp(address.Value);
		addr->SetPort(mTcpPort);

		(void)mTcp->Connect(*addr); // may be pending
		mLastTcpConnectAttempt = FPlatformTime::Seconds();
		mTcpBackoffAttempt = 0;
		mGoodTime = FPlatformTime::Seconds(); // Reset timer to avoid immediate "No Data" warning
		
		// Provide initial status
		OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPConnecting", "TCP Connecting to {0}:{1}..."), 
			FText::FromString(mTcpIp), FText::AsNumber(mTcpPort)), false);
		
		// Do not report failure immediately; Tick will resolve state
		return true;
	}

	return true;
}
void O3DSServer::stop()
{
	if (mServer)
	{
		mServer->stop();
		delete mServer;
		mServer = nullptr;
	}

	if (mWebRTCConnector)
	{
		mWebRTCConnector->Stop();
		mWebRTCConnector.Reset();
	}

	if (mUdp)
	{
		mUdpReceiver->Stop();
		delete mUdpReceiver;
		mUdpReceiver = nullptr;

		mUdp->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(mUdp);
		mUdp = nullptr;
	}

	if (mTcp)
	{
		mTcp->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(mTcp);
		mTcp = nullptr;
	}
	mTcpAnnouncedConnected = false;

	// Allow next session to dump first packet again
	    // Release Opus decoder
	    if (OpusDec)
	    {
	        OpusDec->Reset();
	        OpusDec.Reset();
	    }
	GDumpedFirstNonTcpPacket = false;
}

bool O3DSServer::write(const char *msg, size_t len)
{
	if (mWebRTCConnector)
	{
		// Default to lossy channel for streaming payloads; prefer Reliable for small control messages.
		return mWebRTCConnector->SendDataLossy(reinterpret_cast<const uint8*>(msg), static_cast<int32>(len));
	}
	
	if (!mServer) return false;
	return mServer->write(msg, len);
}

void O3DSServer::inData(const uint8 *msg, size_t len)
{
	// Update activity for WebRTC/UDP/NNG paths and forward payload
	mGoodTime = FPlatformTime::Seconds();
	mNoDataFlag = false;

	// Optional: lightweight verbose trace
	UE_LOG(LogTemp, Verbose, TEXT("O3DS: Received %d bytes (non-TCP)"), (int32)len);

	// One-shot dump of first packet to verify header
	if (!GDumpedFirstNonTcpPacket && CVarO3DSDumpFirstPacket->GetInt() != 0)
	{
		GDumpedFirstNonTcpPacket = true;
		const unsigned char ExpectedHeader[] = "\x00\xff\x03\xfeO3DS-START"; // 14 bytes
		const int32 DumpN = FMath::Min<int32>((int32)len, 64);
		FString Hex;
		Hex.Reserve(DumpN * 3);
		for (int32 i = 0; i < DumpN; ++i)
		{
			Hex += FString::Printf(TEXT("%02X "), msg[i]);
		}
		const bool bHeaderMatch = (len >= 14) && (FMemory::Memcmp(msg, ExpectedHeader, 14) == 0);
		UE_LOG(LogTemp, Warning, TEXT("O3DS: First non-TCP packet dump: size=%d header_match=%s first_%d_bytes=%s"),
			(int32)len, bHeaderMatch?TEXT("true"):TEXT("false"), DumpN, *Hex);
	}

	// If coming from WebRTC, support optional unified multiplexing header for audio frames.
	// We only special-case audio; mocap frames are forwarded as-is.
	if (mWebRTCConnector && msg && len >= sizeof(O3DS::FUnifiedHeader))
	{
		O3DS::FUnifiedHeader Hdr;
		const uint8* PayloadPtr = nullptr;
		int32 PayloadSize = 0;
		if (O3DS::ParseUnifiedMessage(msg, (int32)len, Hdr, PayloadPtr, PayloadSize))
		{
			if (Hdr.GetKind() == O3DS::EUnifiedKind::Audio)
			{
				// Decode simple metadata header (subject/label) if present at start of payload:
				// [uint8 LabelLen][char Label[LabelLen]][uint8 SubjectLen][char Subject[SubjectLen]] followed by raw PCM16
				const uint8* P = PayloadPtr;
				int32 R = PayloadSize;
				auto ReadByte = [&]() -> int32 { if (R <= 0) return -1; int32 V = *P; ++P; --R; return V; };
				FString Label, Subject;
				int32 LabelLen = ReadByte();
				if (LabelLen >= 0 && R >= LabelLen)
				{
					Label = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(P))).Left(LabelLen);
					P += LabelLen; R -= LabelLen;
				}
				int32 SubjectLen = ReadByte();
				if (SubjectLen >= 0 && R >= SubjectLen)
				{
					Subject = FString(UTF8_TO_TCHAR(reinterpret_cast<const char*>(P))).Left(SubjectLen);
					P += SubjectLen; R -= SubjectLen;
				}

				O3DS::FAudioFrameMeta Meta;
				Meta.StreamLabel = Label;
				Meta.SubjectName = Subject;
				Meta.NumChannels = 1; // default mono; future: encode channels
				Meta.SampleRate = 48000; // default
				Meta.TimestampSec = (double)Hdr.TimestampUs() / 1e6;

				if (Hdr.GetCodec() == O3DS::EUnifiedCodec::PCM16 && R > 0)
				{
					FO3DSAudioBus::PublishPcm16(Meta, P, R);
					return; // handled
				}

				if (Hdr.GetCodec() == O3DS::EUnifiedCodec::Opus && R > 0)
				{
					// Lazy init decoder
					if (!OpusDec)
					{
						OpusDec = MakeUnique<O3DS::FOpusDecoder>();
						if (!OpusDec->Init(Meta.SampleRate, Meta.NumChannels))
						{
							UE_LOG(LogTemp, Warning, TEXT("O3DS: Opus decoder init failed (sr=%d ch=%d)"), Meta.SampleRate, Meta.NumChannels);
							return;
						}
					}
					TArray<uint8> Pcm16;
					int32 Frames = 0;
					if (OpusDec->DecodeToPcm16(P, R, Pcm16, Frames) && Pcm16.Num() > 0)
					{
						FO3DSAudioBus::PublishPcm16(Meta, Pcm16.GetData(), Pcm16.Num());
					}
					else
					{
						UE_LOG(LogTemp, Verbose, TEXT("O3DS: Opus decode failed (%d bytes)"), R);
					}
					return; // handled
				}
				// Unknown/unsupported audio codec: drop for now
				return;
			}
			// If Kind=Mocap with header (not expected in this PR), fall through to default path after stripping header
		}
	}

	// Default: forward to animation pipeline
	{
		TArray<uint8> Data;
		Data.Append((uint8*)msg, len);
		if (OnData.IsBound()) { OnData.Execute(Data); }
	}
}

void O3DSServer::tick()
{
	uint32_t bucketSize;

	const unsigned char header[] = "\x00\xff\x03\xfeO3DS-START";

	// Process WebRTC messages if connected
	if (mWebRTCConnector)
	{
		mWebRTCConnector->Tick();
	}

	const double Now = FPlatformTime::Seconds();
	
	// TCP connection management
	if (!mTcpIp.IsEmpty() && mTcpPort > 0)
	{
		ESocketConnectionState ConnState = ESocketConnectionState::SCS_NotConnected;
		
		// Check for "No Data" warning only when connected
		if (mTcp)
		{
			ConnState = mTcp->GetConnectionState();
			bool bIsConnected = (ConnState == ESocketConnectionState::SCS_Connected);
			
			if (ConnState == ESocketConnectionState::SCS_Connected)
			{
				if (!mTcpAnnouncedConnected)
				{
					UE_LOG(LogTemp, Log, TEXT("O3DS: TCP Connected to %s:%d"), *mTcpIp, mTcpPort);
					OnState.ExecuteIfBound(LOCTEXT("TCPConnected", "TCP Connected"), false);
					mTcpAnnouncedConnected = true;
					mState = eState::SYNC; // reset parser on (re)connect
					mPtr = 0;
					mTcpBackoffAttempt = 0;
					mGoodTime = Now; // Reset data timer on connection
				}
				
				// If we haven't received data in a while and are connected, 
				// the connection might be dead - try a 0-byte read to probe it
				if (Now - mGoodTime > 2.0)
				{
					int32 Dummy = 0;
					uint8 ProbeBuffer;
					if (!mTcp->Recv(&ProbeBuffer, 0, Dummy))
					{
						ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
						// If we get an actual error (not just EWOULDBLOCK), the connection is dead
						if (Err != ESocketErrors::SE_EWOULDBLOCK && Err != ESocketErrors::SE_NO_ERROR)
						{
							UE_LOG(LogTemp, Warning, TEXT("O3DS: Socket probe detected dead connection (error %d)"), (int32)Err);
							
							OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPDisconnected", "TCP Disconnected from {0}:{1}, reconnecting..."), 
								FText::FromString(mTcpIp), FText::AsNumber(mTcpPort)), true);
							
							mTcp->Close();
							ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(mTcp);
							mTcp = nullptr;
							mTcpAnnouncedConnected = false;
							ConnState = ESocketConnectionState::SCS_NotConnected;
							mState = eState::SYNC;
							mPtr = 0;
							mLastTcpConnectAttempt = 0.0; // Allow immediate reconnection
							
							UE_LOG(LogTemp, Warning, TEXT("O3DS: Socket destroyed, will attempt reconnection"));
						}
					}
				}
				
				// Fallback: if no data for 5 seconds, force reconnection even if probe didn't detect error
				if (Now - mGoodTime > 5.0)
				{
					UE_LOG(LogTemp, Warning, TEXT("O3DS: No data for 5+ seconds, forcing reconnection"));
					
					OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPTimeout", "TCP Connection timeout, reconnecting to {0}:{1}..."), 
						FText::FromString(mTcpIp), FText::AsNumber(mTcpPort)), true);
					
					mTcp->Close();
					ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(mTcp);
					mTcp = nullptr;
					mTcpAnnouncedConnected = false;
					ConnState = ESocketConnectionState::SCS_NotConnected;
					mState = eState::SYNC;
					mPtr = 0;
					mLastTcpConnectAttempt = 0.0; // Allow immediate reconnection
				}
				
				// Show "No Data" warning if we haven't detected a dead connection
				if (!mNoDataFlag && Now - mGoodTime > 1.0)
				{
					UE_LOG(LogTemp, Warning, TEXT("O3DS: No data for %.1fs, socket state=%d"), Now - mGoodTime, (int32)ConnState);
					OnState.ExecuteIfBound(LOCTEXT("NoData", "No Data"), true);
					mNoDataFlag = true;
				}
			}
			else if (ConnState == ESocketConnectionState::SCS_ConnectionError)
			{
				// Connection attempt failed (not a disconnect during data transfer)
				bool bWasConnected = mTcpAnnouncedConnected;
				
				UE_LOG(LogTemp, Warning, TEXT("O3DS: Connection error detected, was_connected=%d"), bWasConnected);
				
				// Tear down and allow retry path below to recreate socket
				mTcp->Close();
				ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(mTcp);
				mTcp = nullptr;
				mTcpAnnouncedConnected = false;
				mState = eState::SYNC; // reset parser state
				mPtr = 0;
				
				// Announce if this was an unexpected disconnection (not initial connection failure)
				if (bWasConnected)
				{
					OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPDisconnected", "TCP Disconnected from {0}:{1}, reconnecting..."), 
						FText::FromString(mTcpIp), FText::AsNumber(mTcpPort)), true);
					// Reset backoff to allow quick reconnection after unexpected disconnect
					mLastTcpConnectAttempt = 0.0;
				}
			}
			// If pending connection, keep waiting without recreating socket
		}

		// Attempt (re)connect if no socket and backoff elapsed
		if (!mTcp)
		{
			const double Delay = FMath::Min(5.0, FMath::Pow(2.0, (double)FMath::Clamp(mTcpBackoffAttempt, 0, 5)) * 0.1);
			const double TimeSinceLastAttempt = Now - mLastTcpConnectAttempt;
			
			UE_LOG(LogTemp, Verbose, TEXT("O3DS: No socket - delay=%.2fs, time_since_last=%.2fs, backoff_attempt=%d"), 
				Delay, TimeSinceLastAttempt, mTcpBackoffAttempt);
			
			if (TimeSinceLastAttempt > Delay)
			{
				UE_LOG(LogTemp, Log, TEXT("O3DS: Attempting reconnection to %s:%d (attempt %d)"), *mTcpIp, mTcpPort, mTcpBackoffAttempt + 1);
				
				mLastTcpConnectAttempt = Now;
				
				mTcp = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateSocket(NAME_Stream, TEXT("O3DS_TCP_CLIENT"), false);
				if (!mTcp)
				{
					UE_LOG(LogTemp, Error, TEXT("O3DS: Failed to create socket!"));
					++mTcpBackoffAttempt;
					return; // will try again next tick
				}
				mTcp->SetNonBlocking();

				FIPv4Address address;
				FIPv4Address::Parse(mTcpIp, address);
				TSharedRef<FInternetAddr> addr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
				addr->SetIp(address.Value);
				addr->SetPort(mTcpPort);

				(void)mTcp->Connect(*addr); // initiate connect (may be pending)
				++mTcpBackoffAttempt;
				
				// Only show retry message if we've had multiple failures (not after a successful connection)
				if (mTcpBackoffAttempt > 3 && mTcpBackoffAttempt % 5 == 0)
				{
					OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPRetrying", "TCP Reconnecting to {0}:{1}... (attempt {2})"), 
						FText::FromString(mTcpIp), FText::AsNumber(mTcpPort), FText::AsNumber(mTcpBackoffAttempt)), false);
				}
			}
		}

		// Only read when connected to avoid reading during pending connection
		if (mTcp && mTcp->GetConnectionState() == ESocketConnectionState::SCS_Connected)
		{
			while (1)
			{
				int32 read = 0;

				if (mState == eState::SYNC)
				{
					mPtr = 0;

					if (!ReadTcp(1))
						return;

					if (mBuffer[0] != 0x00) continue;

					bool ok = true;
					for (int i = 1; i < 14; i++)
					{
						if (!ReadTcp(i+1))
							return;
						if (mBuffer[i] != header[i]) {
							ok = false;  break;
						}
					}
					if (!ok) 
					{
						// Reset to start sync search over
						mPtr = 0;
						continue;
					}

					mState = eState::HEADER;
				}

				if (mState == eState::HEADER)
				{
					while (mPtr < 18)
					{
						if (!ReadTcp(18))
							return;
					}

					if (strncmp((char*)mBuffer, (char*)header, 14) != 0)
					{
						OnState.ExecuteIfBound(LOCTEXT("MalformedData", "Malformed Data"), true);
						mState = eState::SYNC;
						mPtr = 0;
						continue;
					}
					mState = eState::DATA;
				}

				if (mState == eState::DATA)
				{
					bucketSize = *(uint32_t*)(mBuffer + 14);

					if (bucketSize > 1024 * 50)
					{
						OnState.ExecuteIfBound(LOCTEXT("MalformedData", "Malformed Data"), true);
						mState = eState::SYNC;
						mPtr = 0;
						continue;
					}

					while (mPtr < bucketSize + 18)
					{
						if (!ReadTcp(bucketSize + 18))
							return;
					}


					// Process
					TArray<uint8> Data;
					Data.Append((uint8*)(mBuffer + 18), bucketSize);
					OnData.Execute(Data);
					// Throttle 'Receiving Data' status: rely on NoData warning and suppress per-frame status spam

					mState = eState::HEADER;

					mPtr = 0;
				}
			}
		}
	}
	else
	{
		// Log if TCP info is missing
		static bool bLoggedMissingInfo = false;
		if (!bLoggedMissingInfo && (!mTcpIp.IsEmpty() || mTcpPort > 0))
		{
			UE_LOG(LogTemp, Warning, TEXT("O3DS: TCP info incomplete - IP='%s', Port=%d"), *mTcpIp, mTcpPort);
			bLoggedMissingInfo = true;
		}
	}
	
	// Handle other transport types (NNG, WebRTC, UDP)
	if (mServer || mWebRTCConnector || mUdp)
	{
		bool bHasActiveConnection = (mServer != nullptr) || (mWebRTCConnector != nullptr) || (mUdp != nullptr);
		if (bHasActiveConnection && !mNoDataFlag && Now - mGoodTime > 1.0)
		{
			OnState.ExecuteIfBound(LOCTEXT("NoData", "No Data"), true);
			mNoDataFlag = true;
		}
	}
}


bool O3DSServer::ReadTcp(size_t len)
{
	// Keep reading until the buffer is len bytes in size.
	// Return false if no data was read so we can exit the tick, without logging errors for EWOULDBLOCK.

	if (len <= mPtr)
		return true;

	if (mBuffer == nullptr)
	{
		size_t sz = len < 4096 ? 4096 : len;
		mBuffer = (uint8*)malloc(sz);
		mBufferSize = sz;
		mPtr = 0;
	}
	else
	{
		if (len > mBufferSize)
		{
			mBuffer = (uint8*)realloc(mBuffer, len);
			mBufferSize = len;
		}
	}

	int32 read = 0;
	if (!mTcp->Recv(mBuffer + mPtr, len - mPtr, read))
	{
		ESocketErrors Err = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->GetLastErrorCode();
		if (Err == ESocketErrors::SE_EWOULDBLOCK || Err == ESocketErrors::SE_NO_ERROR)
		{
			// Non-blocking and no data available yet; not a hard error
			return false;
		}
		// Fatal error (connection lost or other error); tear down socket and let tick() retry
		if (mTcpAnnouncedConnected)
		{
			OnState.ExecuteIfBound(FText::Format(LOCTEXT("TCPDisconnected", "TCP Disconnected from {0}:{1}, reconnecting..."), 
				FText::FromString(mTcpIp), FText::AsNumber(mTcpPort)), true);
		}
		else
		{
			OnState.ExecuteIfBound(LOCTEXT("TCPError", "TCP Connection Error"), true);
		}
		
		mTcp->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(mTcp);
		mTcp = nullptr;
		mTcpAnnouncedConnected = false;
		mState = eState::SYNC; // reset parser state on error
		mPtr = 0;
		// Reset backoff to allow immediate reconnection attempt after disconnect
		mLastTcpConnectAttempt = 0.0;
		return false;
	}

	if (read > 0)
	{
		mGoodTime = FPlatformTime::Seconds();
		mNoDataFlag = false;
	}

	mPtr += read;
	return read != 0;
}

#undef LOCTEXT_NAMESPACE
