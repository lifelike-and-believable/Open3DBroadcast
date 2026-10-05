// Copyright 2026 Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "O3DControlTypes.generated.h"

/*
 * Engine-side types for the control channel (docs/adr/0011-control-channel.md, item 8). They
 * mirror O3DS::Control::Value and Change from the core (src/o3ds/control.h); O3DControlConvert.h
 * converts between the two. Keys, event names and targets are FString throughout, because core
 * keys are case-sensitive and FName is not (and keeps its first-registered casing only in
 * builds with editor data).
 */

/** The type held by an FO3DControlValue. Mirrors O3DS::Control::ValueType. */
UENUM(BlueprintType)
enum class EO3DControlValueType : uint8
{
	None,
	Bool,
	Int,
	Float,
	String,
	/** An identifier-like string (an emotion, a cue name). Case is preserved. */
	Name,
	Vector,
	/** Rotations are stored as a quaternion: a rotator round trip is lossy, which would break exact comparison. */
	Quat,
	Transform,
	Color,
	Bytes
};

/** A typed control value. Only the member selected by Type is meaningful; String and Name both use StringValue. */
USTRUCT(BlueprintType)
struct OPEN3DSHARED_API FO3DControlValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	EO3DControlValueType Type = EO3DControlValueType::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	bool BoolValue = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	int64 IntValue = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	double FloatValue = 0.0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	FString StringValue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	FVector VectorValue = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	FQuat QuatValue = FQuat::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	FTransform TransformValue = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	FLinearColor ColorValue = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Open3DBroadcast|Control")
	TArray<uint8> BytesValue;

	static FO3DControlValue MakeBool(bool V);
	static FO3DControlValue MakeInt(int64 V);
	static FO3DControlValue MakeFloat(double V);
	static FO3DControlValue MakeString(const FString& V);
	static FO3DControlValue MakeName(const FString& V);
	static FO3DControlValue MakeVector(const FVector& V);
	static FO3DControlValue MakeQuat(const FQuat& V);
	static FO3DControlValue MakeTransform(const FTransform& V);
	static FO3DControlValue MakeColor(const FLinearColor& V);
	static FO3DControlValue MakeBytes(const TArray<uint8>& V);

	/** Exact comparison of the member selected by Type (as O3DS::Control::Value). */
	bool operator==(const FO3DControlValue& Other) const;
	bool operator!=(const FO3DControlValue& Other) const { return !(*this == Other); }

	/** Short human-readable form for logs and debugging, e.g. "Float 0.25". */
	FString ToString() const;
};

/** Where a control change came from. */
USTRUCT(BlueprintType)
struct OPEN3DSHARED_API FO3DControlMeta
{
	GENERATED_BODY()

	/** The sending component's id (one per sender component instance). */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FString SourceId;

	/** The sending component's display name. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FString SourceName;

	/** The receiving transport's stream. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FString StreamId;

	/** The subject the change is aimed at; empty for the whole stream. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	FString TargetSubject;

	/** When the change happened, in seconds on the sender's clock. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	double SenderTimeSec = 0.0;

	/** The sender's session; a higher epoch is a newer session of the same source. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	int64 Epoch = 0;

	/** Values: the version applied. Events: 0. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	int64 Version = 0;

	/** Events: the event's id within its source and epoch. Values: 0. */
	UPROPERTY(BlueprintReadOnly, Category = "Open3DBroadcast|Control")
	int64 EventId = 0;
};

/** Blueprint helpers to make and read FO3DControlValue. */
UCLASS()
class OPEN3DSHARED_API UO3DControlValueLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Bool)"))
	static FO3DControlValue MakeControlBool(bool Value) { return FO3DControlValue::MakeBool(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Integer64)"))
	static FO3DControlValue MakeControlInt(int64 Value) { return FO3DControlValue::MakeInt(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Float)"))
	static FO3DControlValue MakeControlFloat(double Value) { return FO3DControlValue::MakeFloat(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (String)"))
	static FO3DControlValue MakeControlString(const FString& Value) { return FO3DControlValue::MakeString(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Name)"))
	static FO3DControlValue MakeControlName(const FString& Value) { return FO3DControlValue::MakeName(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Vector)"))
	static FO3DControlValue MakeControlVector(const FVector& Value) { return FO3DControlValue::MakeVector(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Rotator)"))
	static FO3DControlValue MakeControlRotator(const FRotator& Value) { return FO3DControlValue::MakeQuat(Value.Quaternion()); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Quat)"))
	static FO3DControlValue MakeControlQuat(const FQuat& Value) { return FO3DControlValue::MakeQuat(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Transform)"))
	static FO3DControlValue MakeControlTransform(const FTransform& Value) { return FO3DControlValue::MakeTransform(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Color)"))
	static FO3DControlValue MakeControlColor(const FLinearColor& Value) { return FO3DControlValue::MakeColor(Value); }

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control", meta = (DisplayName = "Make Control Value (Bytes)"))
	static FO3DControlValue MakeControlBytes(const TArray<uint8>& Value) { return FO3DControlValue::MakeBytes(Value); }

	/** True for Bool; also true for Int (non-zero is true). */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static bool ControlAsBool(const FO3DControlValue& Value, bool& bSuccess);

	/** Int, or Float truncated toward zero. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static int64 ControlAsInt(const FO3DControlValue& Value, bool& bSuccess);

	/** Float, or Int converted. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static double ControlAsFloat(const FO3DControlValue& Value, bool& bSuccess);

	/** String or Name. */
	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static FString ControlAsString(const FO3DControlValue& Value, bool& bSuccess);

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static FVector ControlAsVector(const FO3DControlValue& Value, bool& bSuccess);

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static FRotator ControlAsRotator(const FO3DControlValue& Value, bool& bSuccess);

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static FQuat ControlAsQuat(const FO3DControlValue& Value, bool& bSuccess);

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static FTransform ControlAsTransform(const FO3DControlValue& Value, bool& bSuccess);

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static FLinearColor ControlAsColor(const FO3DControlValue& Value, bool& bSuccess);

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static TArray<uint8> ControlAsBytes(const FO3DControlValue& Value, bool& bSuccess);

	UFUNCTION(BlueprintPure, Category = "Open3DBroadcast|Control")
	static FString ControlValueToString(const FO3DControlValue& Value) { return Value.ToString(); }
};
