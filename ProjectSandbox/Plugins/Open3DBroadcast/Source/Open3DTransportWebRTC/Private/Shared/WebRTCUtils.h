// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Guid.h"
#include "Containers/StringConv.h"

namespace WebRTCUtils
{
    /** Transport option naming the LiveKit room both sides join in auto-fetch mode (TRF-25). */
    static constexpr TCHAR RoomOptionKey[] = TEXT("webrtc.room");

    /**
     * Secret option keys (ADR 0004). Declared in the customizations' SecretOptionKeys, so their
     * values live in FO3DSecretStore and reach the transport only through FO3DTransportConfig::Secrets.
     */
    /** LiveKit access token for manual token mode. */
    static constexpr TCHAR TokenOptionKey[] = TEXT("webrtc.token");
    /** Credential sent as "Authorization: Bearer <value>" to the token endpoint in auto-fetch mode. */
    static constexpr TCHAR TokenEndpointAuthOptionKey[] = TEXT("webrtc.tokenEndpointAuth");

    /** Environment variables for the secret keys; a non-default profile first tries "<NAME>__<PROFILE>". */
    static constexpr TCHAR TokenEnvVar[] = TEXT("O3DB_WEBRTC_TOKEN");
    static constexpr TCHAR TokenEndpointAuthEnvVar[] = TEXT("O3DB_WEBRTC_TOKEN_ENDPOINT_AUTH");

    /** Returns the resolved secret for Key, or an empty string. */
    inline FString FindSecret(const TMap<FString, FString>& Secrets, const TCHAR* Key)
    {
        const FString* Value = Secrets.Find(Key);
        return Value ? *Value : FString();
    }

    // Convert a UTF-8 C string (LiveKit messages, labels, track names) to FString.
    inline FString FromAnsi(const char* S)
    {
        return S ? FString(UTF8_TO_TCHAR(S)) : FString();
    }

    /**
     * Decodes a UTF-8 label from LiveKit (data channel label or audio track name). The sender
     * encodes labels with FTCHARToUTF8, so this is the inverse (TRF-31). Returns an empty
     * string for null or empty input.
     */
    inline FString DecodeUtf8Label(const char* Utf8)
    {
        if (!Utf8 || !*Utf8)
        {
            return FString();
        }
        const FUTF8ToTCHAR Converter(Utf8);
        return FString(Converter.Length(), Converter.Get());
    }

    /** Returns the trimmed `webrtc.room` option, or an empty string when it is not set. */
    inline FString ResolveRoomName(const TMap<FString, FString>& AdvancedParams)
    {
        if (const FString* Value = AdvancedParams.Find(RoomOptionKey))
        {
            return Value->TrimStartAndEnd();
        }
        return FString();
    }

    /**
     * Builds a LiveKit participant identity that is unique per transport instance, so two
     * senders (or receivers) in one process never share one (TRF-25: DUPLICATE_IDENTITY).
     */
    inline FString MakeParticipantIdentity(const TCHAR* Prefix)
    {
        return FString::Printf(TEXT("%s-%u-%s"), Prefix, FPlatformProcess::GetCurrentProcessId(),
            *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
    }

    /**
     * Prepends the correct WebSocket protocol prefix (ws:// or wss://) to a host address.
     *
     * Protocol selection logic:
     * - ws:// for localhost connections: 127.0.0.1, 0.0.0.0, localhost
     * - wss:// for all other addresses: URLs, domain names, public IP addresses
     *
     * @param HostAddress The host address without protocol prefix (e.g., "livkit.example.com" or "127.0.0.1")
     * @return The complete WebSocket URL with appropriate protocol prefix
     *
     * Examples:
     *   "127.0.0.1" → "ws://127.0.0.1"
     *   "127.0.0.1:7880" → "ws://127.0.0.1:7880" (the port is ignored when classifying the host)
     *   "localhost" → "ws://localhost"
     *   "0.0.0.0" → "ws://0.0.0.0"
     *   "livkit.example.com" → "wss://livkit.example.com"
     *   "1.2.3.4" → "wss://1.2.3.4"
     */
    inline FString PrependWebSocketProtocol(const FString& HostAddress)
    {
        if (HostAddress.IsEmpty())
        {
            return FString();
        }

        // Check if the address already has a protocol prefix
        if (HostAddress.StartsWith(TEXT("ws://"), ESearchCase::IgnoreCase) ||
            HostAddress.StartsWith(TEXT("wss://"), ESearchCase::IgnoreCase))
        {
            return HostAddress;
        }

        // Determine if this is a localhost connection. Compare the host part only: an address
        // usually carries a port ("127.0.0.1:7880", LiveKit's dev default) and may carry a path.
        FString Host = HostAddress;
        int32 SeparatorIndex = INDEX_NONE;
        if (Host.FindChar(TEXT('/'), SeparatorIndex))
        {
            Host.LeftInline(SeparatorIndex);
        }
        if (!Host.StartsWith(TEXT("[")) && Host.FindLastChar(TEXT(':'), SeparatorIndex))
        {
            Host.LeftInline(SeparatorIndex); // "host:port"; bracketed IPv6 keeps its colons
        }
        else if (Host.StartsWith(TEXT("[")) && Host.FindChar(TEXT(']'), SeparatorIndex))
        {
            Host.LeftInline(SeparatorIndex + 1); // "[::1]:7880" -> "[::1]"
        }

        const bool bIsLocalhost = Host.Equals(TEXT("127.0.0.1"), ESearchCase::IgnoreCase) ||
                                  Host.Equals(TEXT("0.0.0.0"), ESearchCase::IgnoreCase) ||
                                  Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) ||
                                  Host.Equals(TEXT("[::1]"), ESearchCase::IgnoreCase);

        // Use ws:// for localhost, wss:// for everything else
        const FString Protocol = bIsLocalhost ? TEXT("ws://") : TEXT("wss://");
        return Protocol + HostAddress;
    }
}
