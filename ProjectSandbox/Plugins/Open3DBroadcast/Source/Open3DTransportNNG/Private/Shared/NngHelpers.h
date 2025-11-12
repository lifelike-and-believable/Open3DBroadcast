// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "O3DTransportTypes.h"

namespace O3DNNG
{
    static constexpr TCHAR HostOptionKey[] = TEXT("host");
    static constexpr TCHAR PortOptionKey[] = TEXT("port");
    static constexpr TCHAR ModeOptionKey[] = TEXT("nng.mode");
    static constexpr TCHAR RoleOptionKey[] = TEXT("nng.role");
    static constexpr TCHAR QueueOptionKey[] = TEXT("nng.qmax");
    static constexpr TCHAR TopicOptionKey[] = TEXT("nng.topic");

    enum class ENngMode : uint8
    {
        Pub,
        Sub,
        Pair,
        Push,
        Pull
    };

    enum class ENngRole : uint8
    {
        None,
        Server,
        Client
    };

    struct FNngSenderOptions
    {
        ENngMode Mode = ENngMode::Pub;
        ENngRole Role = ENngRole::Server;
        FString Host;
        int32 Port = 0;
        FString TcpAddress;
        FString CanonicalUri;
        FString StreamId;
        FString Topic;
        TArray<uint8> TopicUtf8;
        uint64 MaxQueueBytes = 4ull * 1024ull * 1024ull;

        bool bListen = true;
    };

    struct FNngReceiverOptions
    {
        ENngMode Mode = ENngMode::Sub;
        ENngRole Role = ENngRole::Client;
        FString Host;
        int32 Port = 0;
        FString TcpAddress;
        FString CanonicalUri;
        FString StreamId;
        FString Topic;
        TArray<uint8> TopicUtf8;

        bool bListen = false;
    };

    OPEN3DTRANSPORTNNG_API FString ModeToString(ENngMode Mode);
    OPEN3DTRANSPORTNNG_API ENngMode ModeFromString(const FString& ModeString, ENngMode DefaultMode);
    OPEN3DTRANSPORTNNG_API FString RoleToString(ENngRole Role);
    OPEN3DTRANSPORTNNG_API ENngRole RoleFromString(const FString& RoleString, ENngRole DefaultRole = ENngRole::None);

    OPEN3DTRANSPORTNNG_API FString BuildCanonicalUri(ENngMode Mode, const FString& Host, int32 Port, ENngRole Role, const FString& Topic);
    OPEN3DTRANSPORTNNG_API FString MakeStreamId(const FString& Host, int32 Port, const FString& Topic);

    OPEN3DTRANSPORTNNG_API bool ParseSenderOptions(const FO3DTransportConfig& Config, FNngSenderOptions& OutOptions, FString& OutError);
    OPEN3DTRANSPORTNNG_API bool ParseReceiverOptions(const FO3DTransportConfig& Config, FNngReceiverOptions& OutOptions, FString& OutError);
}
