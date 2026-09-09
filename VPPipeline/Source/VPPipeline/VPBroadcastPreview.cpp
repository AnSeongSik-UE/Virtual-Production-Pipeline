#include "VPBroadcastPreview.h"

#include "VPBroadcastOutput.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Rendering/DrawElementTypes.h"
#include "Styling/CoreStyle.h"

FSlateRect UVPBroadcastPreview::CalculateAspectFitRect(
	const FVector2D& AvailableSize,
	float ContentAspectRatio)
{
	if (AvailableSize.X <= 0.0 || AvailableSize.Y <= 0.0 || ContentAspectRatio <= 0.0f)
	{
		return FSlateRect(0.0f, 0.0f, 0.0f, 0.0f);
	}

	double FittedWidth = AvailableSize.X;
	double FittedHeight = FittedWidth / ContentAspectRatio;
	if (FittedHeight > AvailableSize.Y)
	{
		FittedHeight = AvailableSize.Y;
		FittedWidth = FittedHeight * ContentAspectRatio;
	}

	const double Left = (AvailableSize.X - FittedWidth) * 0.5;
	const double Top = (AvailableSize.Y - FittedHeight) * 0.5;
	return FSlateRect(
		static_cast<float>(Left),
		static_cast<float>(Top),
		static_cast<float>(Left + FittedWidth),
		static_cast<float>(Top + FittedHeight));
}

void UVPBroadcastPreview::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (WidgetTree && !WidgetTree->RootWidget)
	{
		UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
		Root->SetVisibility(ESlateVisibility::HitTestInvisible);
		WidgetTree->RootWidget = Root;
	}
	RefreshBrush();
}

int32 UVPBroadcastPreview::NativePaint(
	const FPaintArgs& Args,
	const FGeometry& AllottedGeometry,
	const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements,
	int32 LayerId,
	const FWidgetStyle& InWidgetStyle,
	bool bParentEnabled) const
{
	const int32 BaseLayer = Super::NativePaint(
		Args,
		AllottedGeometry,
		MyCullingRect,
		OutDrawElements,
		LayerId,
		InWidgetStyle,
		bParentEnabled);
	if (PreviewBrush.GetResourceObject())
	{
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			BaseLayer + 1,
			AllottedGeometry.ToPaintGeometry(),
			FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")),
			ESlateDrawEffect::None,
			FLinearColor::Black);

		const FSlateRect PreviewRect = CalculateAspectFitRect(AllottedGeometry.GetLocalSize());
		const FVector2D PreviewSize(
			PreviewRect.Right - PreviewRect.Left,
			PreviewRect.Bottom - PreviewRect.Top);
		const FVector2D PreviewOffset(PreviewRect.Left, PreviewRect.Top);
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			BaseLayer + 2,
			AllottedGeometry.ToPaintGeometry(
				PreviewSize,
				FSlateLayoutTransform(PreviewOffset)),
			FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")),
			ESlateDrawEffect::None,
			BroadcastOutput
				? BroadcastOutput->GetBackgroundColor()
				: FLinearColor::Black);
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			BaseLayer + 3,
			AllottedGeometry.ToPaintGeometry(
				PreviewSize,
				FSlateLayoutTransform(PreviewOffset)),
			&PreviewBrush,
			ESlateDrawEffect::PreMultipliedAlpha,
			FLinearColor::White);
		return BaseLayer + 3;
	}
	return BaseLayer;
}

void UVPBroadcastPreview::SetBroadcastOutput(AVPBroadcastOutput* InBroadcastOutput)
{
	BroadcastOutput = InBroadcastOutput;
	RefreshBrush();
}

void UVPBroadcastPreview::RefreshBrush()
{
	if (BroadcastOutput && BroadcastOutput->GetBroadcastTexture())
	{
		PreviewBrush.SetResourceObject(BroadcastOutput->GetBroadcastTexture());
		PreviewBrush.DrawAs = ESlateBrushDrawType::Image;
	}
}
