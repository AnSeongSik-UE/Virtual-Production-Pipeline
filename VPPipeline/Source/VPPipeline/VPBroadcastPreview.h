#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VPBroadcastPreview.generated.h"

class AVPBroadcastOutput;

/** Always-visible clean broadcast preview shown behind the operator dashboard. */
UCLASS()
class VPPIPELINE_API UVPBroadcastPreview : public UUserWidget
{
	GENERATED_BODY()

public:
	void SetBroadcastOutput(AVPBroadcastOutput* InBroadcastOutput);

	/** Returns the largest centered rectangle that preserves the broadcast aspect ratio. */
	static FSlateRect CalculateAspectFitRect(
		const FVector2D& AvailableSize,
		float ContentAspectRatio = 16.0f / 9.0f);

protected:
	virtual void NativeOnInitialized() override;
	virtual int32 NativePaint(
		const FPaintArgs& Args,
		const FGeometry& AllottedGeometry,
		const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements,
		int32 LayerId,
		const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override;

private:
	UPROPERTY(Transient)
	TObjectPtr<AVPBroadcastOutput> BroadcastOutput;

	FSlateBrush PreviewBrush;

	void RefreshBrush();
};
