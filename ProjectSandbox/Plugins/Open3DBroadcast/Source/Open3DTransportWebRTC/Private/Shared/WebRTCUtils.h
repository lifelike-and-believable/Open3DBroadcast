#pragma once

#include "CoreMinimal.h"

namespace WebRTCUtils
{
    // Convert C-string to FString
    inline FString FromAnsi(const char* S)
    {
        return S ? FString(UTF8_TO_TCHAR(S)) : FString();
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

        // Determine if this is a localhost connection
        const bool bIsLocalhost = HostAddress.Equals(TEXT("127.0.0.1"), ESearchCase::IgnoreCase) ||
                                  HostAddress.Equals(TEXT("0.0.0.0"), ESearchCase::IgnoreCase) ||
                                  HostAddress.Equals(TEXT("localhost"), ESearchCase::IgnoreCase);

        // Use ws:// for localhost, wss:// for everything else
        const FString Protocol = bIsLocalhost ? TEXT("ws://") : TEXT("wss://");
        return Protocol + HostAddress;
    }
}
