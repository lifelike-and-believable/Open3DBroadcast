// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_NNG // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/NngHelpers.h"

#include "GenericPlatform/GenericPlatformHttp.h"
#include "Transport/O3DTransportOptions.h"

namespace O3DNNG
{
    constexpr uint64 kDefaultQueueBytes = 4ull * 1024ull * 1024ull;

    /** What a Uri's authority (host[:port]) turned out to be. */
    enum class ENngUriAuthority : uint8
    {
        /** No Uri, or a Uri without a host ("nng+pub://"). */
        Absent,
        /** A host, with or without a port. */
        Parsed,
        /** Something O3DTransportOptions::ParseHostPort refuses ("host:80abc", "[::1", "a b"). */
        Invalid,
    };

    FString FormatHostForUri(const FString& Host)
    {
        FString CleanHost = Host.TrimStartAndEnd();
        if (CleanHost.StartsWith(TEXT("[")) && CleanHost.EndsWith(TEXT("]")))
        {
            CleanHost = CleanHost.Mid(1, CleanHost.Len() - 2);
        }
        return CleanHost.Contains(TEXT(":")) ? FString::Printf(TEXT("[%s]"), *CleanHost) : CleanHost;
    }

    /**
     * A host with an optional port, parsed with O3DTransportOptions::ParseHostPort (WP-A1 PR 4d,
     * TRB-26): ports 1 to 65535 in digits only, bracketed IPv6. OutPort is 0 when Input has no
     * port. False when Input is malformed.
     */
    bool ParseNngHostPort(const FString& Input, FString& OutHost, int32& OutPort)
    {
        FO3DHostPort Endpoint;
        if (O3DTransportOptions::ParseHostPort(Input, Endpoint))
        {
            OutHost = Endpoint.Host;
            OutPort = Endpoint.Port;
            return true;
        }
        // A placeholder default port is accepted only when Input names no port at all: a port
        // that is present but malformed fails both calls.
        if (O3DTransportOptions::ParseHostPort(Input, Endpoint, /*DefaultPort=*/1))
        {
            OutHost = Endpoint.Host;
            OutPort = 0;
            return true;
        }
        return false;
    }

    void ParseQueryString(const FString& QueryString, TMap<FString, FString>& OutQuery)
    {
        OutQuery.Reset();

        TArray<FString> Pairs;
        QueryString.ParseIntoArray(Pairs, TEXT("&"), true);
        for (const FString& Pair : Pairs)
        {
            FString Key;
            FString Value;
            if (Pair.Split(TEXT("="), &Key, &Value))
            {
                Key.TrimStartAndEndInline();
                Value = FGenericPlatformHttp::UrlDecode(Value);
                OutQuery.Add(Key.ToLower(), Value);
            }
            else
            {
                FString Trimmed = Pair;
                Trimmed.TrimStartAndEndInline();
                if (!Trimmed.IsEmpty())
                {
                    OutQuery.Add(Trimmed.ToLower(), FString());
                }
            }
        }
    }

    /** Splits "nng+mode://host:port/topic?query" into its parts. The scheme is read by ExtractModeFromUri. */
    ENngUriAuthority ExtractHostPortFromUri(const FString& Uri, FString& OutHost, int32& OutPort, FString& OutPath, TMap<FString, FString>& OutQuery)
    {
        OutHost.Empty();
        OutPort = 0;
        OutPath.Empty();
        OutQuery.Reset();

        FString Working = Uri.TrimStartAndEnd();
        if (Working.IsEmpty())
        {
            return ENngUriAuthority::Absent;
        }

        int32 QuestionIdx = INDEX_NONE;
        if (Working.FindChar('?', QuestionIdx))
        {
            ParseQueryString(Working.Mid(QuestionIdx + 1), OutQuery);
            Working = Working.Left(QuestionIdx);
        }

        FString Scheme;
        FString Remainder;
        if (Working.Split(TEXT("://"), &Scheme, &Remainder))
        {
            Working = Remainder;
        }

        if (Working.StartsWith(TEXT("//")))
        {
            Working = Working.RightChop(2);
        }

        int32 SlashIdx = INDEX_NONE;
        if (Working.FindChar('/', SlashIdx))
        {
            OutPath = Working.Mid(SlashIdx + 1);
            Working = Working.Left(SlashIdx);
        }

        Working.TrimStartAndEndInline();
        OutPath.TrimStartAndEndInline();
        if (!OutPath.IsEmpty())
        {
            OutPath = FGenericPlatformHttp::UrlDecode(OutPath);
        }

        if (Working.IsEmpty())
        {
            return ENngUriAuthority::Absent;
        }
        return ParseNngHostPort(Working, OutHost, OutPort) ? ENngUriAuthority::Parsed : ENngUriAuthority::Invalid;
    }

