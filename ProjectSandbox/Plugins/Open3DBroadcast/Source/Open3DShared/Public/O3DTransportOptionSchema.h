// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
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
 * It lives in the transport descriptor (FO3DTransportRoleOptions::OptionSchema, WP-A1 PR 1), and
 * FO3DTransportOptionsView (WP-A1 PR 5a) reads options with its defaults, ranges and VisibleWhen.
 * WP-A1 PR 5c completed ADR 0007 item 8: the Float type, Secret entries that carry their
 * environment variable (SecretEnvVar), bRestartOnChange and Validate.
 */

/** The value type of one option, which selects its editor widget. */
enum class EO3DTransportOptionType : uint8
{
	/** Free text. Also the type of a key a transport reads but does not describe further. */
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
	 * A credential (ADR 0004). The entry declares the key secret: FO3DTransportRegistry::
	 * GetSecretDeclaration lists it, with the entry's SecretEnvVar, whether or not the key is also
	 * in the role's SecretOptionKeys (WP-A1 PR 5c). The editor shows a password box that opens
	 * empty and writes to FO3DSecretStore, never to the option map.
	 */
	Secret,
	/**
	 * A number stored as decimal text ("0.5", "30"), read with O3DTransportOptions::TryParseDouble.
	 * Min and Max bound it when Max > Min (WP-A1 PR 5c). Appended last, so the values above keep
	 * their numbers.
	 */
	Float,
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

	/**
	 * Int and Float: inclusive range, used when Max > Min. For an Int it is the value the editor
	 * shows (after StoredUnitScale), and the editor uses the whole numbers inside it. A double
	 * since WP-A1 PR 5c, so a Float can have a fractional range; every int32 is exact in a double.
	 * FO3DTransportOptionsView::GetDouble clamps a Float to it; the editor clamps what it writes.
	 */
	double Min = 0.0;
	double Max = 0.0;

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

	/**
	 * Secret only: environment variable that supplies the secret, e.g. "O3DB_<TRANSPORT>_TOKEN"
	 * (ADR 0004 item 3; a non-default credential profile first tries "<NAME>__<PROFILE>"). Empty
	 * for none. This replaces the role's SecretEnvVars entry for the key (WP-A1 PR 5c); when both
	 * name one, this one wins.
	 */
	FString SecretEnvVar;

	/**
	 * True when a running transport only picks up a change of this option when it restarts. The
	 * editor panel then restarts the running transport after it commits a change (the sender
	 * component while it captures in a game world; IO3DOptionTarget::RestartTransport) and says so
	 * in the tooltip. False keeps today's behaviour: the value is used at the next start.
	 */
	bool bRestartOnChange = false;

	/**
	 * Optional check of a set value, beyond its type and range. Called with the trimmed stored
	 * text (never empty: an unset key uses Default, which is not checked). Returns false, with
	 * OutError saying what is wrong in a sentence a user can act on, to refuse it: the editor
	 * panel then shows OutError under the row and does not write, and starting the transport
	 * fails with EO3DTransportError::InvalidConfig (O3DTransportOptions::ValidateOptions). Called
	 * on the game thread; must be cheap, must not block (no DNS) and must not read other options.
	 */
	TFunction<bool(const FString& /*Value*/, FText& /*OutError*/)> Validate;
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
