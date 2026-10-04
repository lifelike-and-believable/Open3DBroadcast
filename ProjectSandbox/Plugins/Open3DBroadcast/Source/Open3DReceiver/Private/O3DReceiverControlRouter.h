// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DControlBus.h"
#include "Templates/Function.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

#include <string>
#include <vector>

/**
 * The receiver side of the control channel for one receiver source (docs/adr/0011-control-channel.md
 * item 8; WP-A3 split it out of FO3DReceiverSource): parses control payloads into changes, holds
 * them for alignment with the mocap pose LiveLink presents when the project setting asks for it,
 * and publishes them to its control bus. Game thread.
 */
class FO3DReceiverControlRouter
{
public:
	/** Publishes to InBus: the receiver source's context's bus (ADR 0012 item 6). It must outlive the router. */
	explicit FO3DReceiverControlRouter(FO3DControlBus::FInstance& InBus);

	/** Publishes to the default context's bus. */
	FO3DReceiverControlRouter();

	/**
	 * The sender time (sender clock, microseconds) of the pose LiveLink presents for the mocap stream
	 * carrying these subjects, or false when there is none (changes are then released at once).
	 */
	using FPresentedSenderTime = TFunctionRef<bool(const std::vector<std::string>& MocapSubjects, uint64_t& OutUs)>;

	/** Applies UO3DControlSettings to the core receiver (limits, allowlist) and the aligner. */
	void ApplyConfig();

	/** One control payload from the transport. Dropped (and counted) while control is disabled. */
	void HandlePayload(bool bEnabled, const uint8* Data, int32 NumBytes, double NowSeconds, const FString& StreamId);

	/**
	 * Prunes, releases aligned changes and publishes them. When control was turned off, forgets
	 * every control source silently instead (no Cleared delegates, ADR 0011 item 8).
	 */
	void Tick(bool bEnabled, double NowSeconds, const FString& StreamId, FPresentedSenderTime PresentedSenderTime);

	/** Publishes what alignment still holds (the transport stopped): the changes are real, only their timing is lost. */
	void FlushHeld(const FString& StreamId);

	uint64 GetPayloadsDroppedDisabled() const { return PayloadsDroppedDisabled; }
	const O3DS::Control::AlignerStats& GetAlignerStats() const { return Aligner.GetStats(); }
	size_t GetNumHeld() const { return Aligner.NumHeld(); }

private:
	void Route(O3DS::Control::Change&& Change, double NowSeconds, const FString& StreamId);
	void Publish(const O3DS::Control::Change& Change, const FString& StreamId) const;
	/** Forgets every control source this receiver has seen, without broadcasting. */
	void Discard();

	FO3DControlBus::FInstance& Bus;
	O3DS::Control::ControlReceiver Receiver;
	O3DS::Control::ControlAligner Aligner;
	/** Source ids this receiver has published for, so they can be forgotten on the bus. */
	TSet<FString> SourcesSeen;
	bool bWasEnabled = false;
	uint64 PayloadsDroppedDisabled = 0;
};
