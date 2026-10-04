// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"

/**
 * Records every payload the senders in this process serialize to a .o3dscap file (the core
 * capture format, o3ds/capture.h), verbatim: the evaluation input for apps/QuantEval (CORE-12)
 * and replay. Process-wide, like the console commands that drive it:
 *
 *   o3d.Sender.Capture.Start [file]   relative paths go under Saved/O3DCaptures; default name is a timestamp
 *   o3d.Sender.Capture.Stop
 *
 * Record a take with the legacy encoding (the default: residual and quantization off), so every
 * payload is a full frame and the capture holds the exact pose. Record is called on the sender
 * pipeline's worker; it costs one atomic load while no capture is active.
 */
class OPEN3DSENDER_API FO3DSenderCapture
{
public:
	/** Starts writing to Path (made absolute under Saved/O3DCaptures when relative). Stops a capture already running. */
	static bool Start(const FString& Path);
	/** Stops and closes the file. Returns the number of payloads written. */
	static int64 Stop();
	static bool IsActive();
	/** The file being written, or the last one written. */
	static FString GetPath();

	/** Called by the sender pipeline with each serialized payload. */
	static void Record(TConstArrayView<uint8> Payload);
};
