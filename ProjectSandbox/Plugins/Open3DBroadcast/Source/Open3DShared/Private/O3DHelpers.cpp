// Copyright 2026 Lifelike & Believable. All Rights Reserved.
// Portions Copyright (c) Open3DStream Contributors

#include "O3DHelpers.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/wire_format.h"
THIRD_PARTY_INCLUDES_END

namespace O3DHelpers
{
    FString SanitizeSubjectName(const FString& Raw)
    {
        FString Out = Raw;
        Out = Out.Replace(TEXT(" "), TEXT("_"));

        FString Result;
        Result.Reserve(Out.Len());
        for (int32 i = 0; i < Out.Len(); ++i)
        {
            const TCHAR C = Out[i];
            bool bAllow = false;
            if ((C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9'))
            {
                bAllow = true;
            }
            else if (C == '-' || C == '.' || C == '_' || C == '/')
            {
                bAllow = true;
            }

            if (bAllow)
            {
                Result.AppendChar(C);
            }
        }
        return Result;
    }

    bool NameMatchesPattern(const FString& Text, const FString& Pattern)
    {
        auto Match = [](const TCHAR* str, const TCHAR* pat) -> bool
        {
            const TCHAR* s = str;
            const TCHAR* p = pat;
            const TCHAR* star = nullptr;
            const TCHAR* ss = nullptr;
            while (*s)
            {
                if (*p == '?' || *p == *s)
                {
                    ++s; ++p;
                }
                else if (*p == '*')
                {
                    star = p++;
                    ss = s;
                }
                else if (star)
                {
                    p = star + 1;
                    s = ++ss;
                }
                else
                {
                    return false;
                }
            }
            while (*p == '*') { ++p; }
            return *p == 0;
        };

        return Match(*Text, *Pattern);
    }

    bool IsHttpsOrLoopbackHttpUrl(const FString& InUrl)
    {
        const FString Trimmed = InUrl.TrimStartAndEnd();
        static constexpr TCHAR HttpsPrefix[] = TEXT("https://");
        static constexpr TCHAR HttpPrefix[] = TEXT("http://");

        const bool bHttps = Trimmed.StartsWith(HttpsPrefix, ESearchCase::IgnoreCase);
        const bool bHttp = !bHttps && Trimmed.StartsWith(HttpPrefix, ESearchCase::IgnoreCase);
        if (!bHttps && !bHttp)
        {
            return false;
        }

        const int32 PrefixLen = bHttps ? UE_ARRAY_COUNT(HttpsPrefix) - 1 : UE_ARRAY_COUNT(HttpPrefix) - 1;
        FString Authority = Trimmed.Mid(PrefixLen);
        for (int32 i = 0; i < Authority.Len(); ++i)
        {
            const TCHAR C = Authority[i];
            if (C == '/' || C == '?' || C == '#')
            {
                Authority.LeftInline(i);
                break;
            }
        }

        int32 AtIdx = INDEX_NONE;
        if (Authority.FindLastChar('@', AtIdx))
        {
            Authority.RightChopInline(AtIdx + 1);
        }

        FString Host = Authority;
        if (Host.StartsWith(TEXT("[")))
        {
            int32 CloseIdx = INDEX_NONE;
            Host = Host.FindChar(']', CloseIdx) ? Host.Mid(1, CloseIdx - 1) : FString();
        }
        else
        {
            int32 FirstColon = INDEX_NONE;
            int32 LastColon = INDEX_NONE;
            if (Host.FindChar(':', FirstColon) && Host.FindLastChar(':', LastColon) && FirstColon == LastColon)
            {
                Host.LeftInline(FirstColon); // host:port
            }
        }

        if (Host.IsEmpty())
        {
            return false;
        }

        if (bHttps)
        {
            return true;
        }

        return Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase)
            || Host.Equals(TEXT("127.0.0.1"))
            || Host.Equals(TEXT("::1"));
    }

    uint64 Fnv1a64(const void* Data, SIZE_T Bytes, uint64 Seed)
    {
        uint64 H = Seed;
        const uint8* P = static_cast<const uint8*>(Data);
        for (SIZE_T i = 0; i < Bytes; ++i)
        {
            H ^= P[i];
            H *= 1099511628211ull;
        }
        return H;
    }

    // The core's length-prefixed UTF-8 definition (ADR 0009 item 8, SHR-33): the old hash fed each
    // name's TCHAR bytes with no length or count, so different lists could collide.
    uint64 HashNames(const TArray<FName>& Names)
    {
        uint64 H = O3DS::Wire::HashNamesBegin(static_cast<uint32>(Names.Num()));
        for (const FName& N : Names)
        {
            const FString S = N.ToString();
            const FTCHARToUTF8 Utf8(*S);
            H = O3DS::Wire::HashNamesAdd(H, Utf8.Get(), static_cast<uint32>(Utf8.Length()));
        }
        return H;
    }

    uint64 HashNamesAndParents(const TArray<FName>& Names, const TArray<int32>& Parents)
    {
        return O3DS::Wire::HashParents(HashNames(Names), Parents.GetData(), static_cast<uint32>(Parents.Num()));
    }
}