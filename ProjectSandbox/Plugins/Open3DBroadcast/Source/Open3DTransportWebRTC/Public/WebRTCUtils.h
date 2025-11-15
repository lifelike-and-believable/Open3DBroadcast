#pragma once

#include "CoreMinimal.h"

namespace WebRTCUtils
{
    // Convert C-string to FString
    inline FString FromAnsi(const char* S)
    {
        return S ? FString(UTF8_TO_TCHAR(S)) : FString();
    }
}