    FString ExtractTopicFromStreamId(const FString& StreamId)
    {
        FString Working = StreamId;
        Working.TrimStartAndEndInline();

        int32 SlashIdx = INDEX_NONE;
        if (Working.FindChar('/', SlashIdx))
        {
            FString Topic = Working.Mid(SlashIdx + 1);
            Topic.TrimStartAndEndInline();
            return Topic;
        }

        return FString();
    }

    FString ExtractTopicFromUriParts(const FString& PathSegment, const TMap<FString, FString>& QueryParameters)
    {
        if (!PathSegment.IsEmpty())
        {
            return PathSegment;
        }

        if (const FString* TopicFromQuery = QueryParameters.Find(TEXT("topic")))
        {
            FString Result = *TopicFromQuery;
            Result.TrimStartAndEndInline();
            return Result;
        }

        return FString();
    }

    FString ExtractModeFromUri(const FString& Uri)
    {
        FString SchemePart;
        FString Remainder;
        if (Uri.Split(TEXT("://"), &SchemePart, &Remainder))
        {
            int32 PlusIdx = INDEX_NONE;
            if (SchemePart.FindLastChar('+', PlusIdx))
            {
                FString Mode = SchemePart.Mid(PlusIdx + 1);
                Mode.TrimStartAndEndInline();
                return Mode;
            }
        }
        return FString();
    }

    /** Option, then the Uri's ?mode= query, then the Uri scheme ("nng+sub://"). */
    FString ReadModeString(const FO3DTransportConfig& Config, const TMap<FString, FString>& UriQuery)
    {
        FString ModeString = O3DTransportOptions::GetString(Config.AdvancedParams, ModeOptionKey);
        if (ModeString.IsEmpty())
        {
            if (const FString* ModeOverride = UriQuery.Find(TEXT("mode")))
            {
                ModeString = ModeOverride->TrimStartAndEnd();
            }
        }
        if (ModeString.IsEmpty())
        {
            ModeString = ExtractModeFromUri(Config.Uri);
        }
        return ModeString;
    }

    /** Option, then the Uri's ?role= query. */
    FString ReadRoleString(const FO3DTransportConfig& Config, const TMap<FString, FString>& UriQuery)
    {
        FString RoleString = O3DTransportOptions::GetString(Config.AdvancedParams, RoleOptionKey);
        if (RoleString.IsEmpty())
        {
            if (const FString* RoleOverride = UriQuery.Find(TEXT("role")))
            {
                RoleString = RoleOverride->TrimStartAndEnd();
            }
        }
        return RoleString;
    }

