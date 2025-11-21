// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

#include "MoQ/MoQProtocol.h"
#include "O3DTransportTypes.h"

namespace O3DQuic
{
	static constexpr uint16 DefaultQuicPort = 4700;

	static constexpr TCHAR SenderHostOptionKey[] = TEXT("quic.host");
	static constexpr TCHAR SenderPortOptionKey[] = TEXT("quic.port");
	static constexpr TCHAR SenderTrackNameOptionKey[] = TEXT("quic.track_name");
	static constexpr TCHAR SenderPriorityOptionKey[] = TEXT("quic.priority");
	static constexpr TCHAR SenderReliabilityOptionKey[] = TEXT("quic.reliability");
	static constexpr TCHAR SenderDatagramOptionKey[] = TEXT("quic.datagrams");

	static constexpr TCHAR QuicSchemeName[] = TEXT("quic");
	static constexpr TCHAR DefaultTrackName[] = TEXT("mocap/default");
	static constexpr TCHAR DefaultAlpn[] = TEXT("moq-00");

	struct OPEN3DTRANSPORTQUIC_API FQuicEndpoint
	{
		FString Host;
		uint16 Port = 0;

		bool IsValid() const
		{
			return !Host.IsEmpty() && Port > 0;
		}
	};

	struct OPEN3DTRANSPORTQUIC_API FQuicSenderOptions
	{
		FQuicEndpoint Endpoint;
		FString StreamId;
		FString TrackName;
		O3DMoQ::FMoQTrackProperties TrackProperties;
		bool bEnableDatagrams = true;
		FString Alpn;

		FString Describe() const;
	};

	OPEN3DTRANSPORTQUIC_API bool ParseSenderOptions(const FO3DTransportConfig& Config, FQuicSenderOptions& OutOptions, FString& OutError);
}
