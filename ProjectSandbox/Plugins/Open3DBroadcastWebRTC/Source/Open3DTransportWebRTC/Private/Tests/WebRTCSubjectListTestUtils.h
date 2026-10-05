// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

// Test-only. IOpen3DSender::Send(const O3DS::SubjectList&) was deleted in WP-A1 PR 5b (ADR 0007
// step 5); the add-on's tests that sent a SubjectList use this instead. It does what the WebRTC
// sender's Send did: serialize the list once and send the bytes with SendSerialized under the
// first subject's name. The add-on's tests cannot use Open3DBroadcastTests (an editor module the
// Fab package does not ship), which has the same helper (O3DTests::SendSubjectList).

#if WITH_DEV_AUTOMATION_TESTS

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"
#include "Transport/O3DSenderInterface.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/model.h"
THIRD_PARTY_INCLUDES_END

#include <vector>

namespace WebRTCSubjectListTest
{
	/** True when the sender queued the serialized list (EO3DSendResult::Queued). */
	inline bool SendSubjectList(IOpen3DSender& Sender, const O3DS::SubjectList& List)
	{
		std::vector<char> Buffer;
		const double TimestampSeconds = FPlatformTime::Seconds();
		// SubjectList::Serialize is not const (the deleted Send used the same cast).
		const int32 BytesWritten = const_cast<O3DS::SubjectList&>(List).Serialize(Buffer, TimestampSeconds);

		FString Subject;
		if (!List.mItems.empty() && List.mItems[0])
		{
			Subject = UTF8_TO_TCHAR(List.mItems[0]->mName.c_str());
		}

		TArray<uint8> Bytes;
		if (BytesWritten > 0)
		{
			Bytes.Append(reinterpret_cast<const uint8*>(Buffer.data()), BytesWritten);
		}
		return Sender.SendSerialized(FO3DSendPayload(MoveTemp(Bytes), MoveTemp(Subject), TimestampSeconds)) == EO3DSendResult::Queued;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
