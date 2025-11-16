#include "SocketsTcpAudio.h"

#include "O3DAudioSerialization.h"

namespace O3DSockets::Tcp
{
	bool SerializeAudioFramePayload(const O3DS::FAudioFrameMeta& Meta, const uint8* PCM16Data, int32 NumBytes, TArray<uint8>& OutPayload)
	{
		return O3DAudio::SerializePcm16Frame(Meta, PCM16Data, NumBytes, OutPayload);
	}

	bool DeserializeAudioFramePayload(const uint8* Payload, int32 PayloadSize, FAudioFrame& OutFrame)
	{
		O3DAudio::FPcm16Frame Frame;
		if (!O3DAudio::DeserializePcm16Frame(Payload, PayloadSize, Frame))
		{
			return false;
		}

		OutFrame.Meta = MoveTemp(Frame.Meta);
		OutFrame.PCM16 = MoveTemp(Frame.PCM16);
		return true;
	}
}
