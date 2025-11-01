// Copyright (c) Open3DStream Contributors

#pragma once

#include "CoreMinimal.h"

namespace O3DSHelpers
{
    // Replace whitespace with '_' and keep only [-._A-Za-z0-9/]
    OPEN3DSHARED_API FString SanitizeSubjectName(const FString& Raw);

    // Simple wildcard match supporting '*' and '?' (case-sensitive)
    OPEN3DSHARED_API bool NameMatchesPattern(const FString& Text, const FString& Pattern);

    // URL helpers
    // Split base and query string into key/value (lowercased keys/values)
    OPEN3DSHARED_API void UrlSplitQuery(const FString& InUrl, FString& OutBase, TMap<FString, FString>& OutQuery);

    // Return URL without its query
    OPEN3DSHARED_API FString StripQuery(const FString& InUrl);

    // Fix common tcp URL typo: tcp://host.port -> tcp://host:port (preserves query string if any)
    OPEN3DSHARED_API FString NormalizeTcpUrlHostPort(const FString& InUrl);

    // Hashing helpers (FNV-1a 64-bit)
    OPEN3DSHARED_API uint64 Fnv1a64(const void* Data, SIZE_T Bytes, uint64 Seed = 1469598103934665603ull);
    OPEN3DSHARED_API uint64 HashNames(const TArray<FName>& Names);
    OPEN3DSHARED_API uint64 HashNamesAndParents(const TArray<FName>& Names, const TArray<int32>& Parents);
}
