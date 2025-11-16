#pragma once

#if WITH_EDITOR

#include "Delegates/Delegate.h"
#include "Templates/SharedPointer.h"

#include "O3DTransportConfigPanelBase.h"

class UO3DSenderComponent;
class UO3DReceiverSettingsObject;
class SWidget;

namespace SocketsEditor
{
inline constexpr float TransportPanelWidth = SO3DTransportConfigPanelBase::DefaultPanelWidth;

namespace Sender
{
TSharedPtr<SWidget> BuildTcpSenderSettingsPanel(UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged);
TSharedPtr<SWidget> BuildUdpSenderSettingsPanel(UO3DSenderComponent* SenderComponent, FSimpleDelegate OnConfigChanged);
}

namespace Receiver
{
TSharedPtr<SO3DTransportConfigPanelBase> BuildTcpReceiverSettingsPanel(UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit);
TSharedPtr<SO3DTransportConfigPanelBase> BuildUdpReceiverSettingsPanel(UO3DReceiverSettingsObject* SettingsObject, FSimpleDelegate OnSubmit);
}
} // namespace SocketsEditor

#endif // WITH_EDITOR
