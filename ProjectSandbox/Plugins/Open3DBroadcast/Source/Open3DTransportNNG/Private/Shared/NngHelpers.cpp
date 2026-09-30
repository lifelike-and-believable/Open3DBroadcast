// Copyright (c) Open3DStream Contributors

#if O3D_WITH_TRANSPORT_NNG // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/NngHelpers.h"

#include "GenericPlatform/GenericPlatformHttp.h"

namespace O3DNNG
{
    constexpr uint64 kDefaultQueueBytes = 4ull * 1024ull * 1024ull;

    bool ParseInt(const FString& Input, int32& OutValue)
    {
        if (Input.IsEmpty())
        {
            return false;
        }

        TCHAR* EndPtr = nullptr;
        const int32 Base = 10;
        OutValue = FCString::Strtoi(*Input, &EndPtr, Base);
        return EndPtr != nullptr && *EndPtr == TEXT('\0');
    }

    bool ParseUInt64(const FString& Input, uint64& OutValue)
    {
        if (Input.IsEmpty())
        {
            return false;
        }

        TCHAR* EndPtr = nullptr;
        const int32 Base = 10;
        OutValue = FCString::Strtoui64(*Input, &EndPtr, Base);
        return EndPtr != nullptr && *EndPtr == TEXT('\0');
    }

    FString RemoveBrackets(const FString& Host)
    {
        if (Host.StartsWith(TEXT("[")) && Host.EndsWith(TEXT("]")))
        {
            return Host.Mid(1, Host.Len() - 2);
        }
        return Host;
    }

    bool ParseHostPortInternal(const FString& Input, FString& OutHost, int32& OutPort)
    {
        FString Working = Input;
        Working.TrimStartAndEndInline();
        if (Working.IsEmpty())
        {
            return false;
        }

        if (Working.StartsWith(TEXT("[")))
        {
            int32 ClosingIndex = 0;
            if (!Working.FindChar(']', ClosingIndex))
            {
                return false;
            }

            FString HostPart = Working.Mid(1, ClosingIndex - 1);
            const int32 ColonIndex = Working.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromStart, ClosingIndex);
            if (ColonIndex == INDEX_NONE)
            {
                return false;
            }

            FString PortPart = Working.Mid(ColonIndex + 1);
            PortPart.TrimStartAndEndInline();
            const int32 ParsedPort = FCString::Atoi(*PortPart);
            if (ParsedPort <= 0)
            {
                return false;
            }

            OutHost = HostPart;
            OutPort = ParsedPort;
            return true;
        }

        int32 ColonIndex = INDEX_NONE;
        if (!Working.FindLastChar(':', ColonIndex))
        {
            return false;
        }

        FString HostPart = Working.Left(ColonIndex);
        FString PortPart = Working.Mid(ColonIndex + 1);

        HostPart.TrimStartAndEndInline();
        PortPart.TrimStartAndEndInline();

        if (HostPart.IsEmpty() || PortPart.IsEmpty())
        {
            return false;
        }

        const int32 ParsedPort = FCString::Atoi(*PortPart);
        if (ParsedPort <= 0)
        {
            return false;
        }

