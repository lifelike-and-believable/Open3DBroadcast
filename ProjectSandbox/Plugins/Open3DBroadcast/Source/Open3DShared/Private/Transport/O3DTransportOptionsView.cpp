// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "Transport/O3DTransportOptionsView.h"

#include "Transport/O3DTransportOptions.h"

namespace O3DTransportOptionsViewPrivate
{
	const TMap<FString, FString>& EmptyOptionMap()
	{
		static const TMap<FString, FString> Empty;
		return Empty;
	}
}

const TMap<FString, FString>& FO3DTransportOptionsView::GetValues() const
{
	return Values ? *Values : O3DTransportOptionsViewPrivate::EmptyOptionMap();
}

const FString* FO3DTransportOptionsView::Find(const FString& Key) const
{
	// TMap<FString, ...> hashes and compares FString keys case-insensitively.
	return Values ? Values->Find(Key) : nullptr;
}

bool FO3DTransportOptionsView::IsSet(const FString& Key) const
{
	const FString* Value = Find(Key);
	return Value && !Value->TrimStartAndEnd().IsEmpty();
}

const FO3DTransportOptionField* FO3DTransportOptionsView::FindField(const FString& Key) const
{
	if (!Schema)
	{
		return nullptr;
	}
	return Schema->FindByPredicate([&Key](const FO3DTransportOptionField& Field)
	{
		return Field.Key.Equals(Key, ESearchCase::IgnoreCase);
	});
}

FString FO3DTransportOptionsView::GetDefaultText(const FString& Key) const
{
	const FO3DTransportOptionField* Field = FindField(Key);
	return Field ? Field->Default.TrimStartAndEnd() : FString();
}

FString FO3DTransportOptionsView::GetString(const FString& Key) const
{
	return O3DTransportOptions::GetString(*this, Key, GetDefaultText(Key));
}

int32 FO3DTransportOptionsView::GetInt(const FString& Key) const
{
	int64 Default = 0;
	const FString DefaultText = GetDefaultText(Key);
	if (!O3DTransportOptions::TryParseInt(DefaultText, Default) || Default < MIN_int32 || Default > MAX_int32)
	{
		Default = 0;
	}
	return O3DTransportOptions::GetInt(*this, Key, static_cast<int32>(Default));
}

double FO3DTransportOptionsView::GetDouble(const FString& Key) const
{
	double Default = 0.0;
	if (!O3DTransportOptions::TryParseDouble(GetDefaultText(Key), Default))
	{
		Default = 0.0;
	}

	// A Float field's range applies to the value and to the default alike (WP-A1 PR 5c).
	const FO3DTransportOptionField* Field = FindField(Key);
	if (Field && Field->Type == EO3DTransportOptionType::Float && Field->Max > Field->Min)
	{
		return O3DTransportOptions::GetDouble(*this, Key, FMath::Clamp(Default, Field->Min, Field->Max), Field->Min, Field->Max);
	}
	return O3DTransportOptions::GetDouble(*this, Key, Default);
}

bool FO3DTransportOptionsView::GetBool(const FString& Key) const
{
	bool Default = false;
	if (!O3DTransportOptions::TryParseBool(GetDefaultText(Key), Default))
	{
		Default = false;
	}
	return O3DTransportOptions::GetBool(*this, Key, Default);
}

bool FO3DTransportOptionsView::IsVisible(const FString& Key) const
{
	const FO3DTransportOptionField* Field = FindField(Key);
	if (!Field || !Field->VisibleWhen)
	{
		return true;
	}
	return Field->VisibleWhen(GetValues());
}
