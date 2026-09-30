// Copyright Lifelike & Believable. All Rights Reserved.

#if O3D_WITH_TRANSPORT_MOQ // Whole file: without the transport the module is a stub (O3DBuildFlags).

#include "Shared/MoQTypes.h"

#include "Containers/StringView.h"
#include "MoQFfiApi.h"

DEFINE_LOG_CATEGORY(LogMoQBridge);

namespace
{
    FStringView GetDefaultMessageForCode(EMoQErrorCode Code)
    {
        switch (Code)
        {
        case EMoQErrorCode::Ok: return TEXT("ok");
        case EMoQErrorCode::InvalidArgument: return TEXT("invalid argument");
        case EMoQErrorCode::ConnectionFailed: return TEXT("connection failed");
        case EMoQErrorCode::NotConnected: return TEXT("not connected");
        case EMoQErrorCode::Timeout: return TEXT("operation timed out");
        case EMoQErrorCode::Internal: return TEXT("internal error");
        case EMoQErrorCode::Unsupported: return TEXT("unsupported operation");
        case EMoQErrorCode::BufferTooSmall: return TEXT("buffer too small");
        default: return TEXT("unknown error");
        }
    }
}

FMoQResult FMoQResult::Ok()
{
    return FMoQResult{EMoQErrorCode::Ok, FString(), MOQ_OK};
}

FMoQResult FMoQResult::FromResult(const MoqResult& Result, const FMoQFfiApi& Api)
{
    const EMoQErrorCode Code = ToMoQErrorCode(Result.code);
    FString Message;
    if (Result.message != nullptr)
    {
        Message = MoQFfi::CopyAndFreeString(Api, Result.message);
    }
    else
    {
        Message = FString(GetDefaultMessageForCode(Code));
    }

    return FMoQResult{Code, MoveTemp(Message), Result.code};
}

FMoQResult FMoQResult::FromCode(EMoQErrorCode InCode, FString InMessage, MoqResultCode InRawCode)
{
    if (InMessage.IsEmpty())
    {
        InMessage = FString(GetDefaultMessageForCode(InCode));
    }

    return FMoQResult{InCode, MoveTemp(InMessage), InRawCode};
}

static EMoQErrorCode MapResultCode(MoqResultCode InCode)
{
    switch (InCode)
    {
    case MOQ_OK: return EMoQErrorCode::Ok;
    case MOQ_ERROR_INVALID_ARGUMENT: return EMoQErrorCode::InvalidArgument;
    case MOQ_ERROR_CONNECTION_FAILED: return EMoQErrorCode::ConnectionFailed;
    case MOQ_ERROR_NOT_CONNECTED: return EMoQErrorCode::NotConnected;
    case MOQ_ERROR_TIMEOUT: return EMoQErrorCode::Timeout;
    case MOQ_ERROR_INTERNAL: return EMoQErrorCode::Internal;
    case MOQ_ERROR_UNSUPPORTED: return EMoQErrorCode::Unsupported;
    case MOQ_ERROR_BUFFER_TOO_SMALL: return EMoQErrorCode::BufferTooSmall;
    default: return EMoQErrorCode::Unknown;
    }
}

EMoQErrorCode ToMoQErrorCode(MoqResultCode InCode)
{
    return MapResultCode(InCode);
}

FString LexToString(EMoQErrorCode InCode)
{
    switch (InCode)
    {
    case EMoQErrorCode::Ok: return TEXT("Ok");
    case EMoQErrorCode::InvalidArgument: return TEXT("InvalidArgument");
    case EMoQErrorCode::ConnectionFailed: return TEXT("ConnectionFailed");
    case EMoQErrorCode::NotConnected: return TEXT("NotConnected");
    case EMoQErrorCode::Timeout: return TEXT("Timeout");
    case EMoQErrorCode::Internal: return TEXT("Internal");
    case EMoQErrorCode::Unsupported: return TEXT("Unsupported");
    case EMoQErrorCode::BufferTooSmall: return TEXT("BufferTooSmall");
    default: return TEXT("Unknown");
    }
}

FString LexToString(MoqConnectionState State)
{
    switch (State)
    {
    case MOQ_STATE_DISCONNECTED: return TEXT("Disconnected");
    case MOQ_STATE_CONNECTING: return TEXT("Connecting");
    case MOQ_STATE_CONNECTED: return TEXT("Connected");
    case MOQ_STATE_FAILED: return TEXT("Failed");
    default: return TEXT("Unknown");
    }
}

#endif // O3D_WITH_TRANSPORT_MOQ
