// Copyright Lifelike & Believable. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
THIRD_PARTY_INCLUDES_START
#include "moq_ffi.h"
THIRD_PARTY_INCLUDES_END

struct FMoQFfiApi;

/** Global log category for shared MoQ bridge components */
DECLARE_LOG_CATEGORY_EXTERN(LogMoQBridge, Log, All);

enum class EMoQErrorCode : uint8
{
    Ok = 0,
    InvalidArgument,
    ConnectionFailed,
    NotConnected,
    Timeout,
    Internal,
    Unsupported,
    BufferTooSmall,
    Unknown
};

/** Lightweight result helper used across the MoQ transport */
struct FMoQResult
{
    EMoQErrorCode Code = EMoQErrorCode::Ok;
    FString Message;
    MoqResultCode RawCode = MOQ_OK;

    FMoQResult() = default;
    FMoQResult(EMoQErrorCode InCode, FString InMessage, MoqResultCode InRawCode)
        : Code(InCode)
        , Message(MoveTemp(InMessage))
        , RawCode(InRawCode)
    {
    }

    FORCEINLINE bool IsOk() const
    {
        return Code == EMoQErrorCode::Ok;
    }

    static FMoQResult Ok();
    /** Converts an FFI result; frees Result.message through Api.FreeStr (it is owned by moq-ffi). */
    static FMoQResult FromResult(const MoqResult& Result, const FMoQFfiApi& Api);
    static FMoQResult FromCode(EMoQErrorCode InCode, FString InMessage, MoqResultCode InRawCode = MOQ_ERROR_INTERNAL);
};

EMoQErrorCode ToMoQErrorCode(MoqResultCode InCode);
FString LexToString(EMoQErrorCode InCode);
FString LexToString(MoqConnectionState State);
