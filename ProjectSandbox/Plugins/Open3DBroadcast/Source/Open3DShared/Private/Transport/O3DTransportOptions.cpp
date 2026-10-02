// Copyright Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DTransportOptions.h"

#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "AddressInfoTypes.h"

namespace O3DTransportOptionsPrivate
{
	void SetError(FString* OutError, const FString& Message)
	{
		if (OutError)
		{
			*OutError = Message;
		}
	}

	bool IsHostNameChar(TCHAR C)
	{
		return FChar::IsAlnum(C) || C == TEXT('.') || C == TEXT('-') || C == TEXT('_');
	}

	/** Hex digits, ':' and '.' (an embedded IPv4), and a "%zone" suffix of letters, digits, '-', '_'. */
	bool IsValidIPv6Literal(const FString& Host)
	{
		if (Host.IsEmpty() || !Host.Contains(TEXT(":")))
		{
			return false;
		}
		int32 ZoneIndex = INDEX_NONE;
		Host.FindChar(TEXT('%'), ZoneIndex);
		const int32 AddressEnd = ZoneIndex == INDEX_NONE ? Host.Len() : ZoneIndex;
		for (int32 Index = 0; Index < Host.Len(); ++Index)
		{
			const TCHAR C = Host[Index];
			const bool bValid = Index < AddressEnd
				? (FChar::IsHexDigit(C) || C == TEXT(':') || C == TEXT('.'))
				: (Index == AddressEnd || FChar::IsAlnum(C) || C == TEXT('-') || C == TEXT('_'));
			if (!bValid)
			{
				return false;
			}
		}
		return ZoneIndex == INDEX_NONE || ZoneIndex + 1 < Host.Len();
	}

	bool IsValidHostName(const FString& Host)
	{
		if (Host == TEXT("*"))
		{
			return true;
		}
		if (Host.IsEmpty())
		{
			return false;
		}
		for (const TCHAR C : Host)
		{
			if (!IsHostNameChar(C))
			{
				return false;
			}
		}
		return true;
	}

	bool AllDigits(const FString& Text, int32 Start)
	{
		if (Start >= Text.Len())
		{
			return false;
		}
		for (int32 Index = Start; Index < Text.Len(); ++Index)
		{
			if (!FChar::IsDigit(Text[Index]))
			{
				return false;
			}
		}
		return true;
	}
}

FString FO3DHostPort::ToString() const
{
	return bIPv6 ? FString::Printf(TEXT("[%s]:%d"), *Host, Port) : FString::Printf(TEXT("%s:%d"), *Host, Port);
}

namespace O3DTransportOptions
{
	const FString* Find(const TMap<FString, FString>& Options, const FString& Key)
	{
		// TMap<FString, ...> hashes and compares FString keys case-insensitively.
		return Options.Find(Key);
	}

	FString GetString(const TMap<FString, FString>& Options, const FString& Key, const FString& Default)
	{
		const FString* Value = Find(Options, Key);
		if (!Value)
		{
			return Default;
		}
		FString Trimmed = Value->TrimStartAndEnd();
		return Trimmed.IsEmpty() ? Default : Trimmed;
	}

	bool TryParseInt(const FString& Text, int64& OutValue)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		int32 Index = 0;
		bool bNegative = false;
		if (Trimmed.Len() > 0 && (Trimmed[0] == TEXT('+') || Trimmed[0] == TEXT('-')))
		{
			bNegative = Trimmed[0] == TEXT('-');
			Index = 1;
		}
		if (!O3DTransportOptionsPrivate::AllDigits(Trimmed, Index))
		{
			return false;
		}