    bool PopulateHostPort(const FO3DTransportConfig& Config, const FString& DefaultHost, ENngMode Mode, FString& OutHost, int32& OutPort, FString& OutError)
    {
        // TRB-39: "explicit" means the host option was set. DefaultHost is applied only at the
        // end, so a host from the Uri, its ?host= query or the StreamId is honoured.
        // WP-A1 PR 4d: every host and port goes through O3DTransportOptions, so a malformed one is
        // an error instead of being read as far as it parses ("6000abc" is not port 6000).
        OutPort = 0;
        FString Host;
        const FString HostOption = O3DTransportOptions::GetString(Config.AdvancedParams, HostOptionKey);
        if (!HostOption.IsEmpty())
        {
            int32 IgnoredPort = 0;
            if (!ParseNngHostPort(HostOption, Host, IgnoredPort) || IgnoredPort != 0)
            {
                OutError = FString::Printf(TEXT("Invalid host option '%s'"), *HostOption);
                return false;
            }
        }
        const FString PortOption = O3DTransportOptions::GetString(Config.AdvancedParams, PortOptionKey);
        if (!PortOption.IsEmpty() && !O3DTransportOptions::TryParsePort(PortOption, OutPort))
        {
            OutError = FString::Printf(TEXT("Invalid port option '%s' (1 to 65535)"), *PortOption);
            return false;
        }

        const bool bHostExplicit = !Host.IsEmpty();
        const bool bPortExplicit = OutPort > 0;

        FString UriHost;
        int32 UriPort = 0;
        FString UriPath;
        TMap<FString, FString> UriQuery;
        const ENngUriAuthority UriAuthority = ExtractHostPortFromUri(Config.Uri, UriHost, UriPort, UriPath, UriQuery);
        if (UriAuthority == ENngUriAuthority::Invalid)
        {
            OutError = FString::Printf(TEXT("Invalid host or port in Uri '%s'"), *Config.Uri);
            return false;
        }

        if (!bHostExplicit && !UriHost.IsEmpty())
        {
            Host = UriHost;
        }
        if (!bPortExplicit && UriPort > 0)
        {
            OutPort = UriPort;
        }

        if (!bHostExplicit)
        {
            if (const FString* HostOverride = UriQuery.Find(TEXT("host")))
            {
                const FString QueryHost = HostOverride->TrimStartAndEnd();
                if (!QueryHost.IsEmpty())
                {
                    int32 IgnoredPort = 0;
                    if (!ParseNngHostPort(QueryHost, Host, IgnoredPort) || IgnoredPort != 0)
                    {
                        OutError = FString::Printf(TEXT("Invalid ?host= in Uri '%s'"), *Config.Uri);
                        return false;
                    }
                }
            }
        }

        if (!bPortExplicit && OutPort <= 0)
        {
            if (const FString* PortOverride = UriQuery.Find(TEXT("port")))
            {
                const FString QueryPort = PortOverride->TrimStartAndEnd();
                if (!QueryPort.IsEmpty() && !O3DTransportOptions::TryParsePort(QueryPort, OutPort))
                {
                    OutError = FString::Printf(TEXT("Invalid ?port= in Uri '%s'"), *Config.Uri);
                    return false;
                }
            }
        }

        if (Host.IsEmpty() || OutPort <= 0)
        {
            // The StreamId ("host:port" or "host:port/topic") fills what is still missing. It is a
            // label as well, so one that is not an endpoint is ignored rather than refused.
            FString StreamIdHostPort = Config.StreamId.TrimStartAndEnd();
            int32 SlashIdx = INDEX_NONE;
            if (StreamIdHostPort.FindChar('/', SlashIdx))
            {
                StreamIdHostPort = StreamIdHostPort.Left(SlashIdx);
            }
            FO3DHostPort StreamEndpoint;
            if (!StreamIdHostPort.IsEmpty() && O3DTransportOptions::ParseHostPort(StreamIdHostPort, StreamEndpoint))
            {
                if (Host.IsEmpty())
                {
                    Host = StreamEndpoint.Host;
                }
                if (OutPort <= 0)
                {
                    OutPort = StreamEndpoint.Port;
                }
            }
        }

        if (OutPort <= 0)
        {
            OutPort = GetDefaultPort(Mode);
        }

        if (Host.IsEmpty())
        {
            Host = DefaultHost;
        }

        OutHost = FormatHostForUri(Host);
        return true;
    }

    FString BuildTcpAddress(const FString& Host, int32 Port)
    {
        return FString::Printf(TEXT("tcp://%s:%d"), *FormatHostForUri(Host), Port);
    }

    FString ModeToString(ENngMode Mode)
    {
        switch (Mode)
        {
        case ENngMode::Pub:
            return TEXT("pub");
        case ENngMode::Sub:
            return TEXT("sub");
        case ENngMode::Pair:
            return TEXT("pair");
        case ENngMode::Push:
            return TEXT("push");
        case ENngMode::Pull:
            return TEXT("pull");
        default:
            break;
        }
        return TEXT("pub");
    }

