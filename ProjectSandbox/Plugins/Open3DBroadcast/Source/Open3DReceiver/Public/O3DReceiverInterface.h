#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"
#include "O3DTransportTypes.h"

class ISerializedFrameConsumer;

/** Interface implemented by all transport receiver instances. */
class OPEN3DRECEIVER_API IOpen3DReceiver
{
public:
    virtual ~IOpen3DReceiver() = default;

    virtual bool Initialize(const FO3DTransportConfig& Config) = 0;
    virtual void SetConsumer(const TSharedPtr<ISerializedFrameConsumer>& Consumer) = 0;
    virtual bool Start() = 0;
    virtual void Stop() = 0;

    /** Poll available data and deliver to the previously configured sink. Returns processed frames. */
    virtual int32 Poll() = 0;

    virtual FO3DTransportStats GetStats() const = 0;
};

using FO3DReceiverFactory = TFunction<TSharedPtr<IOpen3DReceiver>()>;
