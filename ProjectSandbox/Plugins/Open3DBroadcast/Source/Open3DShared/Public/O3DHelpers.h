// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

namespace O3DHelpers
{
    // Replace each space with '_', then keep only [-._A-Za-z0-9/]: any other character, tabs and
    // newlines included, is dropped.
    OPEN3DSHARED_API FString SanitizeSubjectName(const FString& Raw);

    // Simple wildcard match supporting '*' and '?' (case-sensitive)
    OPEN3DSHARED_API bool NameMatchesPattern(const FString& Text, const FString& Pattern);

    // UrlSplitQuery and StripQuery were deleted (SHR-34): unused, and UrlSplitQuery lowercased
    // the values. Parse URLs with FURL, FParse or FGenericPlatformHttp::UrlDecode.

    // NormalizeTcpUrlHostPort was deleted (SHR-9: it rewrote tcp://192.168.1.10 to
    // tcp://192.168.1:10). Parse endpoints with O3DTransportOptions::ParseHostPort.

    // True for https:// URLs, and for http:// URLs whose host is localhost, 127.0.0.1 or ::1.
    // Every other URL (plain http:// to another host, other schemes, no scheme) is false.
    // Used to refuse credential-bearing requests over plain HTTP (ADR 0004 item 6).
    OPEN3DSHARED_API bool IsHttpsOrLoopbackHttpUrl(const FString& InUrl);

    // Hashing helpers (FNV-1a 64-bit). HashNames and HashNamesAndParents use the core's
    // length-prefixed UTF-8 definition (o3ds/wire_format.h, ADR 0009 item 8): local change
    // detection only, never on the wire.
    OPEN3DSHARED_API uint64 Fnv1a64(const void* Data, SIZE_T Bytes, uint64 Seed = 1469598103934665603ull);
    OPEN3DSHARED_API uint64 HashNames(const TArray<FName>& Names);
    OPEN3DSHARED_API uint64 HashNamesAndParents(const TArray<FName>& Names, const TArray<int32>& Parents);
}
