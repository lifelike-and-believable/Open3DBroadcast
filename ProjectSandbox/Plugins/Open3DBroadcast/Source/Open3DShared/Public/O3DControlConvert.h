// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "O3DControlBus.h"
#include "O3DControlTypes.h"

#include <string>

namespace O3DS
{
	namespace Control
	{
		struct Value;
		struct Change;
	}
}

/**
 * Conversions between the engine control types (O3DControlTypes.h, O3DControlBus.h) and the
 * core ones (src/o3ds/control.h, compiled as Open3DStreamCore). Declared here without the core
 * headers so Open3DShared's public interface stays free of them; callers that hold core values
 * (the sender component, the receiver source) already include the core.
 */
namespace O3DControl
{
	/** UTF-8 encode, exactly (no NUL termination, no substitution of valid characters). */
	OPEN3DSHARED_API std::string ToUtf8(const FString& In);

	/** UTF-8 decode. The core only produces well-formed UTF-8 without NUL (O3DS::Control::IsValidUtf8). */
	OPEN3DSHARED_API FString FromUtf8(const std::string& In);

	/** Engine value to core value. Validation (lengths, finite numbers) is the core's job. */
	OPEN3DSHARED_API void ToCore(const FO3DControlValue& In, O3DS::Control::Value& Out);

	/** Core value to engine value. */
	OPEN3DSHARED_API FO3DControlValue FromCore(const O3DS::Control::Value& In);

	/** A core receiver change as the bus publishes it. StreamId is the receiving transport's stream. */
	OPEN3DSHARED_API FO3DControlChange FromCore(const O3DS::Control::Change& In, const FString& StreamId);
}