		// Accumulate as a negative number, whose range includes MIN_int64.
		int64 Value = 0;
		for (; Index < Trimmed.Len(); ++Index)
		{
			const int64 Digit = Trimmed[Index] - TEXT('0');
			if (Value < (MIN_int64 + Digit) / 10)
			{
				return false;
			}
			Value = Value * 10 - Digit;
		}
		if (!bNegative)
		{
			if (Value == MIN_int64)
			{
				return false;
			}
			Value = -Value;
		}
		OutValue = Value;
		return true;
	}

	int32 GetInt(const TMap<FString, FString>& Options, const FString& Key, int32 Default, int32 Min, int32 Max)
	{
		const FString* Value = Find(Options, Key);
		int64 Parsed = 0;
		if (!Value || !TryParseInt(*Value, Parsed))
		{
			return Default;
		}
		return static_cast<int32>(FMath::Clamp<int64>(Parsed, Min, Max));
	}

	bool TryParseDouble(const FString& Text, double& OutValue)
	{
		const FString Trimmed = Text.TrimStartAndEnd();
		// [+-] digits [. digits] [e [+-] digits], with at least one digit in the mantissa.
		int32 Index = 0;
		const int32 Len = Trimmed.Len();
		if (Index < Len && (Trimmed[Index] == TEXT('+') || Trimmed[Index] == TEXT('-')))
		{
			++Index;
		}
		int32 MantissaDigits = 0;
		while (Index < Len && FChar::IsDigit(Trimmed[Index]))
		{
			++Index;
			++MantissaDigits;
		}
		if (Index < Len && Trimmed[Index] == TEXT('.'))
		{
			++Index;
			while (Index < Len && FChar::IsDigit(Trimmed[Index]))
			{
				++Index;
				++MantissaDigits;
			}
		}
		if (MantissaDigits == 0)
		{
			return false;
		}
		if (Index < Len && (Trimmed[Index] == TEXT('e') || Trimmed[Index] == TEXT('E')))
		{
			++Index;
			if (Index < Len && (Trimmed[Index] == TEXT('+') || Trimmed[Index] == TEXT('-')))
			{
				++Index;
			}
			if (!O3DTransportOptionsPrivate::AllDigits(Trimmed, Index))
			{
				return false;
			}
			Index = Len;
		}
		if (Index != Len)
		{
			return false;
		}
		const double Value = FCString::Atod(*Trimmed);
		if (!FMath::IsFinite(Value))
		{
			return false;
		}
		OutValue = Value;
		return true;
	}

	double GetDouble(const TMap<FString, FString>& Options, const FString& Key, double Default, double Min, double Max)
	{
		const FString* Value = Find(Options, Key);
		double Parsed = 0.0;
		if (!Value || !TryParseDouble(*Value, Parsed))
		{
			return Default;
		}
		return FMath::Clamp(Parsed, Min, Max);
	}

	bool GetBool(const TMap<FString, FString>& Options, const FString& Key, bool Default)
	{
		const FString* Value = Find(Options, Key);
		if (!Value)
		{
			return Default;
		}
		const FString Trimmed = Value->TrimStartAndEnd();
		if (Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trimmed == TEXT("1")
			|| Trimmed.Equals(TEXT("yes"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("on"), ESearchCase::IgnoreCase))
		{
			return true;
		}
		if (Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Trimmed == TEXT("0")
			|| Trimmed.Equals(TEXT("no"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("off"), ESearchCase::IgnoreCase))
		{
			return false;
		}
		return Default;
	}

	bool TryParsePort(const FString& Text, int32& OutPort)
	{
		// Digits only (no sign, no spaces); at most 5 of them before leading zeros are ignored.
		if (!O3DTransportOptionsPrivate::AllDigits(Text, 0))
		{
			return false;
		}
		int32 Value = 0;
		for (const TCHAR C : Text)
		{
			Value = Value * 10 + (C - TEXT('0'));
			if (Value > 65535)
			{
				return false;
			}
		}
		if (Value < 1)
		{
			return false;
		}
		OutPort = Value;
		return true;
	}

	bool ParseHostPort(const FString& Input, FO3DHostPort& OutEndpoint, int32 DefaultPort, FString* OutError)
	{
		using namespace O3DTransportOptionsPrivate;

		FString Work = Input.TrimStartAndEnd();
		const int32 SchemeEnd = Work.Find(TEXT("://"), ESearchCase::CaseSensitive);
		if (SchemeEnd != INDEX_NONE)
		{
			Work.RightChopInline(SchemeEnd + 3);
		}
		// Path, query or fragment after the authority.
		int32 Cut = Work.Len();
		static const TCHAR Separators[] = { TEXT('/'), TEXT('?'), TEXT('#') };
		for (const TCHAR Separator : Separators)
		{
			int32 Found = INDEX_NONE;
			if (Work.FindChar(Separator, Found))
			{
				Cut = FMath::Min(Cut, Found);
			}
		}
		Work.LeftInline(Cut);

		if (Work.IsEmpty())
		{
			SetError(OutError, FString::Printf(TEXT("'%s' has no host."), *Input));
			return false;
		}
		if (Work.Contains(TEXT("@")))
		{
			SetError(OutError, FString::Printf(TEXT("'%s': user info (user@host) is not supported."), *Input));
			return false;
		}

		FO3DHostPort Result;
		FString PortText;
		bool bHasPort = false;

		if (Work[0] == TEXT('['))
		{
			int32 Close = INDEX_NONE;
			if (!Work.FindChar(TEXT(']'), Close))
			{
				SetError(OutError, FString::Printf(TEXT("'%s': '[' without a closing ']'."), *Input));
				return false;
			}
			Result.Host = Work.Mid(1, Close - 1);
			Result.bIPv6 = true;
			const FString Rest = Work.Mid(Close + 1);
			if (!Rest.IsEmpty())
			{
				if (Rest[0] != TEXT(':'))
				{
					SetError(OutError, FString::Printf(TEXT("'%s': unexpected characters after ']'."), *Input));
					return false;
				}
				PortText = Rest.Mid(1);
				bHasPort = true;
			}
			if (!IsValidIPv6Literal(Result.Host))
			{
				SetError(OutError, FString::Printf(TEXT("'%s': '%s' is not an IPv6 address."), *Input, *Result.Host));
				return false;
			}
		}
		else
		{
			int32 Colons = 0;
			for (const TCHAR C : Work)
			{
				Colons += C == TEXT(':') ? 1 : 0;
			}
			if (Colons > 1)
			{
				// A bare IPv6 literal; a port needs brackets ("[::1]:9000").
				Result.Host = Work;
				Result.bIPv6 = true;
				if (!IsValidIPv6Literal(Result.Host))
				{
					SetError(OutError, FString::Printf(TEXT("'%s' is not an IPv6 address (a port after an IPv6 address needs brackets)."), *Input));
					return false;
				}
			}
			else
			{
				int32 Colon = INDEX_NONE;
				if (Work.FindChar(TEXT(':'), Colon))
				{
					Result.Host = Work.Left(Colon);
					PortText = Work.Mid(Colon + 1);
					bHasPort = true;
				}
				else
				{
					Result.Host = Work;
				}
				if (!IsValidHostName(Result.Host))
				{
					SetError(OutError, FString::Printf(TEXT("'%s': '%s' is not a valid host."), *Input, *Result.Host));
					return false;
				}
			}
		}

		if (bHasPort)
		{
			if (!TryParsePort(PortText, Result.Port))
			{
				SetError(OutError, FString::Printf(TEXT("'%s': port '%s' is not a number from 1 to 65535."), *Input, *PortText));
				return false;
			}
		}
		else if (DefaultPort >= 1 && DefaultPort <= 65535)
		{
			Result.Port = DefaultPort;
		}
		else
		{
			SetError(OutError, FString::Printf(TEXT("'%s' has no port."), *Input));
			return false;
		}

		OutEndpoint = MoveTemp(Result);
		return true;
	}

	bool ResolveHostPort(const FO3DHostPort& Endpoint, TSharedPtr<FInternetAddr>& OutAddress, FString* OutError)
	{
		using namespace O3DTransportOptionsPrivate;

		if (Endpoint.Port < 1 || Endpoint.Port > 65535)
		{
			SetError(OutError, FString::Printf(TEXT("Port %d is not from 1 to 65535."), Endpoint.Port));
			return false;
		}
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			SetError(OutError, TEXT("No socket subsystem."));
			return false;
		}

		const FString Lookup = (Endpoint.Host.IsEmpty() || Endpoint.Host == TEXT("*")) ? FString(TEXT("0.0.0.0")) : Endpoint.Host;
		FAddressInfoResult Result = SocketSubsystem->GetAddressInfo(*Lookup, nullptr, EAddressInfoFlags::Default, NAME_None);
		if (Result.ReturnCode != SE_NO_ERROR || Result.Results.Num() == 0)
		{
			SetError(OutError, FString::Printf(TEXT("'%s' does not resolve (socket error %d)."), *Lookup, static_cast<int32>(Result.ReturnCode)));
			return false;
		}

		TSharedRef<FInternetAddr> Address = Result.Results[0].Address;
		Address->SetPort(Endpoint.Port);
		OutAddress = Address;
		return true;
	}
}
