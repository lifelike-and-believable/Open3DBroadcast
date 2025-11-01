// Copyright (c) Open3DStream Contributors

#include "O3DSHelpers.h"

namespace O3DSHelpers
{
    FString SanitizeSubjectName(const FString& Raw)
    {
        // Replace whitespace with underscore
        FString Out = Raw;
        Out = Out.Replace(TEXT(" "), TEXT("_"));

        // Keep only [-._A-Za-z0-9/]
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
        // iterative backtracking for '*'
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

    void UrlSplitQuery(const FString& InUrl, FString& OutBase, TMap<FString, FString>& OutQuery)
    {
        OutBase = InUrl;
        OutQuery.Reset();
        int32 QIdx;
        if (InUrl.FindChar('?', QIdx))
        {
            OutBase = InUrl.Left(QIdx);
            const FString Qs = InUrl.Mid(QIdx + 1);
            TArray<FString> Pairs; Qs.ParseIntoArray(Pairs, TEXT("&"), true);
            for (const FString& P : Pairs)
            {
                FString K, V;
                if (P.Split(TEXT("="), &K, &V))
                {
                    OutQuery.Add(K.ToLower(), V.ToLower());
                }
                else if (!P.IsEmpty())
                {
                    OutQuery.Add(P.ToLower(), TEXT(""));
                }
            }
        }
    }

    FString StripQuery(const FString& InUrl)
    {
        int32 QIdx; return InUrl.FindChar('?', QIdx) ? InUrl.Left(QIdx) : InUrl;
    }

    FString NormalizeTcpUrlHostPort(const FString& InUrl)
    {
        FString Out = InUrl;
        if (!Out.StartsWith(TEXT("tcp://"), ESearchCase::IgnoreCase))
        {
            return Out;
        }

        FString Base = Out;
        FString QueryPart;
        int32 QIdx;
        if (Out.FindChar('?', QIdx))
        {
            Base = Out.Left(QIdx);
            QueryPart = Out.Mid(QIdx); // keep leading '?'
        }

        const int32 SchemeLen = 6; // "tcp://"
        if (Base.Len() <= SchemeLen)
        {
            return Out;
        }

        const FString HostPort = Base.Mid(SchemeLen);
        if (HostPort.Contains(TEXT(":")))
        {
            return Out; // already has ':' (or IPv6)
        }

        int32 LastDotIdx;
        if (HostPort.FindLastChar('.', LastDotIdx))
        {
            const FString PortStr = HostPort.Mid(LastDotIdx + 1);
            bool bAllDigits = !PortStr.IsEmpty();
            for (int32 i = 0; i < PortStr.Len(); ++i)
            {
                const TCHAR C = PortStr[i];
                if (C < '0' || C > '9') { bAllDigits = false; break; }
            }
            if (bAllDigits)
            {
                const FString Before = Base.Left(SchemeLen + LastDotIdx);
                const FString After = Base.Mid(SchemeLen + LastDotIdx + 1);
                const FString FixedBase = Before + TEXT(":") + After;
                return FixedBase + QueryPart;
            }
        }

        return Out;
    }

    uint64 Fnv1a64(const void* Data, SIZE_T Bytes, uint64 Seed)
    {
        uint64 H = Seed;
        const uint8* P = static_cast<const uint8*>(Data);
        for (SIZE_T i = 0; i < Bytes; ++i)
        {
            H ^= P[i];
            H *= 1099511628211ull; // FNV prime
        }
        return H;
    }

    uint64 HashNames(const TArray<FName>& Names)
    {
        uint64 H = 1469598103934665603ull;
        for (const FName& N : Names)
        {
            const FString S = N.ToString();
            H = Fnv1a64(*S, S.Len() * sizeof(TCHAR), H);
        }
        return H;
    }

    uint64 HashNamesAndParents(const TArray<FName>& Names, const TArray<int32>& Parents)
    {
        uint64 H = HashNames(Names);
        if (Parents.Num() > 0)
        {
            H = Fnv1a64(Parents.GetData(), Parents.Num() * sizeof(int32), H);
        }
        return H;
    }
}
