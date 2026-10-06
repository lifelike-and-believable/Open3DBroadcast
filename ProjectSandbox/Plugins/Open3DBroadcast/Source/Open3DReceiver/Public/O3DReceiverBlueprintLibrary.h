// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ILiveLinkSource.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "O3DReceiverBlueprintLibrary.generated.h"

/**
 * Runtime control of Open3DStream LiveLink receiver sources from Blueprint and C++ (WP-U2, UX-3),
 * in a game as well as in the editor. A created source is an ordinary LiveLink source: remove it,
 * read its status or check it with LiveLink's own nodes (Remove Source, Get Source Status, Is
 * Source Still Valid) on the returned handle, and it saves into a LiveLink preset like one
 * created in the LiveLink panel.
 */
UCLASS()
class OPEN3DRECEIVER_API UO3DReceiverBlueprintLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * Creates an Open3DStream receiver source and adds it to LiveLink. Game thread.
	 *
	 * @param TransportName  A registered receiver transport, for example "udp", "nng" or "webrtc".
	 * @param Options        Transport options, with the keys of the user guide's Transport Options
	 *                       Reference. Options left out take the project defaults (Project Settings >
	 *                       Plugins > Open3DBroadcast). Set credentials with Set Transport Secret, not
	 *                       here: a credential found here is moved to the credential store for this
	 *                       session, with a warning.
	 * @param ContextName    The runtime context the source publishes audio, control and metrics to
	 *                       (empty: the default context).
	 * @param bEnableAudio   Play the audio the transport receives.
	 * @param SourceHandle   The new source, for LiveLink's source nodes; empty on failure.
	 * @return False when the transport has no receiver registered or LiveLink is not available.
	 */
	UFUNCTION(BlueprintCallable, Category = "Open3DBroadcast|Receiver", meta = (AutoCreateRefTerm = "Options", DisplayName = "Create Open3DStream LiveLink Source"))
	static bool CreateLiveLinkSource(FName TransportName, const TMap<FString, FString>& Options, FName ContextName, bool bEnableAudio, FLiveLinkSourceHandle& SourceHandle);
};
