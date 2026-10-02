// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DTransportOptionSchema.h"

/**
 * Read access to one transport role's options (ADR 0007 items 4 and 8, WP-A1 PR 5a; SHR-36).
 *
 * The values are the role's namespaced option map with the secret keys left out (the sender
 * component's or the receiver source's TransportOptions; secrets reach a transport only through
 * FO3DTransportConfig::Secrets, ADR 0004). The schema is the one the transport declared for the
 * role (FO3DTransportRoleOptions::OptionSchema), or none.
 *
 * The configure functions receive one (FO3DTransportConfigureFunction), and
 * FO3DTransportConfig::GetOptions() returns one over the config's own copy of the values.
 *
 * A view does not own anything: the map and the schema it was made from must outlive it. It
 * converts implicitly from a plain map, so the O3DTransportOptions getters, which take a view,
 * also take FO3DTransportConfig::AdvancedParams or any other TMap<FString, FString>.
 *
 * Keys are matched case-insensitively (TMap<FString, ...> hashes them that way). Values are
 * trimmed before they are parsed.
 *
 * Threading: a view is a pair of const pointers; reading it is safe on any thread for as long as
 * nobody changes the map or the schema it points to.
 */
class OPEN3DSHARED_API FO3DTransportOptionsView
{
public:
	/** A view of no options and no schema. */
	FO3DTransportOptionsView() = default;

	/** A view of Values without a schema. Implicit, so a map can be passed wherever a view is taken. */
	FO3DTransportOptionsView(const TMap<FString, FString>& InValues)
		: Values(&InValues)
	{
	}

	/** A view of Values whose schema supplies the defaults, types and visibility. */
	FO3DTransportOptionsView(const TMap<FString, FString>& InValues, const FO3DTransportOptionSchema* InSchema)
		: Values(&InValues)
		, Schema(InSchema)
	{
	}

	/** The stored values (an empty map for a default-constructed view). */
	const TMap<FString, FString>& GetValues() const;

	/** The schema, or null. */
	const FO3DTransportOptionSchema* GetSchema() const { return Schema; }

	/** The stored value of Key, untrimmed, or null when it is not set. */
	const FString* Find(const FString& Key) const;

	/** True when Key is set to something other than whitespace. */
	bool IsSet(const FString& Key) const;

	/** The schema's entry for Key (case-insensitive), or null. */
	const FO3DTransportOptionField* FindField(const FString& Key) const;

	/** The schema's Default text for Key, or an empty string. */
	FString GetDefaultText(const FString& Key) const;

	/**
	 * The schema-typed getters. A value that is set and parses wins; otherwise the schema's
	 * Default (parsed the same way); otherwise "", 0, 0.0 or false. Parsing is the strict
	 * O3DTransportOptions one (TryParseInt, TryParseDouble, GetBool's words).
	 *
	 * GetDouble of a Float field with a range (Max > Min) clamps the result, the value or the
	 * default, to [Min, Max] (WP-A1 PR 5c). GetInt does not clamp: an Int range is in the editor's
	 * shown unit (StoredUnitScale), and transports clamp their stored ints themselves.
	 */
	FString GetString(const FString& Key) const;
	int32 GetInt(const FString& Key) const;
	double GetDouble(const FString& Key) const;
	bool GetBool(const FString& Key) const;

	/**
	 * True when the schema's row for Key is shown for the current values: its VisibleWhen returns
	 * true, or it has none. A key the schema does not declare is visible. Game thread, like the
	 * VisibleWhen functions themselves (O3DTransportOptionSchema.h).
	 */
	bool IsVisible(const FString& Key) const;

private:
	const TMap<FString, FString>* Values = nullptr;
	const FO3DTransportOptionSchema* Schema = nullptr;
};