    ENngMode ModeFromString(const FString& ModeString, ENngMode DefaultMode)
    {
        if (ModeString.IsEmpty())
        {
            return DefaultMode;
        }

        if (ModeString.Equals(TEXT("pub"), ESearchCase::IgnoreCase))
        {
            return ENngMode::Pub;
        }
        if (ModeString.Equals(TEXT("sub"), ESearchCase::IgnoreCase))
        {
            return ENngMode::Sub;
        }
        if (ModeString.Equals(TEXT("pair"), ESearchCase::IgnoreCase))
        {
            return ENngMode::Pair;
        }
        if (ModeString.Equals(TEXT("push"), ESearchCase::IgnoreCase))
        {
            return ENngMode::Push;
        }
        if (ModeString.Equals(TEXT("pull"), ESearchCase::IgnoreCase))
        {
            return ENngMode::Pull;
        }

        return DefaultMode;
    }

    FString RoleToString(ENngRole Role)
    {
        switch (Role)
        {
        case ENngRole::Server:
            return TEXT("server");
        case ENngRole::Client:
            return TEXT("client");
        default:
            break;
        }
        return TEXT("none");
    }

    ENngRole RoleFromString(const FString& RoleString, ENngRole DefaultRole)
    {
        if (RoleString.IsEmpty())
        {
            return DefaultRole;
        }

        if (RoleString.Equals(TEXT("server"), ESearchCase::IgnoreCase))
        {
            return ENngRole::Server;
        }
        if (RoleString.Equals(TEXT("client"), ESearchCase::IgnoreCase))
        {
            return ENngRole::Client;
        }

        return DefaultRole;
    }

    bool IsModeSupported(ENngMode Mode, bool bSender)
    {
        switch (Mode)
        {
        case ENngMode::Pub:
        case ENngMode::Push:
            return bSender;
        case ENngMode::Sub:
        case ENngMode::Pull:
            return !bSender;
        case ENngMode::Pair:
            return true;
        default:
            return false;
        }
    }

    ENngRole GetDefaultRole(ENngMode Mode, bool bSender)
    {
        switch (Mode)
        {
        case ENngMode::Pub:
            return ENngRole::Server;
        case ENngMode::Sub:
            return ENngRole::Client;
        case ENngMode::Pair:
            // TRB-40: the sender listens and the receiver dials, so default Pair ends connect.
            return bSender ? ENngRole::Server : ENngRole::Client;
        case ENngMode::Push:
            return ENngRole::Client;
        case ENngMode::Pull:
            return ENngRole::Server;
        default:
            return bSender ? ENngRole::Server : ENngRole::Client;
        }
    }

    bool IsRoleSupported(ENngMode Mode, ENngRole Role, bool bSender)
    {
        if (!IsModeSupported(Mode, bSender) || Role == ENngRole::None)
        {
            return false;
        }

        switch (Mode)
        {
        case ENngMode::Pub:
            return Role == ENngRole::Server;
        case ENngMode::Sub:
            return Role == ENngRole::Client;
        case ENngMode::Pair:
        case ENngMode::Push:
        case ENngMode::Pull:
            return true;
        default:
            return false;
        }
    }

    ENngRole ResolveRole(ENngMode Mode, ENngRole Requested, bool bSender)
    {
        return IsRoleSupported(Mode, Requested, bSender) ? Requested : GetDefaultRole(Mode, bSender);
    }

    FString GetDefaultHost(bool bListen)
    {
        return bListen ? FString(TEXT("0.0.0.0")) : FString(TEXT("127.0.0.1"));
    }

    int32 GetDefaultPort(ENngMode Mode)
    {
        switch (Mode)
        {
        case ENngMode::Pair:
            return 7000;
        case ENngMode::Push:
        case ENngMode::Pull:
            return 8000;
        case ENngMode::Pub:
        case ENngMode::Sub:
        default:
            return 6000;
        }
    }

    FString BuildCanonicalUri(ENngMode Mode, const FString& Host, int32 Port, ENngRole Role, const FString& Topic)
    {
        const FString ModeSegment = ModeToString(Mode);
        FString Uri = FString::Printf(TEXT("nng+%s://%s:%d"), *ModeSegment, *FormatHostForUri(Host), Port);
        TArray<FString> QueryParts;

        if (Role != ENngRole::None && !(Mode == ENngMode::Pub && Role == ENngRole::Server) && !(Mode == ENngMode::Sub && Role == ENngRole::Client))
        {
            QueryParts.Add(FString::Printf(TEXT("role=%s"), *RoleToString(Role)));
        }

        if (!Topic.IsEmpty())
        {
            QueryParts.Add(FString::Printf(TEXT("topic=%s"), *FGenericPlatformHttp::UrlEncode(Topic)));
        }

        if (QueryParts.Num() > 0)
        {
            Uri += TEXT("?") + FString::Join(QueryParts, TEXT("&"));
        }

        return Uri;
    }

