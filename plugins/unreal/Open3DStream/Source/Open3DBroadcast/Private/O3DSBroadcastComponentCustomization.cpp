#include "O3DSBroadcastComponentCustomization.h"

#include "DetailLayoutBuilder.h"
#include "IDetailPropertyRow.h"
#include "DetailWidgetRow.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Input/SEditableTextBox.h"

#include "O3DSBroadcastComponent.h"
#include "WebRTCConnectorFactory.h"

TSharedRef<IDetailCustomization> FO3DSBroadcastComponentCustomization::MakeInstance()
{
    return MakeShareable(new FO3DSBroadcastComponentCustomization());
}

void FO3DSBroadcastComponentCustomization::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
#if WITH_EDITOR
    TArray<TWeakObjectPtr<UObject>> Objects;
    DetailBuilder.GetObjectsBeingCustomized(Objects);
    if (Objects.Num() == 0) { return; }

    UO3DSBroadcastComponent* Comp = Cast<UO3DSBroadcastComponent>(Objects[0].Get());
    if (!Comp) { return; }

    // LiveKitToken property handle
    TSharedRef<IPropertyHandle> TokenHandle = DetailBuilder.GetProperty(
        GET_MEMBER_NAME_CHECKED(UO3DSBroadcastComponent, LiveKitToken),
        UO3DSBroadcastComponent::StaticClass());

    if (TokenHandle->IsValidHandle())
    {
        if (IDetailPropertyRow* Row = DetailBuilder.EditDefaultProperty(TokenHandle))
        {
            Row->CustomWidget()
            .NameContent()
            [
                SNew(STextBlock)
                .Text(NSLOCTEXT("Open3DStream", "LiveKitTokenLabel", "Token"))
                .Font(IDetailLayoutBuilder::GetDetailFont())
            ]
            .ValueContent()
            .MinDesiredWidth(250.f)
            [
                SNew(SEditableTextBox)
                .Text_Lambda([TokenHandle]() -> FText {
                    FString Val; TokenHandle->GetValue(Val);
                    return FText::FromString(Val);
                })
                .OnTextCommitted_Lambda([TokenHandle](const FText& NewText, ETextCommit::Type){
                    TokenHandle->SetValue(NewText.ToString());
                })
                .HintText_Lambda([Comp]() -> FText {
                    EO3DSWebRtcBackend Backend = EO3DSWebRtcBackend::LibDataChannel;
                    if (Comp->WebRtcBackend == EO3DSWebRtcBackendSender::LiveKit)
                    {
                        Backend = EO3DSWebRtcBackend::LiveKit;
                    }
                    return FText::FromString(FWebRTCConnectorFactory::BackendTokenHint(Backend));
                })
            ];
        }
    }
#endif
}
