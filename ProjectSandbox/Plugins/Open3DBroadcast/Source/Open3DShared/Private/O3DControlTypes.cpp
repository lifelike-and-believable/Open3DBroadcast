// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#include "O3DControlTypes.h"

namespace
{
	FO3DControlValue MakeTyped(EO3DControlValueType Type)
	{
		FO3DControlValue V;
		V.Type = Type;
		return V;
	}

	bool ExactlyEqual(const FVector& A, const FVector& B) { return A.X == B.X && A.Y == B.Y && A.Z == B.Z; }
	bool ExactlyEqual(const FQuat& A, const FQuat& B) { return A.X == B.X && A.Y == B.Y && A.Z == B.Z && A.W == B.W; }
}

FO3DControlValue FO3DControlValue::MakeBool(bool V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Bool); R.BoolValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeInt(int64 V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Int); R.IntValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeFloat(double V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Float); R.FloatValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeString(const FString& V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::String); R.StringValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeName(const FString& V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Name); R.StringValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeVector(const FVector& V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Vector); R.VectorValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeQuat(const FQuat& V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Quat); R.QuatValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeTransform(const FTransform& V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Transform); R.TransformValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeColor(const FLinearColor& V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Color); R.ColorValue = V; return R; }
FO3DControlValue FO3DControlValue::MakeBytes(const TArray<uint8>& V) { FO3DControlValue R = MakeTyped(EO3DControlValueType::Bytes); R.BytesValue = V; return R; }

bool FO3DControlValue::operator==(const FO3DControlValue& Other) const
{
	if (Type != Other.Type)
	{
		return false;
	}
	switch (Type)
	{
	case EO3DControlValueType::None: return true;
	case EO3DControlValueType::Bool: return BoolValue == Other.BoolValue;
	case EO3DControlValueType::Int: return IntValue == Other.IntValue;
	case EO3DControlValueType::Float: return FloatValue == Other.FloatValue;
	case EO3DControlValueType::String:
	case EO3DControlValueType::Name: return StringValue.Equals(Other.StringValue, ESearchCase::CaseSensitive);
	case EO3DControlValueType::Vector: return ExactlyEqual(VectorValue, Other.VectorValue);
	case EO3DControlValueType::Quat: return ExactlyEqual(QuatValue, Other.QuatValue);
	case EO3DControlValueType::Transform:
		return ExactlyEqual(TransformValue.GetTranslation(), Other.TransformValue.GetTranslation())
			&& ExactlyEqual(TransformValue.GetRotation(), Other.TransformValue.GetRotation())
			&& ExactlyEqual(TransformValue.GetScale3D(), Other.TransformValue.GetScale3D());
	case EO3DControlValueType::Color: return ColorValue == Other.ColorValue;
	case EO3DControlValueType::Bytes: return BytesValue == Other.BytesValue;
	}
	return false;
}

FString FO3DControlValue::ToString() const
{
	switch (Type)
	{
	case EO3DControlValueType::None: return TEXT("None");
	case EO3DControlValueType::Bool: return FString::Printf(TEXT("Bool %s"), BoolValue ? TEXT("true") : TEXT("false"));
	case EO3DControlValueType::Int: return FString::Printf(TEXT("Int %lld"), static_cast<long long>(IntValue));
	case EO3DControlValueType::Float: return FString::Printf(TEXT("Float %g"), FloatValue);
	case EO3DControlValueType::String: return FString::Printf(TEXT("String \"%s\""), *StringValue);
	case EO3DControlValueType::Name: return FString::Printf(TEXT("Name %s"), *StringValue);
	case EO3DControlValueType::Vector: return FString::Printf(TEXT("Vector %s"), *VectorValue.ToString());
	case EO3DControlValueType::Quat: return FString::Printf(TEXT("Quat %s"), *QuatValue.ToString());
	case EO3DControlValueType::Transform: return FString::Printf(TEXT("Transform %s"), *TransformValue.ToString());
	case EO3DControlValueType::Color: return FString::Printf(TEXT("Color %s"), *ColorValue.ToString());
	case EO3DControlValueType::Bytes: return FString::Printf(TEXT("Bytes [%d]"), BytesValue.Num());
	}
	return TEXT("Unknown");
}

bool UO3DControlValueLibrary::ControlAsBool(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Bool || Value.Type == EO3DControlValueType::Int;
	return Value.Type == EO3DControlValueType::Bool ? Value.BoolValue : (Value.Type == EO3DControlValueType::Int && Value.IntValue != 0);
}

int64 UO3DControlValueLibrary::ControlAsInt(const FO3DControlValue& Value, bool& bSuccess)
{
	if (Value.Type == EO3DControlValueType::Int)
	{
		bSuccess = true;
		return Value.IntValue;
	}
	if (Value.Type == EO3DControlValueType::Float && FMath::IsFinite(Value.FloatValue)
		&& Value.FloatValue >= static_cast<double>(MIN_int64) && Value.FloatValue < static_cast<double>(MAX_int64))
	{
		bSuccess = true;
		return static_cast<int64>(Value.FloatValue);
	}
	bSuccess = false;
	return 0;
}

double UO3DControlValueLibrary::ControlAsFloat(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Float || Value.Type == EO3DControlValueType::Int;
	if (Value.Type == EO3DControlValueType::Float)
	{
		return Value.FloatValue;
	}
	return Value.Type == EO3DControlValueType::Int ? static_cast<double>(Value.IntValue) : 0.0;
}

FString UO3DControlValueLibrary::ControlAsString(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::String || Value.Type == EO3DControlValueType::Name;
	return bSuccess ? Value.StringValue : FString();
}

FVector UO3DControlValueLibrary::ControlAsVector(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Vector;
	return bSuccess ? Value.VectorValue : FVector::ZeroVector;
}

FRotator UO3DControlValueLibrary::ControlAsRotator(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Quat;
	return bSuccess ? Value.QuatValue.Rotator() : FRotator::ZeroRotator;
}

FQuat UO3DControlValueLibrary::ControlAsQuat(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Quat;
	return bSuccess ? Value.QuatValue : FQuat::Identity;
}

FTransform UO3DControlValueLibrary::ControlAsTransform(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Transform;
	return bSuccess ? Value.TransformValue : FTransform::Identity;
}

FLinearColor UO3DControlValueLibrary::ControlAsColor(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Color;
	return bSuccess ? Value.ColorValue : FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

TArray<uint8> UO3DControlValueLibrary::ControlAsBytes(const FO3DControlValue& Value, bool& bSuccess)
{
	bSuccess = Value.Type == EO3DControlValueType::Bytes;
	return bSuccess ? Value.BytesValue : TArray<uint8>();
}
