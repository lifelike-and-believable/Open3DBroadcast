// Copyright (c) Open3DStream Contributors

#include "O3DRedact.h"

namespace O3DRedact
{
	namespace
	{
		const TCHAR* const SensitiveFragments[] =
		{
			TEXT("token"),
			TEXT("secret"),
			TEXT("password"),
			TEXT("passwd"),
			TEXT("auth"),
			TEXT("credential"),
			TEXT("jwt"),
			TEXT("apikey"),
			TEXT("api_key"),
		};

		/** Rewrites "a=1&b&c=2" as "a=<redacted>&<redacted>&c=<redacted>". */
		FString RedactQuery(const FString& Query)
		{
			if (Query.IsEmpty())
			{
				return Query;
			}

			TArray<FString> Parts;
			Query.ParseIntoArray(Parts, TEXT("&"), /*InCullEmpty=*/false);
			for (FString& Part : Parts)
			{
				if (Part.IsEmpty())
				{
					continue;
				}

				int32 EqualsIdx = INDEX_NONE;
				if (Part.FindChar(TEXT('='), EqualsIdx))
				{
					Part = Part.Left(EqualsIdx + 1) + Marker();
				}
				else
				{
					// A bare query component may itself be the credential.
					Part = Marker();
				}
			}
			return FString::Join(Parts, TEXT("&"));
		}
	}

	bool IsSensitiveKey(const FString& Key)
	{
		if (Key.IsEmpty())
		{
			return false;
		}

		const FString Lower = Key.ToLower();
		for (const TCHAR* Fragment : SensitiveFragments)
		{
			if (Lower.Contains(Fragment, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}

		return Lower.EndsWith(TEXT(".key"), ESearchCase::CaseSensitive)
			|| Lower.EndsWith(TEXT("_key"), ESearchCase::CaseSensitive);
	}

	FString Value(const FString& Key, const FString& InValue)
	{
		return IsSensitiveKey(Key) ? FString(Marker()) : InValue;
	}

	FString Url(const FString& InUrl)
	{
		if (InUrl.IsEmpty())
		{
			return InUrl;
		}

		FString Work = InUrl;

		FString FragmentSuffix;
		int32 HashIdx = INDEX_NONE;
		if (Work.FindChar(TEXT('#'), HashIdx))
		{
			FragmentSuffix = FString(TEXT("#")) + Marker();
			Work.LeftInline(HashIdx);
		}

		FString QuerySuffix;
		int32 QueryIdx = INDEX_NONE;
		if (Work.FindChar(TEXT('?'), QueryIdx))
		{
			QuerySuffix = FString(TEXT("?")) + RedactQuery(Work.Mid(QueryIdx + 1));
			Work.LeftInline(QueryIdx);
		}

		// The authority starts after "scheme://" (or at the start when there is no scheme) and
		// ends at the first '/'. User-info is everything up to the last '@' in it.
		const int32 SchemeSepIdx = Work.Find(TEXT("://"), ESearchCase::CaseSensitive);
		const int32 AuthorityStart = (SchemeSepIdx == INDEX_NONE) ? 0 : SchemeSepIdx + 3;
		int32 AuthorityEnd = Work.Len();
		for (int32 Index = AuthorityStart; Index < Work.Len(); ++Index)
		{
			if (Work[Index] == TEXT('/'))
			{
				AuthorityEnd = Index;
				break;
			}
		}

		FString Authority = Work.Mid(AuthorityStart, AuthorityEnd - AuthorityStart);
		int32 AtIdx = INDEX_NONE;
		if (Authority.FindLastChar(TEXT('@'), AtIdx))
		{
			Authority = FString(Marker()) + TEXT("@") + Authority.Mid(AtIdx + 1);
		}

		return Work.Left(AuthorityStart) + Authority + Work.Mid(AuthorityEnd) + QuerySuffix + FragmentSuffix;
	}
}
