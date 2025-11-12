#pragma once

#if WITH_EDITOR

#include "Delegates/Delegate.h"
#include "Templates/SharedPointer.h"

class UO3DSenderComponent;
class UO3DReceiverSettingsObject;
class SWidget;

namespace SocketsEditor
{
inline constexpr float TransportPanelWidth = 360.f;

namespace Sender
{
TSharedPtr<SWidget> BuildTcpSenderSettingsPanel(UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged);
TSharedPtr<SWidget> BuildUdpSenderSettingsPanel(UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged);
}

namespace Receiver
{
TSharedPtr<SWidget> BuildTcpReceiverSettingsPanel(UO3DReceiverSettingsObject* SettingsObject);
TSharedPtr<SWidget> BuildUdpReceiverSettingsPanel(UO3DReceiverSettingsObject* SettingsObject);
}
} // namespace SocketsEditor

#endif // WITH_EDITOR
