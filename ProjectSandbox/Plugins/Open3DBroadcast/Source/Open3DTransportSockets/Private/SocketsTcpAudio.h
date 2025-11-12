#pragma once

#include "CoreMinimal.h"
#include "O3DUnifiedMessage.h"

namespace O3DSockets::Tcp
{
	/** Encapsulated audio frame parsed from TCP transport payloads. */
	struct FAudioFrame
	{
		O3DS::FAudioFrameMeta Meta;
		TArray<uint8> PCM16;
	};

	/** Serialize audio metadata and PCM payload into a wire buffer (little-endian fields). */
	bool SerializeAudioFramePayload(const O3DS::FAudioFrameMeta& Meta, const uint8* PCM16Data, int32 NumBytes, TArray<uint8>& OutPayload);

	/** Parse audio metadata and PCM payload from a wire buffer. */
	bool DeserializeAudioFramePayload(const uint8* Payload, int32 PayloadSize, FAudioFrame& OutFrame);
}
