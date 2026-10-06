// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"
#include "Transport/O3DTransportTypes.h"

namespace O3DNNG
{
    static constexpr TCHAR HostOptionKey[] = TEXT("host");
    static constexpr TCHAR PortOptionKey[] = TEXT("port");
    static constexpr TCHAR ModeOptionKey[] = TEXT("nng.mode");
    static constexpr TCHAR RoleOptionKey[] = TEXT("nng.role");
    static constexpr TCHAR QueueOptionKey[] = TEXT("nng.qmax");
    /** No longer supported (WP-R1): read only to warn that it is ignored. */
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

        bool bListen = false;
    };

    OPEN3DTRANSPORTNNG_API FString ModeToString(ENngMode Mode);
    OPEN3DTRANSPORTNNG_API ENngMode ModeFromString(const FString& ModeString, ENngMode DefaultMode);
    OPEN3DTRANSPORTNNG_API FString RoleToString(ENngRole Role);
    OPEN3DTRANSPORTNNG_API ENngRole RoleFromString(const FString& RoleString, ENngRole DefaultRole = ENngRole::None);

    /**
     * Mode and role rules, in one place (TRB-40). The module, the settings panels and the
     * option parser all use these, so both ends agree on who listens and who dials.
     *
     *   Sender:   pub  -> server (listen) only
     *             pair -> server (listen, default) or client (dial)
     *             push -> client (dial, default) or server (listen)
     *   Receiver: sub  -> client (dial) only
     *             pair -> client (dial, default) or server (listen)
     *             pull -> server (listen, default) or client (dial)
     *
     * With default roles, exactly one side of every mode pair listens.
     */
    OPEN3DTRANSPORTNNG_API bool IsModeSupported(ENngMode Mode, bool bSender);
    OPEN3DTRANSPORTNNG_API ENngRole GetDefaultRole(ENngMode Mode, bool bSender);
    OPEN3DTRANSPORTNNG_API bool IsRoleSupported(ENngMode Mode, ENngRole Role, bool bSender);
    /** Requested if the mode supports it for this side, otherwise the default role. */
    OPEN3DTRANSPORTNNG_API ENngRole ResolveRole(ENngMode Mode, ENngRole Requested, bool bSender);
    inline bool IsListenRole(ENngRole Role) { return Role == ENngRole::Server; }
    /** 0.0.0.0 for a listening socket, 127.0.0.1 for a dialing one. */
    OPEN3DTRANSPORTNNG_API FString GetDefaultHost(bool bListen);
    /** 6000 pub/sub, 7000 pair, 8000 push/pull. */
    OPEN3DTRANSPORTNNG_API int32 GetDefaultPort(ENngMode Mode);

    OPEN3DTRANSPORTNNG_API FString BuildCanonicalUri(ENngMode Mode, const FString& Host, int32 Port, ENngRole Role);
    OPEN3DTRANSPORTNNG_API FString MakeStreamId(const FString& Host, int32 Port);
    /**
     * A subscription topic Config still names (the nng.topic option, the Uri's path or ?topic=,
     * or the StreamId's path), or empty. Topics are not supported (WP-R1): the receiver warns.
     */
    OPEN3DTRANSPORTNNG_API FString FindIgnoredTopic(const FO3DTransportConfig& Config);

    OPEN3DTRANSPORTNNG_API bool ParseSenderOptions(const FO3DTransportConfig& Config, FNngSenderOptions& OutOptions, FString& OutError);
    OPEN3DTRANSPORTNNG_API bool ParseReceiverOptions(const FO3DTransportConfig& Config, FNngReceiverOptions& OutOptions, FString& OutError);

    /** The mode Config names (nng.mode, then the URI), as the option parsers read it; Pub when none. */
    OPEN3DTRANSPORTNNG_API ENngMode ResolveConfiguredMode(const FO3DTransportConfig& Config);

    /** Capabilities of an NNG socket in Mode (ADR 0007 item 4): pub/sub Unreliable, pair and push/pull ReliableOrdered. Any thread. */
    OPEN3DTRANSPORTNNG_API FO3DTransportCapabilities GetCapabilitiesForMode(ENngMode Mode);

    /** GetCapabilitiesForMode(ResolveConfiguredMode(Config)); the descriptor's capability query. Any thread. */
    OPEN3DTRANSPORTNNG_API FO3DTransportCapabilities GetCapabilities(const FO3DTransportConfig& Config);
}
