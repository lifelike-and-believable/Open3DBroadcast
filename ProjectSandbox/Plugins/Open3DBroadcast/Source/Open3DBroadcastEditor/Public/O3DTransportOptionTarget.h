// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DSecretStore.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UObject;
class UO3DSenderComponent;
class UO3DReceiverSettingsObject;

/**
 * The object a transport options panel edits (ADR 0010 §5, TRB-45).
 *
 * Implementations hold a TWeakObjectPtr and check it on every call, so a panel that outlives its
 * object (garbage collection, the end of PIE, a Details panel rebuilt for another selection) does
 * nothing instead of touching freed memory. Game thread only.
 */
class OPEN3DBROADCASTEDITOR_API IO3DOptionTarget
{
public:
	virtual ~IO3DOptionTarget() = default;

	/** False once the object is gone. */
	virtual bool IsValid() const = 0;

	/** Transport whose options and secrets this target edits; NAME_None when the object is gone. */
	virtual FName GetTransportName() const = 0;

	/** Copy of the option map; empty when the object is gone. */
	virtual TMap<FString, FString> GetOptions() const = 0;

	/** True when the target's transport declares Key secret (ADR 0004). */
	virtual bool IsSecretKey(const FString& Key) const = 0;

	/** Where the secret Key resolves from, without the value. Not set when the object is gone. */
	virtual FO3DSecretStatus GetSecretStatus(const FString& Key) const = 0;

	/** Stores a secret for the target's transport and credential profile. Never enters the option map or the undo buffer. */
	virtual void SetSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence) = 0;

	/** Moves the session secret to or from the per-user store. Returns false when there was nothing to move. */
	virtual bool SetSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence) = 0;

	/** Clears the session secret and any remembered copy. */
	virtual void ClearSecret(const FString& Key) = 0;

	/**
	 * Restarts the object's running transport so it picks up an option whose schema entry has
	 * bRestartOnChange (WP-A1 PR 5c). The panel calls it after such a commit changed the object.
	 * Returns true when a transport was restarted. The default does nothing (nothing is running:
	 * the receiver's settings object is only used to create a source).
	 */
	virtual bool RestartTransport() { return false; }

	/** The stored value of Key; empty when unset or the object is gone. */
	FString GetOption(const FString& Key) const;

	/**
	 * Commits one option as one undoable edit: opens an FScopedTransaction, calls Modify() on the
	 * object and writes Key. An empty Value removes the key. Does nothing, and opens no
	 * transaction, when the object is gone, Key is empty or secret, or the stored value already
	 * equals Value (case-sensitive). Returns true when the object changed.
	 */
	bool CommitOption(const FString& Key, const FString& Value);

protected:
	/** The object to Modify(); null when it is gone. */
	virtual UObject* GetObject() const = 0;

	/** Writes Key (removes it when Value is empty). Called inside the transaction, after Modify(). */
	virtual void WriteOption(const FString& Key, const FString& Value) = 0;
};

/** Edits UO3DSenderComponent::TransportOptions through the component's own setters. */
class OPEN3DBROADCASTEDITOR_API FO3DSenderOptionTarget final : public IO3DOptionTarget
{
public:
	explicit FO3DSenderOptionTarget(UO3DSenderComponent* InComponent);

	virtual bool IsValid() const override;
	virtual FName GetTransportName() const override;
	virtual TMap<FString, FString> GetOptions() const override;
	virtual bool IsSecretKey(const FString& Key) const override;
	virtual FO3DSecretStatus GetSecretStatus(const FString& Key) const override;
	virtual void SetSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence) override;
	virtual bool SetSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence) override;
	virtual void ClearSecret(const FString& Key) override;
	/** Stops and starts capture, as a restart property does, while the component captures in a game world. */
	virtual bool RestartTransport() override;

protected:
	virtual UObject* GetObject() const override;
	virtual void WriteOption(const FString& Key, const FString& Value) override;

private:
	TWeakObjectPtr<UO3DSenderComponent> WeakComponent;
};

/**
 * Edits FO3DReceiverSourceConfig::TransportOptions of a UO3DReceiverSettingsObject. Secrets go to
 * FO3DSecretStore under the settings' transport and credential profile.
 */
class OPEN3DBROADCASTEDITOR_API FO3DReceiverOptionTarget final : public IO3DOptionTarget
{
public:
	explicit FO3DReceiverOptionTarget(UO3DReceiverSettingsObject* InSettingsObject);

	virtual bool IsValid() const override;
	virtual FName GetTransportName() const override;
	virtual TMap<FString, FString> GetOptions() const override;
	virtual bool IsSecretKey(const FString& Key) const override;
	virtual FO3DSecretStatus GetSecretStatus(const FString& Key) const override;
	virtual void SetSecret(const FString& Key, const FString& Value, EO3DSecretPersistence Persistence) override;
	virtual bool SetSecretPersistence(const FString& Key, EO3DSecretPersistence Persistence) override;
	virtual void ClearSecret(const FString& Key) override;

protected:
	virtual UObject* GetObject() const override;
	virtual void WriteOption(const FString& Key, const FString& Value) override;

private:
	/** Transport and credential profile the secrets are keyed by; false when the object is gone. */
	bool GetSecretScope(FString& OutTransport, FString& OutProfile) const;

	TWeakObjectPtr<UO3DReceiverSettingsObject> WeakSettingsObject;
};
