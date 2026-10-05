// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DControlConvert.h"

#include "Containers/StringConv.h"
#include "O3DUnifiedMessage.h"

THIRD_PARTY_INCLUDES_START
#include "o3ds/control.h"
THIRD_PARTY_INCLUDES_END

#include <limits>

// Open3DShared's public headers cannot see the core's limits; keep the two in step here.
static_assert(O3DS::UnifiedMaxControlPayloadSize == static_cast<int32>(O3DS::ControlLimits::kMaxPayloadBytes),
	"O3DS::UnifiedMaxControlPayloadSize must equal ControlLimits::kMaxPayloadBytes (src/o3ds/parse_limits.h)");
static_assert(O3DS::UnifiedWireHeaderSize + O3DS::UnifiedMaxControlPayloadSize <= static_cast<int32>(O3DS::ControlLimits::kMaxEnvelopeBytes),
	"a full control envelope must fit ControlLimits::kMaxEnvelopeBytes");

namespace
{
	int64 ToInt64(uint64 V)
	{
		return V > static_cast<uint64>(MAX_int64) ? MAX_int64 : static_cast<int64>(V);
	}
}

namespace O3DControl
{
	std::string ToUtf8(const FString& In)
	{
		if (In.IsEmpty())
		{
			return std::string();
		}
		const FTCHARToUTF8 Converted(*In, In.Len());
		return std::string(reinterpret_cast<const char*>(Converted.Get()), static_cast<size_t>(Converted.Length()));
	}

	FString FromUtf8(const std::string& In)
	{
		if (In.empty())
		{
			return FString();
		}
		const FUTF8ToTCHAR Converted(In.data(), static_cast<int32>(In.size()));
		return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
	}

	void ToCore(const FO3DControlValue& In, O3DS::Control::Value& Out)
	{
		using O3DS::Control::Value;
		switch (In.Type)
		{
		case EO3DControlValueType::None: Out = Value::MakeNone(); return;
		case EO3DControlValueType::Bool: Out = Value::MakeBool(In.BoolValue); return;
		case EO3DControlValueType::Int: Out = Value::MakeInt(In.IntValue); return;
		case EO3DControlValueType::Float: Out = Value::MakeDouble(In.FloatValue); return;
		case EO3DControlValueType::String: Out = Value::MakeString(ToUtf8(In.StringValue)); return;
		case EO3DControlValueType::Name: Out = Value::MakeName(ToUtf8(In.StringValue)); return;
		case EO3DControlValueType::Vector:
			Out = Value::MakeVector3({ In.VectorValue.X, In.VectorValue.Y, In.VectorValue.Z });
			return;
		case EO3DControlValueType::Quat:
			Out = Value::MakeQuat({ In.QuatValue.X, In.QuatValue.Y, In.QuatValue.Z, In.QuatValue.W });
			return;
		case EO3DControlValueType::Transform:
		{
			const FVector T = In.TransformValue.GetTranslation();
			const FQuat R = In.TransformValue.GetRotation();
			const FVector S = In.TransformValue.GetScale3D();
			O3DS::Control::TransformValue Core;
			Core.translation = { T.X, T.Y, T.Z };
			Core.rotation = { R.X, R.Y, R.Z, R.W };
			Core.scale = { S.X, S.Y, S.Z };
			Out = Value::MakeTransform(Core);
			return;
		}
		case EO3DControlValueType::Color:
			Out = Value::MakeColor({ In.ColorValue.R, In.ColorValue.G, In.ColorValue.B, In.ColorValue.A });
			return;
		case EO3DControlValueType::Bytes:
			Out = Value::MakeBytes(std::vector<uint8_t>(In.BytesValue.GetData(), In.BytesValue.GetData() + In.BytesValue.Num()));
			return;
		}
		Out = Value::MakeNone();
	}

	FO3DControlValue FromCore(const O3DS::Control::Value& In)
	{
		using O3DS::Control::ValueType;
		switch (In.type)
		{
		case ValueType::None: return FO3DControlValue();
		case ValueType::Bool: return FO3DControlValue::MakeBool(In.b);
		case ValueType::Int: return FO3DControlValue::MakeInt(In.i);
		case ValueType::Double: return FO3DControlValue::MakeFloat(In.d);
		case ValueType::String: return FO3DControlValue::MakeString(FromUtf8(In.text));
		case ValueType::Name: return FO3DControlValue::MakeName(FromUtf8(In.text));
		case ValueType::Vector3: return FO3DControlValue::MakeVector(FVector(In.vec.x, In.vec.y, In.vec.z));
		case ValueType::Quat: return FO3DControlValue::MakeQuat(FQuat(In.quat.x, In.quat.y, In.quat.z, In.quat.w));
		case ValueType::Transform:
		{
			const O3DS::Control::TransformValue& T = In.transform;
			return FO3DControlValue::MakeTransform(FTransform(
				FQuat(T.rotation.x, T.rotation.y, T.rotation.z, T.rotation.w),
				FVector(T.translation.x, T.translation.y, T.translation.z),
				FVector(T.scale.x, T.scale.y, T.scale.z)));
		}
		case ValueType::Color: return FO3DControlValue::MakeColor(FLinearColor(In.color.r, In.color.g, In.color.b, In.color.a));
		case ValueType::Bytes:
		{
			TArray<uint8> Bytes;
			Bytes.Append(In.bytes.data(), static_cast<int32>(In.bytes.size()));
			return FO3DControlValue::MakeBytes(Bytes);
		}
		}
		return FO3DControlValue();
	}

	FO3DControlChange FromCore(const O3DS::Control::Change& In, const FString& StreamId)
	{
		using O3DS::Control::Change;
		FO3DControlChange Out;
		switch (In.kind)
		{
		case Change::Kind::ValueChanged: Out.Kind = FO3DControlChange::EKind::ValueChanged; break;
		case Change::Kind::ValueCleared: Out.Kind = FO3DControlChange::EKind::ValueCleared; break;
		case Change::Kind::Event: Out.Kind = FO3DControlChange::EKind::Event; break;
		}
		Out.Name = FromUtf8(In.name);
		if (In.kind != Change::Kind::ValueCleared)
		{
			Out.Value = FromCore(In.value);
		}
		Out.Meta.SourceId = FromUtf8(In.source_id);
		Out.Meta.SourceName = FromUtf8(In.source_name);
		Out.Meta.StreamId = StreamId;
		Out.Meta.TargetSubject = FromUtf8(In.target);
		Out.Meta.SenderTimeSec = static_cast<double>(In.sender_time_us) / 1.0e6;
		Out.Meta.Epoch = static_cast<int64>(In.epoch);
		Out.Meta.Version = In.kind == Change::Kind::Event ? 0 : ToInt64(In.version);
		Out.Meta.EventId = In.kind == Change::Kind::Event ? ToInt64(In.event_id) : 0;
		return Out;
	}
}