    FString MakeStreamId(const FString& Host, int32 Port, const FString& Topic)
    {
        FString StreamId = FString::Printf(TEXT("%s:%d"), *FormatHostForUri(Host), Port);
        if (!Topic.IsEmpty())
        {
            StreamId += FString::Printf(TEXT("/%s"), *Topic);
        }
        return StreamId;
    }

    /** Option, then the Uri path ("nng+sub://host:port/topic"), then its ?topic=, then the StreamId. */
    FString ReadTopic(const FO3DTransportConfig& Config, const FString& UriPath, const TMap<FString, FString>& UriQuery)
    {
        FString Topic = O3DTransportOptions::GetString(Config.AdvancedParams, TopicOptionKey);
        if (Topic.IsEmpty())
        {
            Topic = ExtractTopicFromUriParts(UriPath, UriQuery);
        }
        if (Topic.IsEmpty())
        {
            Topic = ExtractTopicFromStreamId(Config.StreamId);
        }
        return Topic;
    }

    void SetTopic(const FString& Topic, FString& OutTopic, TArray<uint8>& OutTopicUtf8)
    {
        OutTopic = Topic;
        OutTopicUtf8.Reset();
        if (!Topic.IsEmpty())
        {
            const FTCHARToUTF8 TopicUtf8(*Topic);
            OutTopicUtf8.Append(reinterpret_cast<const uint8*>(TopicUtf8.Get()), TopicUtf8.Length());
        }
    }

    bool ParseSenderOptions(const FO3DTransportConfig& Config, FNngSenderOptions& OutOptions, FString& OutError)
    {
        OutOptions = FNngSenderOptions();

        FString UriHost;
        int32 UriPort = 0;
        FString UriPath;
        TMap<FString, FString> UriQuery;
        ExtractHostPortFromUri(Config.Uri, UriHost, UriPort, UriPath, UriQuery);

        OutOptions.Mode = ModeFromString(ReadModeString(Config, UriQuery), ENngMode::Pub);
        if (!IsModeSupported(OutOptions.Mode, /*bSender=*/true))
        {
            OutError = TEXT("NNG sender does not support subscriber or pull modes");
            return false;
        }

        // Role first: it decides whether the default host is a bind-all or a loopback address.
        OutOptions.Role = ResolveRole(OutOptions.Mode, RoleFromString(ReadRoleString(Config, UriQuery), ENngRole::None), /*bSender=*/true);
        OutOptions.bListen = IsListenRole(OutOptions.Role);

        FString Host;
        int32 Port = 0;
        FString HostPortError;
        if (!PopulateHostPort(Config, GetDefaultHost(OutOptions.bListen), OutOptions.Mode, Host, Port, HostPortError))
        {
            OutError = FString::Printf(TEXT("Failed to parse host/port for NNG sender: %s"), *HostPortError);
            return false;
        }

        OutOptions.Host = Host;
        OutOptions.Port = Port;
        OutOptions.TcpAddress = BuildTcpAddress(Host, Port);
        SetTopic(ReadTopic(Config, UriPath, UriQuery), OutOptions.Topic, OutOptions.TopicUtf8);
        OutOptions.StreamId = MakeStreamId(Host, Port, OutOptions.Topic);

        // nng.qmax: bytes, digits only. Absent or 0 means the default (as before WP-A1 PR 4d).
        uint64 QueueBytes = kDefaultQueueBytes;
        const FString QueueString = O3DTransportOptions::GetString(Config.AdvancedParams, QueueOptionKey);
        if (!QueueString.IsEmpty())
        {
            int64 Parsed = 0;
            if (!O3DTransportOptions::TryParseInt(QueueString, Parsed) || Parsed < 0)
            {
                OutError = TEXT("Invalid queue size specified for NNG sender");
                return false;
            }
            QueueBytes = Parsed == 0 ? kDefaultQueueBytes : static_cast<uint64>(Parsed);
        }
        OutOptions.MaxQueueBytes = QueueBytes;

        OutOptions.CanonicalUri = BuildCanonicalUri(OutOptions.Mode, OutOptions.Host, OutOptions.Port, OutOptions.Role, OutOptions.Topic);
        return true;
    }

