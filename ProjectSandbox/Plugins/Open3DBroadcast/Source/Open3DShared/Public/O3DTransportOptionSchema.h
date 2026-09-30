// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

/**
 * Declared transport options (ADR 0007 item 8, ADR 0010 §4).
 *
 * A transport describes the options it reads from its namespaced TMap<FString, FString> (the
 * sender component's TransportOptions or the receiver's FO3DReceiverSourceConfig::TransportOptions)
 * as plain data. The editor module (Open3DBroadcastEditor) builds one generic, undoable panel from
 * it, so runtime modules carry no Slate code. Nothing here depends on the editor or on Slate, and
 * no member changes with WITH_EDITOR.
 *
 * This is the subset of ADR 0007's schema that the WP-F7 panels need. WP-A1 moves it into the
 * transport descriptor and adds the typed accessors, Float, bRestartOnChange and Validate.
 */

/** The value type of one option, which selects its editor widget. */
enum class EO3DTransportOptionType : uint8
{
	/** Free text. */
	String,
	/** Free text holding a URL. */
	Url,
	/** Whole number stored as decimal text. See FO3DTransportOptionField::StoredUnitScale. */
	Int,
	/** "true" or "false". */
	Bool,
	/** One of FO3DTransportOptionField::EnumValues. */
	Enum,
	/**
	 * A credential (ADR 0004). The key must also be in the customization's SecretOptionKeys. The
	 * editor shows a password box that opens empty and writes to FO3DSecretStore, never to the
	 * option map.
	 */
	Secret,
};

/** One choice of an Enum option. */
struct FO3DTransportOptionEnumValue
{
	/** The text stored in the option map. */
	FString Value;
	FText DisplayName;
};

/** One declared option. */
struct FO3DTransportOptionField
{
	/** Key in the option map, e.g. "port" or "webrtc.url". */
	FString Key;
	FText DisplayName;
	FText Tooltip;
	EO3DTransportOptionType Type = EO3DTransportOptionType::String;

	/**
	 * The value the transport uses when the key is unset, as stored text. The editor shows it as a
	 * hint and never writes it: opening a panel must not change the object (TRB-45).
	 */
	FString Default;

	/**
	 * Optional text for an empty box when the transport has no single default (for example "auto:
	 * from Subject Name if blank"). When empty, the editor shows Default instead.
	 */
	FText Hint;

	/** Int only: inclusive range of the value the editor shows (after StoredUnitScale). */
	int32 Min = 0;
	int32 Max = 0;

	/**
	 * Int only: stored value = shown value * StoredUnitScale. For example 1048576 shows a byte
	 * count in MiB. 1 stores the shown value.
	 */
	int64 StoredUnitScale = 1;

	/** Enum only: the choices, in display order. */
	TArray<FO3DTransportOptionEnumValue> EnumValues;

	/**
	 * Optional. The row is shown only while this returns true for the current option map. Called
	 * on the game thread whenever the panel repaints, so it must be cheap and must not block.
	 */
	TFunction<bool(const TMap<FString, FString>& /*Options*/)> VisibleWhen;
};

/** The options of one transport role, in display order. */
using FO3DTransportOptionSchema = TArray<FO3DTransportOptionField>;

namespace O3DTransportOptions
{
	/** Reads Key from Options; an unset key yields Fallback. */
	inline FString GetOption(const TMap<FString, FString>& Options, const FString& Key, const FString& Fallback = FString())
	{
		const FString* Value = Options.Find(Key);
		return (Value && !Value->IsEmpty()) ? *Value : Fallback;
	}

	/** A VisibleWhen that is true while the Bool option Key equals bExpected (unset reads as bDefault). */
	inline TFunction<bool(const TMap<FString, FString>&)> VisibleWhenBool(const FString& Key, bool bExpected, bool bDefault = false)
	{
		return [Key, bExpected, bDefault](const TMap<FString, FString>& Options)
		{
			const FString* Value = Options.Find(Key);
			const bool bValue = (Value && !Value->IsEmpty()) ? Value->ToBool() : bDefault;
			return bValue == bExpected;
		};
	}

	/** A VisibleWhen that is true while the option Key equals Expected, ignoring case (unset reads as Default). */
	inline TFunction<bool(const TMap<FString, FString>&)> VisibleWhenEquals(const FString& Key, const FString& Expected, const FString& Default = FString())
	{
		return [Key, Expected, Default](const TMap<FString, FString>& Options)
		{
			return GetOption(Options, Key, Default).Equals(Expected, ESearchCase::IgnoreCase);
		};
	}
}
