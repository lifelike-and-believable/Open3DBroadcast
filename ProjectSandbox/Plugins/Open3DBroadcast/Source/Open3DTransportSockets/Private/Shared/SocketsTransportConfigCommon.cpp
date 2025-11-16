#include "SocketsTransportConfig.h"

namespace O3DSocketsConfig
{
	int32 ParsePositiveInt(const FString& Value, int32 DefaultValue)
	{
		if (Value.IsEmpty())
		{
			return DefaultValue;
		}

		const int32 Parsed = FCString::Atoi(*Value);
		return Parsed > 0 ? Parsed : DefaultValue;
	}

	bool ParseBoolOption(const FString& Value, bool DefaultValue)
	{
		if (Value.IsEmpty())
		{
			return DefaultValue;
		}

		if (Value.Equals(TEXT("1"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("true"), ESearchCase::IgnoreCase))
		{
			return true;
		}

		if (Value.Equals(TEXT("0"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("false"), ESearchCase::IgnoreCase))
		{
			return false;
		}

		return DefaultValue;
	}
}