    bool ParseReceiverOptions(const FO3DTransportConfig& Config, FNngReceiverOptions& OutOptions, FString& OutError)
    {
        OutOptions = FNngReceiverOptions();

        FString UriHost;
        int32 UriPort = 0;
        FString UriPath;
        TMap<FString, FString> UriQuery;
        ExtractHostPortFromUri(Config.Uri, UriHost, UriPort, UriPath, UriQuery);

        OutOptions.Mode = ModeFromString(ReadModeString(Config, UriQuery), ENngMode::Sub);
        if (!IsModeSupported(OutOptions.Mode, /*bSender=*/false))
        {
            OutError = TEXT("NNG receiver mode must be sub, pair, or pull");
            return false;
        }

        // Role first: it decides whether the default host is a bind-all or a loopback address.
        OutOptions.Role = ResolveRole(OutOptions.Mode, RoleFromString(ReadRoleString(Config, UriQuery), ENngRole::None), /*bSender=*/false);
        OutOptions.bListen = IsListenRole(OutOptions.Role);

        SetTopic(ReadTopic(Config, UriPath, UriQuery), OutOptions.Topic, OutOptions.TopicUtf8);

        FString Host;
        int32 Port = 0;
        FString HostPortError;
        if (!PopulateHostPort(Config, GetDefaultHost(OutOptions.bListen), OutOptions.Mode, Host, Port, HostPortError))
        {
            OutError = FString::Printf(TEXT("Failed to parse host/port for NNG receiver: %s"), *HostPortError);
            return false;
        }

        OutOptions.Host = Host;
        OutOptions.Port = Port;
        OutOptions.TcpAddress = BuildTcpAddress(Host, Port);
        OutOptions.StreamId = MakeStreamId(Host, Port, OutOptions.Topic);

        OutOptions.CanonicalUri = BuildCanonicalUri(OutOptions.Mode, OutOptions.Host, OutOptions.Port, OutOptions.Role, OutOptions.Topic);
        return true;
    }

    ENngMode ResolveConfiguredMode(const FO3DTransportConfig& Config)
    {
        // The same precedence ParseSenderOptions and ParseReceiverOptions use: option, URI
        // query, URI scheme. Pub when nothing names a mode (the sender default).
        FString UriHost;
        int32 UriPort = 0;
        FString UriPath;
        TMap<FString, FString> UriQuery;
        ExtractHostPortFromUri(Config.Uri, UriHost, UriPort, UriPath, UriQuery);
        return ModeFromString(ReadModeString(Config, UriQuery), ENngMode::Pub);
    }

    FO3DTransportCapabilities GetCapabilitiesForMode(ENngMode Mode)
    {
        FO3DTransportCapabilities Caps;
        Caps.bSend = true;
        Caps.bReceive = true;
        Caps.bAudioSend = true;
        Caps.bAudioReceive = true;
        Caps.bControl = true;
        // ADR 0005 (iii): pair and push/pull over TCP are reliable and ordered; a pub socket drops
        // messages for a slow subscriber (needs-verification, ADR 0005 Q3).
        const bool bPubSub = Mode == ENngMode::Pub || Mode == ENngMode::Sub;
        Caps.Delivery = bPubSub ? EO3DDeliveryGuarantee::Unreliable : EO3DDeliveryGuarantee::ReliableOrdered;
        Caps.bBidirectional = Mode == ENngMode::Pair;
        // ADR 0005 (vi): the sender reports each added pipe (a subscriber, the pair peer, a pull
        // socket). A property of the transport, so the receiver modes report it too.
        Caps.bPeerJoinSignal = true;
        return Caps;
    }

    FO3DTransportCapabilities GetCapabilities(const FO3DTransportConfig& Config)
    {
        return GetCapabilitiesForMode(ResolveConfiguredMode(Config));
    }
}

#endif // O3D_WITH_TRANSPORT_NNG