        OutHost = HostPart;
        OutPort = ParsedPort;
        return true;
    }

    FString NormaliseHost(const FString& Host)
    {
        FString Result = RemoveBrackets(Host);
        Result.TrimStartAndEndInline();
        return Result;
    }

    FString AddIpv6BracketsIfNeeded(const FString& Host)
    {
        const bool bHasColon = Host.Contains(TEXT(":"));
        if (bHasColon && !Host.StartsWith(TEXT("[")))
        {
            return FString::Printf(TEXT("[%s]"), *Host);
        }
        return Host;
    }

    FString FormatHostForUri(const FString& Host)
    {
        FString CleanHost = RemoveBrackets(Host);
        CleanHost.TrimStartAndEndInline();
        return AddIpv6BracketsIfNeeded(CleanHost);
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

    bool ExtractHostPortFromUri(const FString& Uri, FString& OutHost, int32& OutPort, FString& OutPath, TMap<FString, FString>& OutQuery)
    {
        OutHost.Empty();
        OutPort = 0;
        OutPath.Empty();
        OutQuery.Reset();

        if (Uri.IsEmpty())
        {
            return false;
        }

        FString Working = Uri;
        FString QueryString;
        int32 QuestionIdx = INDEX_NONE;
        if (Working.FindChar('?', QuestionIdx))
        {
            QueryString = Working.Mid(QuestionIdx + 1);
            Working = Working.Left(QuestionIdx);
            ParseQueryString(QueryString, OutQuery);
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
            return false;
        }

        FString ParsedHost;
        int32 ParsedPort = 0;
        if (ParseHostPortInternal(Working, ParsedHost, ParsedPort))
        {
            OutHost = ParsedHost;
            OutPort = ParsedPort;
        }
        else
        {
            OutHost = Working;
        }

        return true;
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

    FString GetAdvancedOption(const FO3DTransportConfig& Config, const TCHAR* Key)
    {
        for (const TPair<FString, FString>& Pair : Config.AdvancedParams)
        {
            if (Pair.Key.Equals(Key, ESearchCase::IgnoreCase))
            {
                return Pair.Value;
            }
        }
        return FString();
    }

    bool PopulateHostPort(const FO3DTransportConfig& Config, const FString& DefaultHost, ENngMode Mode, FString& OutHost, int32& OutPort)
    {
        // TRB-39: "explicit" means the host option was set. DefaultHost is applied only at the
        // end, so a host from the Uri, its ?host= query or the StreamId is honoured.
        FString Host = NormaliseHost(GetAdvancedOption(Config, HostOptionKey));
        FString PortStr = GetAdvancedOption(Config, PortOptionKey);
        PortStr.TrimStartAndEndInline();

        const bool bHostExplicit = !Host.IsEmpty();
        const bool bPortExplicit = !PortStr.IsEmpty();

        if (bPortExplicit)
        {
            if (!ParseInt(PortStr, OutPort))
            {
                return false;
            }
        }

        FString UriHost;
        int32 UriPort = 0;
        FString UriPath;
        TMap<FString, FString> UriQuery;
        const bool bParsedUri = ExtractHostPortFromUri(Config.Uri, UriHost, UriPort, UriPath, UriQuery);
        if (bParsedUri)
        {
            UriHost = NormaliseHost(UriHost);

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
                    const FString QueryHost = NormaliseHost(*HostOverride);
                    if (!QueryHost.IsEmpty())
                    {
                        Host = QueryHost;
                    }
                }
            }

            if (!bPortExplicit && OutPort <= 0)
            {
                if (const FString* PortOverride = UriQuery.Find(TEXT("port")))
                {
                    int32 ParsedPort = 0;
                    if (ParseInt(*PortOverride, ParsedPort) && ParsedPort > 0)
                    {
                        OutPort = ParsedPort;
                    }
                }
            }
        }

        if (!bHostExplicit || !bPortExplicit)
        {
            FString StreamIdHostPort = Config.StreamId;
            if (!StreamIdHostPort.IsEmpty())
            {
                int32 SlashIdx = INDEX_NONE;
                if (StreamIdHostPort.FindChar('/', SlashIdx))
                {
                    StreamIdHostPort = StreamIdHostPort.Left(SlashIdx);
                }

                FString ParsedHost;
                int32 ParsedPort = 0;
                if (ParseHostPortInternal(StreamIdHostPort, ParsedHost, ParsedPort))
                {
                    ParsedHost = NormaliseHost(ParsedHost);
                    if (!bHostExplicit && !ParsedHost.IsEmpty())
                    {
                        Host = ParsedHost;
                    }
                    if (!bPortExplicit && ParsedPort > 0)
                    {
                        OutPort = ParsedPort;
                    }
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

        OutHost = AddIpv6BracketsIfNeeded(Host);
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

    bool ParseSenderOptions(const FO3DTransportConfig& Config, FNngSenderOptions& OutOptions, FString& OutError)
    {
        OutOptions = FNngSenderOptions();

        FString DummyUriHost;
        int32 DummyUriPort = 0;
        FString DummyUriPath;
        TMap<FString, FString> UriQuery;
        ExtractHostPortFromUri(Config.Uri, DummyUriHost, DummyUriPort, DummyUriPath, UriQuery);

        FString ModeString = GetAdvancedOption(Config, ModeOptionKey);
        if (ModeString.IsEmpty())
        {
            if (const FString* ModeOverride = UriQuery.Find(TEXT("mode")))
            {
                ModeString = *ModeOverride;
            }
        }
        if (ModeString.IsEmpty())
        {
            ModeString = ExtractModeFromUri(Config.Uri);
        }

        OutOptions.Mode = ModeFromString(ModeString, ENngMode::Pub);
        if (!IsModeSupported(OutOptions.Mode, /*bSender=*/true))
        {
            OutError = TEXT("NNG sender does not support subscriber or pull modes");
            return false;
        }

        // Role first: it decides whether the default host is a bind-all or a loopback address.
        FString RoleString = GetAdvancedOption(Config, RoleOptionKey);
        if (RoleString.IsEmpty())
        {
            if (const FString* RoleOverride = UriQuery.Find(TEXT("role")))
            {
                RoleString = *RoleOverride;
            }
        }
        OutOptions.Role = ResolveRole(OutOptions.Mode, RoleFromString(RoleString, ENngRole::None), /*bSender=*/true);
        OutOptions.bListen = IsListenRole(OutOptions.Role);

        FString Host;
        int32 Port = 0;
        if (!PopulateHostPort(Config, GetDefaultHost(OutOptions.bListen), OutOptions.Mode, Host, Port))
        {
            OutError = TEXT("Failed to parse host/port for NNG sender");
            return false;
        }

        FString Topic = GetAdvancedOption(Config, TopicOptionKey);
        Topic.TrimStartAndEndInline();
        if (Topic.IsEmpty())
        {
            Topic = ExtractTopicFromUriParts(DummyUriPath, UriQuery);
        }
        if (Topic.IsEmpty())
        {
            Topic = ExtractTopicFromStreamId(Config.StreamId);
        }

        OutOptions.Host = Host;
        OutOptions.Port = Port;
        OutOptions.TcpAddress = BuildTcpAddress(Host, Port);
        OutOptions.Topic = Topic;
        if (!Topic.IsEmpty())
        {
            const FTCHARToUTF8 TopicUtf8(*Topic);
            OutOptions.TopicUtf8.SetNumUninitialized(TopicUtf8.Length());
            if (TopicUtf8.Length() > 0)
            {
                FMemory::Memcpy(OutOptions.TopicUtf8.GetData(), TopicUtf8.Get(), TopicUtf8.Length());
            }
        }
        else
        {
            OutOptions.TopicUtf8.Reset();
        }
        OutOptions.StreamId = MakeStreamId(Host, Port, Topic);

        uint64 QueueBytes = kDefaultQueueBytes;
        const FString QueueString = GetAdvancedOption(Config, QueueOptionKey);
        if (!QueueString.IsEmpty() && !ParseUInt64(QueueString, QueueBytes))
        {
            OutError = TEXT("Invalid queue size specified for NNG sender");
            return false;
        }
        if (QueueBytes == 0)
        {
            QueueBytes = kDefaultQueueBytes;
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

        FString ModeString = GetAdvancedOption(Config, ModeOptionKey);
        if (ModeString.IsEmpty())
        {
            if (const FString* ModeOverride = UriQuery.Find(TEXT("mode")))
            {
                ModeString = *ModeOverride;
            }
        }
        if (ModeString.IsEmpty())
        {
            ModeString = ExtractModeFromUri(Config.Uri);
        }

        OutOptions.Mode = ModeFromString(ModeString, ENngMode::Sub);
        if (!IsModeSupported(OutOptions.Mode, /*bSender=*/false))
        {
            OutError = TEXT("NNG receiver mode must be sub, pair, or pull");
            return false;
        }

        // Role first: it decides whether the default host is a bind-all or a loopback address.
        FString RoleString = GetAdvancedOption(Config, RoleOptionKey);
        if (RoleString.IsEmpty())
        {
            if (const FString* RoleOverride = UriQuery.Find(TEXT("role")))
            {
                RoleString = *RoleOverride;
            }
        }
        OutOptions.Role = ResolveRole(OutOptions.Mode, RoleFromString(RoleString, ENngRole::None), /*bSender=*/false);
        OutOptions.bListen = IsListenRole(OutOptions.Role);

        FString Topic = GetAdvancedOption(Config, TopicOptionKey);
        Topic.TrimStartAndEndInline();
        if (Topic.IsEmpty())
        {
            Topic = ExtractTopicFromUriParts(UriPath, UriQuery);
        }
        if (Topic.IsEmpty())
        {
            Topic = ExtractTopicFromStreamId(Config.StreamId);
        }
        OutOptions.Topic = Topic;
        if (!Topic.IsEmpty())
        {
            const FTCHARToUTF8 TopicUtf8(*Topic);
            OutOptions.TopicUtf8.SetNumUninitialized(TopicUtf8.Length());
            if (TopicUtf8.Length() > 0)
            {
                FMemory::Memcpy(OutOptions.TopicUtf8.GetData(), TopicUtf8.Get(), TopicUtf8.Length());
            }
        }
        else
        {
            OutOptions.TopicUtf8.Reset();
        }

        FString Host;
        int32 Port = 0;
        if (!PopulateHostPort(Config, GetDefaultHost(OutOptions.bListen), OutOptions.Mode, Host, Port))
        {
            OutError = TEXT("Failed to parse host/port for NNG receiver");
            return false;
        }

        OutOptions.Host = Host;
        OutOptions.Port = Port;
        OutOptions.TcpAddress = BuildTcpAddress(Host, Port);
        OutOptions.StreamId = MakeStreamId(Host, Port, Topic);

        OutOptions.CanonicalUri = BuildCanonicalUri(OutOptions.Mode, OutOptions.Host, OutOptions.Port, OutOptions.Role, OutOptions.Topic);
        return true;
    }
}

#endif // O3D_WITH_TRANSPORT_NNG
