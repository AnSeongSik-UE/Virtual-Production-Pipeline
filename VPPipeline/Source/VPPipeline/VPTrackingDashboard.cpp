#include "VPTrackingDashboard.h"

#include "VPAvatarManager.h"
#include "VPBroadcastOutput.h"
#include "VPBroadcastPreview.h"
#include "VPAnimInstance.h"
#include "VPUDPReceiver.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ComboBoxString.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/ScrollBox.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/SizeBox.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Fonts/CompositeFont.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "Rendering/DrawElementTypes.h"
#include "Styling/CoreStyle.h"

namespace
{
const FLinearColor PanelColor(0.018f, 0.026f, 0.043f, 0.96f);
const FLinearColor RaisedPanelColor(0.035f, 0.047f, 0.072f, 0.96f);
const FLinearColor AccentColor(0.08f, 0.82f, 0.92f, 1.0f);
const FLinearColor PrimaryButtonColor(0.08f, 0.35f, 0.42f, 1.0f);
const FLinearColor SecondaryButtonColor(0.12f, 0.15f, 0.21f, 1.0f);
const FLinearColor TextColor(0.91f, 0.94f, 0.98f, 1.0f);
const FLinearColor MutedTextColor(0.56f, 0.64f, 0.73f, 1.0f);
const FLinearColor TrackingGoodColor(0.13f, 0.92f, 0.48f, 1.0f);
const FLinearColor TrackingWeakColor(1.0f, 0.67f, 0.12f, 1.0f);
const FLinearColor TrackingLostColor(0.98f, 0.24f, 0.28f, 1.0f);
constexpr float PacketRateSampleSeconds = 1.0f;
constexpr float PacketSilenceTimeoutSeconds = 0.5f;
constexpr float DelayedPacketRateThreshold = 20.0f;
constexpr float GuidanceSuccessDisplaySeconds = 2.0f;
constexpr float CalibrationFailureDisplaySeconds = 4.0f;
constexpr float BackgroundColorCommitDelaySeconds = 0.2f;
constexpr float OutputFPSCommitDelaySeconds = 0.4f;

FLinearColor ConfidenceColor(float Confidence)
{
	if (Confidence >= 0.6f)
	{
		return TrackingGoodColor;
	}
	if (Confidence >= 0.3f)
	{
		return TrackingWeakColor;
	}
	return TrackingLostColor;
}

TSharedPtr<const FCompositeFont> GetDashboardFont()
{
	static TSharedPtr<const FCompositeFont> Font = MakeShared<FStandaloneCompositeFont>(
		NAME_None,
		FPaths::EngineContentDir() / TEXT("Slate/Fonts/DroidSansFallback.ttf"),
		EFontHinting::Default,
		EFontLoadingPolicy::LazyLoad);
	return Font;
}

bool IsCalibrationActive(EVPCalibrationState State)
{
	return State == EVPCalibrationState::CountingDown ||
		State == EVPCalibrationState::WaitingForTracking;
}

bool IsValidationActive(EVPArmValidationStage Stage)
{
	return Stage == EVPArmValidationStage::Neutral ||
		Stage == EVPArmValidationStage::LeftArmRaised ||
		Stage == EVPArmValidationStage::NeutralAfterLeft ||
		Stage == EVPArmValidationStage::RightArmRaised;
}

FString BuildTransportDiagnostics(
	const UVPUDPReceiver* Receiver,
	bool bPacketRateReady,
	float PacketsPerSecond)
{
	if (!Receiver)
	{
		return TEXT("통신 진단 · UDP 수신기 연결 대기");
	}

	const FString RateText = bPacketRateReady
		? FString::Printf(TEXT("%.1ffps"), PacketsPerSecond)
		: TEXT("측정 대기");
	const FVPTrackingLatencySnapshot Latency = Receiver->GetLatencySnapshot();
	const FString LatencyText = Latency.bHasSamples
		? FString::Printf(
			TEXT("캡처→UDP 수신 %.1fms\n")
			TEXT("캡처→UE 전달 · 현재 %.1fms · 평균 %.1fms · 최근 P95 %.1fms · 최대 %.1fms"),
			Latency.LatestCaptureToReceiveMs,
			Latency.LatestCaptureToGameThreadMs,
			Latency.AverageCaptureToGameThreadMs,
			Latency.P95CaptureToGameThreadMs,
			Latency.MaxCaptureToGameThreadMs)
		: FString::Printf(
			TEXT("캡처→UE 전달 지연 측정 대기 · 유효하지 않은 시각 %d"),
			Latency.InvalidSampleCount);

	return FString::Printf(
		TEXT("통신 진단 · 수신률 %s · 수신 %d · 적용 %d\n")
		TEXT("거부 %d · 드롭 %d · 최신교체 %d · 버전불일치 %d\n%s"),
		*RateText,
		Receiver->GetPacketCount(),
		Latency.DeliveredFrameCount,
		Receiver->GetRejectedPacketCount(),
		Receiver->GetDroppedPacketCount(),
		Receiver->GetSupersededFrameCount(),
		Receiver->GetProtocolMismatchCount(),
		*LatencyText);
}

bool IsAvatarPreviewArea(const FGeometry& Geometry, const FPointerEvent& PointerEvent)
{
	const FVector2D LocalPosition = Geometry.AbsoluteToLocal(PointerEvent.GetScreenSpacePosition());
	const FVector2D LocalSize = Geometry.GetLocalSize();
	const FSlateRect PreviewRect = UVPBroadcastPreview::CalculateAspectFitRect(LocalSize);
	const bool bInsideFittedPreview =
		LocalPosition.X >= PreviewRect.Left &&
		LocalPosition.X <= PreviewRect.Right &&
		LocalPosition.Y >= PreviewRect.Top &&
		LocalPosition.Y <= PreviewRect.Bottom;
	return bInsideFittedPreview &&
		LocalPosition.X >= 360.0 &&
		LocalPosition.Y >= 96.0 &&
		LocalPosition.Y <= LocalSize.Y - 52.0;
}
}

void UVPTrackingDashboard::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	FindTrackingTargets();
	RefreshDashboard();
}

void UVPTrackingDashboard::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (AvatarManager)
	{
		AnimInstance = AvatarManager->GetActiveAnimInstance();
		if (ObservedAvatarLibraryRevision != AvatarManager->GetLibraryRevision())
		{
			RefreshAvatarSelector();
		}
		if (ObservedAvatarNoticeRevision != AvatarManager->GetUserNoticeRevision())
		{
			ObservedAvatarNoticeRevision = AvatarManager->GetUserNoticeRevision();
			if (!AvatarManager->GetUserNoticeText().IsEmpty())
			{
				ShowNoticeModal(
					AvatarManager->GetUserNoticeText(),
					AvatarManager->GetUserNoticeSeverity() == EVPAvatarNoticeSeverity::Error);
			}
		}
	}
	if (BroadcastOutput &&
		ObservedObsFPSNoticeRevision != BroadcastOutput->GetObsFPSNoticeRevision() &&
		!IsModalOpen())
	{
		ObservedObsFPSNoticeRevision = BroadcastOutput->GetObsFPSNoticeRevision();
		if (!BroadcastOutput->GetObsFPSNoticeText().IsEmpty())
		{
			ShowNoticeModal(
				BroadcastOutput->GetObsFPSNoticeText(),
				false,
				TEXT("OBS 출력 중"));
		}
	}

	TargetSearchElapsedSeconds += InDeltaTime;
	if ((!Receiver || !BroadcastOutput) && TargetSearchElapsedSeconds >= 0.5f)
	{
		TargetSearchElapsedSeconds = 0.0f;
		FindTrackingTargets();
	}
	UpdatePacketRate(InDeltaTime);
	UpdateGuidanceVisibility(InDeltaTime);
	UpdateBackgroundColorCommit(InDeltaTime);
	UpdateAvatarExposureCommit(InDeltaTime);
	UpdateOutputFPSCommit(InDeltaTime);
	RefreshDashboard();
}

void UVPTrackingDashboard::NativeDestruct()
{
	if (bBackgroundColorCommitPending && IsValid(BroadcastOutput))
	{
		BroadcastOutput->CommitBackgroundColor();
	}
	if (bAvatarExposureCommitPending && IsValid(BroadcastOutput))
	{
		BroadcastOutput->CommitAvatarExposure();
	}
	if (bOutputFPSCommitPending && IsValid(BroadcastOutput))
	{
		BroadcastOutput->CommitOutputFPS();
	}
	bBackgroundColorCommitPending = false;
	bAvatarExposureCommitPending = false;
	bOutputFPSCommitPending = false;
	Super::NativeDestruct();
}

FReply UVPTrackingDashboard::NativeOnMouseButtonDown(
	const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if (IsModalOpen())
	{
		return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
	}

	if (InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton &&
		BroadcastOutput &&
		BroadcastOutput->IsAvatarCaptureReady() &&
		IsAvatarPreviewArea(InGeometry, InMouseEvent))
	{
		bPanningCamera = true;
		LastCameraPointerPosition = InMouseEvent.GetScreenSpacePosition();
		return FReply::Handled().CaptureMouse(TakeWidget());
	}

	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

FReply UVPTrackingDashboard::NativeOnMouseButtonUp(
	const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if (bPanningCamera && InMouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bPanningCamera = false;
		return FReply::Handled().ReleaseMouseCapture();
	}

	return Super::NativeOnMouseButtonUp(InGeometry, InMouseEvent);
}

FReply UVPTrackingDashboard::NativeOnMouseMove(
	const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if (bPanningCamera && BroadcastOutput)
	{
		const FVector2D CurrentPosition = InMouseEvent.GetScreenSpacePosition();
		BroadcastOutput->PanCamera(CurrentPosition - LastCameraPointerPosition);
		LastCameraPointerPosition = CurrentPosition;
		return FReply::Handled();
	}

	return Super::NativeOnMouseMove(InGeometry, InMouseEvent);
}

FReply UVPTrackingDashboard::NativeOnMouseWheel(
	const FGeometry& InGeometry,
	const FPointerEvent& InMouseEvent)
{
	if (IsModalOpen())
	{
		return Super::NativeOnMouseWheel(InGeometry, InMouseEvent);
	}

	if (BroadcastOutput &&
		BroadcastOutput->IsAvatarCaptureReady() &&
		IsAvatarPreviewArea(InGeometry, InMouseEvent))
	{
		BroadcastOutput->AdjustCameraZoom(InMouseEvent.GetWheelDelta());
		return FReply::Handled();
	}

	return Super::NativeOnMouseWheel(InGeometry, InMouseEvent);
}

void UVPTrackingDashboard::NativeOnMouseCaptureLost(const FCaptureLostEvent& CaptureLostEvent)
{
	bPanningCamera = false;
	Super::NativeOnMouseCaptureLost(CaptureLostEvent);
}

FVector2D UVPTrackingDashboard::ProjectPoseLandmark(
	const FVector& NormalizedPosition,
	const FVector2D& PanelOrigin,
	const FVector2D& PanelSize)
{
	return PanelOrigin + FVector2D(
		FMath::Clamp(NormalizedPosition.X, 0.0, 1.0) * PanelSize.X,
		FMath::Clamp(NormalizedPosition.Y, 0.0, 1.0) * PanelSize.Y);
}

float UVPTrackingDashboard::GetPoseLandmarkConfidence(const FVPPoseLandmark& Landmark)
{
	return FMath::Clamp(FMath::Min(Landmark.Visibility, Landmark.Presence), 0.0f, 1.0f);
}

FText UVPTrackingDashboard::BuildPoseStatusText(
	const FVPTrackingFrame& Frame,
	float& OutWeakestConfidence)
{
	OutWeakestConfidence = 0.0f;
	if (!Frame.bPoseTracked || Frame.PoseLandmarks.Num() < 15)
	{
		return FText::FromString(TEXT("원시 POSE\n인식되지 않음"));
	}

	struct FNamedLandmark
	{
		int32 Index;
		const TCHAR* Name;
	};
	static constexpr FNamedLandmark RequiredLandmarks[] = {
		{ 0, TEXT("얼굴") },
		{ 7, TEXT("왼쪽 귀") },
		{ 8, TEXT("오른쪽 귀") },
		{ 11, TEXT("왼쪽 어깨") },
		{ 12, TEXT("오른쪽 어깨") },
		{ 13, TEXT("왼쪽 팔꿈치") },
		{ 14, TEXT("오른쪽 팔꿈치") },
	};

	OutWeakestConfidence = 1.0f;
	const TCHAR* WeakestName = TEXT("주요 부위");
	for (const FNamedLandmark& Candidate : RequiredLandmarks)
	{
		const float Confidence = GetPoseLandmarkConfidence(Frame.PoseLandmarks[Candidate.Index]);
		if (Confidence < OutWeakestConfidence)
		{
			OutWeakestConfidence = Confidence;
			WeakestName = Candidate.Name;
		}
	}

	return FText::FromString(OutWeakestConfidence >= 0.6f
		? FString::Printf(TEXT("원시 POSE · 정상\n가장 낮음: %s %.2f"), WeakestName, OutWeakestConfidence)
		: FString::Printf(TEXT("원시 POSE · 확인 필요\n%s 인식 약함 · %.2f"), WeakestName, OutWeakestConfidence));
}

int32 UVPTrackingDashboard::NativePaint(
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

	if (!Receiver)
	{
		return BaseLayer;
	}

	const FVPTrackingFrame Frame = Receiver->GetLatestTrackingData();
	if (!Frame.bPoseTracked || Frame.PoseLandmarks.Num() < 15)
	{
		return BaseLayer;
	}

	const FVector2D LocalSize = AllottedGeometry.GetLocalSize();
	const FVector2D PanelOrigin(LocalSize.X - 292.0, 153.0);
	const FVector2D PanelSize(266.0, 174.0);
	const FPaintGeometry PaintGeometry = AllottedGeometry.ToPaintGeometry();
	const int32 SkeletonLayer = BaseLayer + 1;

	auto DrawSegment = [&](int32 StartIndex, int32 EndIndex)
	{
		const FVPPoseLandmark& Start = Frame.PoseLandmarks[StartIndex];
		const FVPPoseLandmark& End = Frame.PoseLandmarks[EndIndex];
		const FVector2D StartPoint = ProjectPoseLandmark(Start.Position, PanelOrigin, PanelSize);
		const FVector2D EndPoint = ProjectPoseLandmark(End.Position, PanelOrigin, PanelSize);
		TArray<FVector2f> Points;
		Points.Add(FVector2f(StartPoint));
		Points.Add(FVector2f(EndPoint));
		FSlateDrawElement::MakeLines(
			OutDrawElements,
			SkeletonLayer,
			PaintGeometry,
			MoveTemp(Points),
			ESlateDrawEffect::None,
			ConfidenceColor(FMath::Min(
				GetPoseLandmarkConfidence(Start),
				GetPoseLandmarkConfidence(End))),
			true,
			3.0f);
	};

	DrawSegment(7, 0);
	DrawSegment(0, 8);
	DrawSegment(7, 11);
	DrawSegment(8, 12);
	DrawSegment(11, 12);
	DrawSegment(11, 13);
	DrawSegment(12, 14);

	const int32 LandmarkIndices[] = { 0, 7, 8, 11, 12, 13, 14 };
	const FSlateBrush* NodeBrush = FCoreStyle::Get().GetBrush(TEXT("WhiteBrush"));
	for (const int32 Index : LandmarkIndices)
	{
		const FVPPoseLandmark& Landmark = Frame.PoseLandmarks[Index];
		const FVector2D Point = ProjectPoseLandmark(Landmark.Position, PanelOrigin, PanelSize);
		const FVector2f NodeOffset(
			static_cast<float>(Point.X - 4.0),
			static_cast<float>(Point.Y - 4.0));
		FSlateDrawElement::MakeBox(
			OutDrawElements,
			SkeletonLayer + 1,
			AllottedGeometry.ToPaintGeometry(
				FVector2f(8.0f, 8.0f),
				FSlateLayoutTransform(NodeOffset)),
			NodeBrush,
			ESlateDrawEffect::None,
			ConfidenceColor(GetPoseLandmarkConfidence(Landmark)));
	}

	return SkeletonLayer + 1;
}

UTextBlock* UVPTrackingDashboard::CreateText(
	const FString& Text,
	int32 FontSize,
	const FLinearColor& Color)
{
	UTextBlock* TextBlock = WidgetTree->ConstructWidget<UTextBlock>();
	TextBlock->SetText(FText::FromString(Text));
	TextBlock->SetColorAndOpacity(FSlateColor(Color));
	TextBlock->SetFont(FSlateFontInfo(GetDashboardFont(), FontSize));
	TextBlock->SetAutoWrapText(true);
	return TextBlock;
}

UButton* UVPTrackingDashboard::CreateButton(
	UVerticalBox* Parent,
	const FString& Label,
	UTextBlock*& OutLabel,
	const FLinearColor& BackgroundColor)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	Button->SetBackgroundColor(BackgroundColor);
	Button->SetCursor(EMouseCursor::Hand);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Button->IsFocusable = false;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	OutLabel = CreateText(Label, 15, TextColor);
	OutLabel->SetJustification(ETextJustify::Center);
	Button->AddChild(OutLabel);
	UVerticalBoxSlot* ButtonSlot = Parent->AddChildToVerticalBox(Button);
	ButtonSlot->SetPadding(FMargin(0.0f, 3.0f));
	ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
	ButtonSlot->SetVerticalAlignment(VAlign_Center);
	return Button;
}

UButton* UVPTrackingDashboard::CreateInlineButton(
	UHorizontalBox* Parent,
	const FString& Label,
	const FLinearColor& BackgroundColor,
	UTextBlock*& OutLabel)
{
	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	Button->SetBackgroundColor(BackgroundColor);
	Button->SetCursor(EMouseCursor::Hand);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Button->IsFocusable = false;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	OutLabel = CreateText(Label, 12, TextColor);
	OutLabel->SetJustification(ETextJustify::Center);
	Button->AddChild(OutLabel);
	UHorizontalBoxSlot* InlineSlot = Parent->AddChildToHorizontalBox(Button);
	InlineSlot->SetPadding(FMargin(2.0f));
	InlineSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	InlineSlot->SetVerticalAlignment(VAlign_Center);
	return Button;
}

void UVPTrackingDashboard::BuildLayout()
{
	if (!WidgetTree || WidgetTree->RootWidget)
	{
		return;
	}

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	Root->SetVisibility(ESlateVisibility::Visible);
	WidgetTree->RootWidget = Root;

	UBorder* LeftBorder = WidgetTree->ConstructWidget<UBorder>();
	LeftBorder->SetBrushColor(PanelColor);
	LeftBorder->SetPadding(FMargin(18.0f, 16.0f));
	UCanvasPanelSlot* LeftSlot = Root->AddChildToCanvas(LeftBorder);
	LeftSlot->SetAnchors(FAnchors(0.0f, 0.0f, 0.0f, 1.0f));
	LeftSlot->SetOffsets(FMargin(12.0f, 12.0f, 342.0f, 58.0f));

	UVerticalBox* PaletteShell = WidgetTree->ConstructWidget<UVerticalBox>();
	LeftBorder->SetContent(PaletteShell);

	UTextBlock* Title = CreateText(TEXT("Virtual Production Pipeline"), 22, TextColor);
	Title->SetJustification(ETextJustify::Center);
	PaletteShell->AddChildToVerticalBox(Title)->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 0.0f));
	UTextBlock* Subtitle = CreateText(TEXT("WEBCAM AVATAR CONTROL"), 11, AccentColor);
	Subtitle->SetJustification(ETextJustify::Center);
	PaletteShell->AddChildToVerticalBox(Subtitle)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));

	USizeBox* PaletteButtonSize = WidgetTree->ConstructWidget<USizeBox>();
	PaletteButtonSize->SetHeightOverride(38.0f);
	PaletteShell->AddChildToVerticalBox(PaletteButtonSize)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 9.0f));
	UHorizontalBox* PaletteButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	PaletteButtonSize->AddChild(PaletteButtonRow);
	UTextBlock* BroadcastPaletteLabel = nullptr;
	BroadcastPaletteButton = CreateInlineButton(
		PaletteButtonRow, TEXT("방송"), PrimaryButtonColor, BroadcastPaletteLabel);
	BroadcastPaletteLabel->SetAutoWrapText(false);
	CastChecked<UHorizontalBoxSlot>(BroadcastPaletteButton->Slot)->SetVerticalAlignment(VAlign_Fill);
	BroadcastPaletteButton->OnClicked.AddDynamic(
		this, &UVPTrackingDashboard::HandleBroadcastPaletteClicked);
	UTextBlock* TrackingSettingsPaletteLabel = nullptr;
	TrackingSettingsPaletteButton = CreateInlineButton(
		PaletteButtonRow, TEXT("트래킹"), SecondaryButtonColor, TrackingSettingsPaletteLabel);
	TrackingSettingsPaletteLabel->SetAutoWrapText(false);
	CastChecked<UHorizontalBoxSlot>(TrackingSettingsPaletteButton->Slot)->SetVerticalAlignment(VAlign_Fill);
	TrackingSettingsPaletteButton->OnClicked.AddDynamic(
		this, &UVPTrackingDashboard::HandleTrackingSettingsPaletteClicked);

	PaletteSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	UVerticalBoxSlot* PaletteSwitcherSlot = PaletteShell->AddChildToVerticalBox(PaletteSwitcher);
	PaletteSwitcherSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	PaletteSwitcherSlot->SetHorizontalAlignment(HAlign_Fill);
	PaletteSwitcherSlot->SetVerticalAlignment(VAlign_Fill);

	UScrollBox* BasicScroll = WidgetTree->ConstructWidget<UScrollBox>();
	BasicScroll->SetScrollBarVisibility(ESlateVisibility::Visible);
	PaletteSwitcher->AddChild(BasicScroll);
	ControlPanel = WidgetTree->ConstructWidget<UVerticalBox>();
	BasicScroll->AddChild(ControlPanel);

	UScrollBox* TrackingSettingsScroll = WidgetTree->ConstructWidget<UScrollBox>();
	TrackingSettingsScroll->SetScrollBarVisibility(ESlateVisibility::Visible);
	PaletteSwitcher->AddChild(TrackingSettingsScroll);
	UVerticalBox* TrackingSettingsPanel = WidgetTree->ConstructWidget<UVerticalBox>();
	TrackingSettingsScroll->AddChild(TrackingSettingsPanel);
	SetTrackingSettingsPaletteActive(false);

	UTextBlock* AvatarSection = CreateText(TEXT("아바타"), 16, AccentColor);
	ControlPanel->AddChildToVerticalBox(AvatarSection)->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 2.0f));
	AvatarStatusText = CreateText(TEXT("아바타 없음 · VRM 파일을 추가하세요."), 12, MutedTextColor);
	ControlPanel->AddChildToVerticalBox(AvatarStatusText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 3.0f));
	AvatarSelector = WidgetTree->ConstructWidget<UComboBoxString>();
	AvatarSelector->SetToolTipText(FText::FromString(TEXT("사용할 아바타를 선택합니다.")));
	AvatarSelector->OnSelectionChanged.AddDynamic(
		this, &UVPTrackingDashboard::HandleAvatarSelectionChanged);
	ControlPanel->AddChildToVerticalBox(AvatarSelector)->SetPadding(FMargin(0.0f, 2.0f));
	UTextBlock* AddAvatarLabel = nullptr;
	UButton* AddAvatarButton = CreateButton(
		ControlPanel, TEXT("VRM 추가 (파일 선택)"), AddAvatarLabel, PrimaryButtonColor);
	AddAvatarButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleAddAvatarClicked);
	UTextBlock* DropHelp = CreateText(
		TEXT("또는 .vrm 파일을 이 창에 드래그앤드롭하세요."), 11, MutedTextColor);
	ControlPanel->AddChildToVerticalBox(DropHelp)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));
	UTextBlock* DeleteAvatarLabel = nullptr;
	UButton* DeleteAvatarButton = CreateButton(
		ControlPanel, TEXT("현재 아바타 삭제"), DeleteAvatarLabel, SecondaryButtonColor);
	DeleteAvatarButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleDeleteAvatarClicked);
	UTextBlock* ResetCameraLabel = nullptr;
	UButton* ResetCameraButton = CreateButton(
		ControlPanel, TEXT("카메라 전신 자동 맞춤"), ResetCameraLabel, SecondaryButtonColor);
	ResetCameraButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleResetCameraClicked);
	UTextBlock* CameraHelp = CreateText(
		TEXT("중앙 화면 드래그: 이동 · 휠: 확대/축소 · 위치는 아바타별 자동 저장"),
		11,
		MutedTextColor);
	ControlPanel->AddChildToVerticalBox(CameraHelp)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 7.0f));

	UTextBlock* BroadcastSection = CreateText(TEXT("방송 출력 · SPOUT 기본"), 16, AccentColor);
	ControlPanel->AddChildToVerticalBox(BroadcastSection)->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 2.0f));
	BroadcastStatusText = CreateText(TEXT("방송 출력 준비 중"), 12, MutedTextColor);
	ControlPanel->AddChildToVerticalBox(BroadcastStatusText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 3.0f));
	UButton* BackgroundModeButton = CreateButton(
		ControlPanel,
		TEXT("배경 모드: 배경 제거"),
		BackgroundModeButtonText,
		PrimaryButtonColor);
	BackgroundModeButton->OnClicked.AddDynamic(
		this, &UVPTrackingDashboard::HandleBackgroundModeClicked);
	BackgroundColorUsageText = CreateText(
		TEXT("미리보기 배경색 · OBS에는 아바타만 송출"),
		11,
		MutedTextColor);
	ControlPanel->AddChildToVerticalBox(BackgroundColorUsageText)->SetPadding(
		FMargin(0.0f, 1.0f, 0.0f, 2.0f));

	UHorizontalBox* PresetRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ControlPanel->AddChildToVerticalBox(PresetRow)->SetPadding(FMargin(0.0f, 1.0f));
	UTextBlock* GreenLabel = nullptr;
	UButton* GreenButton = CreateInlineButton(PresetRow, TEXT("녹색"), FLinearColor(0.0f, 0.42f, 0.06f, 1.0f), GreenLabel);
	GreenButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleGreenBackgroundClicked);
	UTextBlock* BlueLabel = nullptr;
	UButton* BlueButton = CreateInlineButton(PresetRow, TEXT("파란색"), FLinearColor(0.0f, 0.16f, 0.62f, 1.0f), BlueLabel);
	BlueButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleBlueBackgroundClicked);
	UTextBlock* MagentaLabel = nullptr;
	UButton* MagentaButton = CreateInlineButton(PresetRow, TEXT("자홍색"), FLinearColor(0.58f, 0.0f, 0.46f, 1.0f), MagentaLabel);
	MagentaButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleMagentaBackgroundClicked);

	UHorizontalBox* ColorSummaryRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ControlPanel->AddChildToVerticalBox(ColorSummaryRow)->SetPadding(FMargin(2.0f, 1.0f, 2.0f, 2.0f));
	BackgroundColorSwatch = WidgetTree->ConstructWidget<UBorder>();
	BackgroundColorSwatch->SetBrushColor(FLinearColor::Green);
	UHorizontalBoxSlot* SwatchSlot = ColorSummaryRow->AddChildToHorizontalBox(BackgroundColorSwatch);
	SwatchSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
	SwatchSlot->SetPadding(FMargin(0.0f, 2.0f, 5.0f, 2.0f));
	BackgroundColorSwatch->SetPadding(FMargin(14.0f, 8.0f));
	BackgroundColorHexText = CreateText(TEXT("#00FF00"), 13, TextColor);
	BackgroundColorHexText->SetJustification(ETextJustify::Center);
	UHorizontalBoxSlot* HexTextSlot = ColorSummaryRow->AddChildToHorizontalBox(BackgroundColorHexText);
	HexTextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	HexTextSlot->SetVerticalAlignment(VAlign_Center);

	auto AddColorSliderRow = [this](
		const FString& ChannelName,
		const FLinearColor& ChannelColor,
		UTextBlock*& OutValueText) -> USlider*
	{
		UHorizontalBox* SliderRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		ControlPanel->AddChildToVerticalBox(SliderRow)->SetPadding(FMargin(2.0f, 1.0f));

		USizeBox* ChannelLabelSize = WidgetTree->ConstructWidget<USizeBox>();
		ChannelLabelSize->SetWidthOverride(24.0f);
		SliderRow->AddChildToHorizontalBox(ChannelLabelSize)->SetVerticalAlignment(VAlign_Center);
		UTextBlock* ChannelLabel = CreateText(ChannelName, 12, ChannelColor);
		ChannelLabelSize->AddChild(ChannelLabel);

		USlider* Slider = WidgetTree->ConstructWidget<USlider>();
		Slider->SetMinValue(0.0f);
		Slider->SetMaxValue(1.0f);
		Slider->SetSliderBarColor(ChannelColor.CopyWithNewOpacity(0.7f));
		Slider->SetSliderHandleColor(ChannelColor);
		UHorizontalBoxSlot* SliderSlot = SliderRow->AddChildToHorizontalBox(Slider);
		SliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SliderSlot->SetPadding(FMargin(3.0f, 0.0f, 7.0f, 0.0f));
		SliderSlot->SetVerticalAlignment(VAlign_Center);

		USizeBox* ValueSize = WidgetTree->ConstructWidget<USizeBox>();
		ValueSize->SetWidthOverride(34.0f);
		SliderRow->AddChildToHorizontalBox(ValueSize)->SetVerticalAlignment(VAlign_Center);
		OutValueText = CreateText(TEXT("0"), 11, TextColor);
		OutValueText->SetJustification(ETextJustify::Right);
		ValueSize->AddChild(OutValueText);
		return Slider;
	};

	BackgroundRedSlider = AddColorSliderRow(
		TEXT("R"), FLinearColor(1.0f, 0.2f, 0.2f, 1.0f), BackgroundRedValueText);
	BackgroundGreenSlider = AddColorSliderRow(
		TEXT("G"), FLinearColor(0.2f, 1.0f, 0.3f, 1.0f), BackgroundGreenValueText);
	BackgroundBlueSlider = AddColorSliderRow(
		TEXT("B"), FLinearColor(0.2f, 0.55f, 1.0f, 1.0f), BackgroundBlueValueText);
	BackgroundRedSlider->OnValueChanged.AddDynamic(
		this, &UVPTrackingDashboard::HandleBackgroundSliderChanged);
	BackgroundGreenSlider->OnValueChanged.AddDynamic(
		this, &UVPTrackingDashboard::HandleBackgroundSliderChanged);
	BackgroundBlueSlider->OnValueChanged.AddDynamic(
		this, &UVPTrackingDashboard::HandleBackgroundSliderChanged);

	UHorizontalBox* ExposureRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ControlPanel->AddChildToVerticalBox(ExposureRow)->SetPadding(FMargin(2.0f, 7.0f, 2.0f, 2.0f));
	USizeBox* ExposureLabelSize = WidgetTree->ConstructWidget<USizeBox>();
	ExposureLabelSize->SetWidthOverride(82.0f);
	ExposureRow->AddChildToHorizontalBox(ExposureLabelSize)->SetVerticalAlignment(VAlign_Center);
	ExposureLabelSize->AddChild(CreateText(TEXT("아바타 밝기"), 12, TextColor));
	AvatarExposureSlider = WidgetTree->ConstructWidget<USlider>();
	AvatarExposureSlider->SetMinValue(-2.0f);
	AvatarExposureSlider->SetMaxValue(2.0f);
	AvatarExposureSlider->SetStepSize(0.1f);
	AvatarExposureSlider->SetSliderBarColor(AccentColor.CopyWithNewOpacity(0.7f));
	AvatarExposureSlider->SetSliderHandleColor(AccentColor);
	UHorizontalBoxSlot* ExposureSliderSlot =
		ExposureRow->AddChildToHorizontalBox(AvatarExposureSlider);
	ExposureSliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ExposureSliderSlot->SetPadding(FMargin(3.0f, 0.0f, 7.0f, 0.0f));
	ExposureSliderSlot->SetVerticalAlignment(VAlign_Center);
	USizeBox* ExposureValueSize = WidgetTree->ConstructWidget<USizeBox>();
	ExposureValueSize->SetWidthOverride(58.0f);
	ExposureRow->AddChildToHorizontalBox(ExposureValueSize)->SetVerticalAlignment(VAlign_Center);
	AvatarExposureValueText = CreateText(TEXT("+0.0 EV"), 11, TextColor);
	AvatarExposureValueText->SetJustification(ETextJustify::Right);
	ExposureValueSize->AddChild(AvatarExposureValueText);
	AvatarExposureSlider->OnValueChanged.AddDynamic(
		this, &UVPTrackingDashboard::HandleAvatarExposureChanged);

	UHorizontalBox* OutputFPSRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ControlPanel->AddChildToVerticalBox(OutputFPSRow)->SetPadding(FMargin(2.0f, 7.0f, 2.0f, 1.0f));
	USizeBox* OutputFPSLabelSize = WidgetTree->ConstructWidget<USizeBox>();
	OutputFPSLabelSize->SetWidthOverride(82.0f);
	OutputFPSRow->AddChildToHorizontalBox(OutputFPSLabelSize)->SetVerticalAlignment(VAlign_Center);
	OutputFPSLabelSize->AddChild(CreateText(TEXT("송출 FPS"), 12, TextColor));
	OutputFPSSlider = WidgetTree->ConstructWidget<USlider>();
	OutputFPSSlider->SetMinValue(15.0f);
	OutputFPSSlider->SetMaxValue(144.0f);
	OutputFPSSlider->SetStepSize(1.0f);
	OutputFPSSlider->SetSliderBarColor(AccentColor.CopyWithNewOpacity(0.7f));
	OutputFPSSlider->SetSliderHandleColor(AccentColor);
	OutputFPSSlider->SetToolTipText(FText::FromString(
		TEXT("앱과 Spout의 FPS를 조절합니다. OBS가 출력 중이 아니면 OBS FPS도 함께 변경합니다.")));
	UHorizontalBoxSlot* OutputFPSSliderSlot =
		OutputFPSRow->AddChildToHorizontalBox(OutputFPSSlider);
	OutputFPSSliderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	OutputFPSSliderSlot->SetPadding(FMargin(3.0f, 0.0f, 7.0f, 0.0f));
	OutputFPSSliderSlot->SetVerticalAlignment(VAlign_Center);
	USizeBox* OutputFPSValueSize = WidgetTree->ConstructWidget<USizeBox>();
	OutputFPSValueSize->SetWidthOverride(58.0f);
	OutputFPSRow->AddChildToHorizontalBox(OutputFPSValueSize)->SetVerticalAlignment(VAlign_Center);
	OutputFPSValueText = CreateText(TEXT("60 FPS"), 11, TextColor);
	OutputFPSValueText->SetJustification(ETextJustify::Right);
	OutputFPSValueSize->AddChild(OutputFPSValueText);
	OutputFPSSlider->OnValueChanged.AddDynamic(
		this, &UVPTrackingDashboard::HandleOutputFPSChanged);
	ObsFPSStatusText = CreateText(TEXT("OBS FPS 확인 대기"), 11, MutedTextColor);
	ControlPanel->AddChildToVerticalBox(ObsFPSStatusText)->SetPadding(
		FMargin(82.0f, 0.0f, 0.0f, 3.0f));

	UTextBlock* CalibrationSection = CreateText(TEXT("1. 중립 자세 캘리브레이션"), 16, AccentColor);
	TrackingSettingsPanel->AddChildToVerticalBox(CalibrationSection)->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 2.0f));
	UTextBlock* CalibrationHelp = CreateText(
		TEXT("정면을 보고 어깨를 수평으로 맞추세요.\n양팔을 몸 옆에 내리고 팔꿈치까지 보여주세요."),
		12,
		MutedTextColor);
	TrackingSettingsPanel->AddChildToVerticalBox(CalibrationHelp)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 4.0f));
	UButton* CalibrationButton = CreateButton(
		TrackingSettingsPanel,
		TEXT("3초 캘리브레이션 시작 (C)"),
		CalibrationButtonText,
		PrimaryButtonColor);
	CalibrationButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleCalibrationClicked);
	AvatarRequiredControls.Add(CalibrationButton);
	CalibrationStatusText = CreateText(TEXT("활성 아바타 없음 · 캘리브레이션 비활성"), 12, MutedTextColor);
	TrackingSettingsPanel->AddChildToVerticalBox(CalibrationStatusText)->SetPadding(FMargin(0.0f, 3.0f, 0.0f, 6.0f));

	UTextBlock* MappingSection = CreateText(TEXT("2. 팔 매핑"), 16, AccentColor);
	TrackingSettingsPanel->AddChildToVerticalBox(MappingSection)->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 2.0f));
	UButton* SwapButton = CreateButton(
		TrackingSettingsPanel, TEXT("좌우 교환: 꺼짐"), SwapButtonText, SecondaryButtonColor);
	SwapButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleSwapClicked);
	AvatarRequiredControls.Add(SwapButton);
	UButton* LeftInvertButton = CreateButton(
		TrackingSettingsPanel, TEXT("왼팔 반전: 꺼짐"), LeftInvertButtonText, SecondaryButtonColor);
	LeftInvertButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleLeftInvertClicked);
	AvatarRequiredControls.Add(LeftInvertButton);
	UButton* RightInvertButton = CreateButton(
		TrackingSettingsPanel, TEXT("오른팔 반전: 꺼짐"), RightInvertButtonText, SecondaryButtonColor);
	RightInvertButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleRightInvertClicked);
	AvatarRequiredControls.Add(RightInvertButton);

	UTextBlock* ValidationSection = CreateText(TEXT("3. 단계별 팔 검증"), 16, AccentColor);
	TrackingSettingsPanel->AddChildToVerticalBox(ValidationSection)->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 2.0f));
	UButton* ValidationButton = CreateButton(
		TrackingSettingsPanel,
		TEXT("중립 → 왼팔 → 중립 → 오른팔"),
		ValidationButtonText,
		PrimaryButtonColor);
	ValidationButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleValidationClicked);
	AvatarRequiredControls.Add(ValidationButton);

	UTextBlock* ProfileSection = CreateText(TEXT("4. 프로필"), 16, AccentColor);
	TrackingSettingsPanel->AddChildToVerticalBox(ProfileSection)->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 2.0f));
	ProfileStatusText = CreateText(TEXT("활성 아바타 없음 · 프로필 기능 비활성"), 12, MutedTextColor);
	TrackingSettingsPanel->AddChildToVerticalBox(ProfileStatusText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 3.0f));
	UTextBlock* SaveLabel = nullptr;
	UButton* SaveButton = CreateButton(
		TrackingSettingsPanel, TEXT("현재 설정 저장"), SaveLabel, SecondaryButtonColor);
	SaveButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleSaveProfileClicked);
	AvatarRequiredControls.Add(SaveButton);
	UTextBlock* ResetLabel = nullptr;
	UButton* ResetButton = CreateButton(
		TrackingSettingsPanel, TEXT("프로필 기본값 복원"), ResetLabel, SecondaryButtonColor);
	ResetButton->OnClicked.AddDynamic(this, &UVPTrackingDashboard::HandleResetProfileClicked);
	AvatarRequiredControls.Add(ResetButton);

	DiagnosticsText = CreateText(TEXT("진단값 대기"), 12, MutedTextColor);
	TrackingSettingsPanel->AddChildToVerticalBox(DiagnosticsText)->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 0.0f));

	GuidancePanel = WidgetTree->ConstructWidget<UBorder>();
	GuidancePanel->SetBrushColor(RaisedPanelColor);
	GuidancePanel->SetPadding(FMargin(20.0f, 12.0f));
	GuidancePanel->SetVisibility(ESlateVisibility::Collapsed);
	UCanvasPanelSlot* TopSlot = Root->AddChildToCanvas(GuidancePanel);
	TopSlot->SetAnchors(FAnchors(0.36f, 0.0f, 0.98f, 0.0f));
	TopSlot->SetOffsets(FMargin(0.0f, 14.0f, 0.0f, 126.0f));

	UVerticalBox* GuideBox = WidgetTree->ConstructWidget<UVerticalBox>();
	GuidancePanel->SetContent(GuideBox);
	GuidanceText = CreateText(TEXT("1. 중립 자세 캘리브레이션"), 25, TextColor);
	GuidanceText->SetJustification(ETextJustify::Center);
	GuideBox->AddChildToVerticalBox(GuidanceText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 5.0f));
	StageProgressBar = WidgetTree->ConstructWidget<UProgressBar>();
	StageProgressBar->SetFillColorAndOpacity(AccentColor);
	StageProgressBar->SetPercent(0.0f);
	GuideBox->AddChildToVerticalBox(StageProgressBar)->SetPadding(FMargin(18.0f, 0.0f));

	UBorder* PoseBorder = WidgetTree->ConstructWidget<UBorder>();
	PoseBorder->SetBrushColor(RaisedPanelColor);
	PoseBorder->SetPadding(FMargin(12.0f, 9.0f));
	UCanvasPanelSlot* PoseSlot = Root->AddChildToCanvas(PoseBorder);
	PoseSlot->SetAnchors(FAnchors(1.0f, 0.0f, 1.0f, 0.0f));
	PoseSlot->SetOffsets(FMargin(-330.0f, 160.0f, 318.0f, 250.0f));
	UVerticalBox* PoseBox = WidgetTree->ConstructWidget<UVerticalBox>();
	PoseBorder->SetContent(PoseBox);
	PoseStatusText = CreateText(TEXT("원시 POSE\n인식 대기"), 14, MutedTextColor);
	PoseStatusText->SetJustification(ETextJustify::Center);
	PoseBox->AddChildToVerticalBox(PoseStatusText);
	USpacer* PoseDrawingSpace = WidgetTree->ConstructWidget<USpacer>();
	PoseBox->AddChildToVerticalBox(PoseDrawingSpace)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	UBorder* BottomBorder = WidgetTree->ConstructWidget<UBorder>();
	BottomBorder->SetBrushColor(PanelColor);
	BottomBorder->SetPadding(FMargin(14.0f, 8.0f));
	UCanvasPanelSlot* BottomSlot = Root->AddChildToCanvas(BottomBorder);
	BottomSlot->SetAnchors(FAnchors(0.0f, 1.0f, 1.0f, 1.0f));
	BottomSlot->SetOffsets(FMargin(0.0f, -46.0f, 0.0f, 46.0f));
	TrackingStatusText = CreateText(TEXT("FACE 대기 · POSE 대기 · UDP 대기"), 14, TextColor);
	TrackingStatusText->SetJustification(ETextJustify::Center);
	BottomBorder->SetContent(TrackingStatusText);

	ModalOverlay = WidgetTree->ConstructWidget<UBorder>();
	ModalOverlay->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.72f));
	ModalOverlay->SetHorizontalAlignment(HAlign_Center);
	ModalOverlay->SetVerticalAlignment(VAlign_Center);
	UCanvasPanelSlot* ModalSlot = Root->AddChildToCanvas(ModalOverlay);
	ModalSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	ModalSlot->SetOffsets(FMargin(0.0f));
	ModalSlot->SetZOrder(100);

	USizeBox* ModalSize = WidgetTree->ConstructWidget<USizeBox>();
	ModalSize->SetWidthOverride(560.0f);
	ModalOverlay->SetContent(ModalSize);
	UBorder* ModalPanel = WidgetTree->ConstructWidget<UBorder>();
	ModalPanel->SetBrushColor(RaisedPanelColor);
	ModalPanel->SetPadding(FMargin(28.0f, 24.0f));
	ModalSize->AddChild(ModalPanel);
	UVerticalBox* ModalBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ModalPanel->SetContent(ModalBox);

	ModalTitleText = CreateText(TEXT("안내"), 24, AccentColor);
	ModalTitleText->SetJustification(ETextJustify::Center);
	ModalBox->AddChildToVerticalBox(ModalTitleText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	ModalMessageText = CreateText(FString(), 15, TextColor);
	ModalMessageText->SetJustification(ETextJustify::Center);
	ModalBox->AddChildToVerticalBox(ModalMessageText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 18.0f));

	UHorizontalBox* ModalButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	ModalBox->AddChildToVerticalBox(ModalButtonRow);
	UTextBlock* ModalPrimaryLabel = nullptr;
	ModalPrimaryButton = CreateInlineButton(
		ModalButtonRow,
		TEXT("확인"),
		PrimaryButtonColor,
		ModalPrimaryLabel);
	ModalPrimaryButtonText = ModalPrimaryLabel;
	ModalPrimaryButton->OnClicked.AddDynamic(
		this,
		&UVPTrackingDashboard::HandleModalPrimaryClicked);
	UTextBlock* ModalSecondaryButtonText = nullptr;
	ModalSecondaryButton = CreateInlineButton(
		ModalButtonRow,
		TEXT("취소"),
		SecondaryButtonColor,
		ModalSecondaryButtonText);
	ModalSecondaryButton->OnClicked.AddDynamic(
		this,
		&UVPTrackingDashboard::HandleModalSecondaryClicked);
	ModalOverlay->SetVisibility(ESlateVisibility::Collapsed);
}

void UVPTrackingDashboard::FindTrackingTargets()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!BroadcastOutput)
		{
			BroadcastOutput = Cast<AVPBroadcastOutput>(Actor);
		}
		if (!Receiver)
		{
			Receiver = Actor->FindComponentByClass<UVPUDPReceiver>();
		}

		if (Receiver && BroadcastOutput)
		{
			break;
		}
	}
	AnimInstance = AvatarManager ? AvatarManager->GetActiveAnimInstance() : nullptr;
	SetDashboardEnabled(AnimInstance != nullptr);
}

void UVPTrackingDashboard::SetDashboardEnabled(bool bEnabled)
{
	for (UWidget* Control : AvatarRequiredControls)
	{
		if (Control)
		{
			Control->SetIsEnabled(bEnabled);
		}
	}
}

void UVPTrackingDashboard::SetTrackingSettingsPaletteActive(bool bActive)
{
	bTrackingSettingsPaletteActive = bActive;
	if (PaletteSwitcher)
	{
		PaletteSwitcher->SetActiveWidgetIndex(bActive ? 1 : 0);
	}
	if (BroadcastPaletteButton)
	{
		BroadcastPaletteButton->SetBackgroundColor(
			bActive ? SecondaryButtonColor : PrimaryButtonColor);
	}
	if (TrackingSettingsPaletteButton)
	{
		TrackingSettingsPaletteButton->SetBackgroundColor(
			bActive ? PrimaryButtonColor : SecondaryButtonColor);
	}
}

void UVPTrackingDashboard::SetAvatarManager(AVPAvatarManager* InAvatarManager)
{
	AvatarManager = InAvatarManager;
	AnimInstance = AvatarManager ? AvatarManager->GetActiveAnimInstance() : nullptr;
	ObservedAvatarLibraryRevision = INDEX_NONE;
	ObservedAvatarNoticeRevision = 0;
	RefreshAvatarSelector();
	SetDashboardEnabled(AnimInstance != nullptr);
}

bool UVPTrackingDashboard::IsModalOpen() const
{
	return ModalOverlay && ModalOverlay->GetVisibility() != ESlateVisibility::Collapsed;
}

bool UVPTrackingDashboard::CanUseCalibrationShortcut() const
{
	return bTrackingSettingsPaletteActive &&
		IsVisible() &&
		!IsModalOpen();
}

void UVPTrackingDashboard::RefreshAvatarSelector()
{
	if (!AvatarSelector || !AvatarManager)
	{
		return;
	}

	AvatarOptionToId.Reset();
	AvatarSelector->ClearOptions();
	FString SelectedOption;
	for (const FString& AvatarId : AvatarManager->GetAvatarIds())
	{
		const FString Option = FString::Printf(
			TEXT("%s · %s"),
			*AvatarManager->GetAvatarDisplayName(AvatarId),
			*AvatarId.Left(8));
		AvatarOptionToId.Add(Option, AvatarId);
		AvatarSelector->AddOption(Option);
		if (AvatarId == AvatarManager->GetActiveAvatarId())
		{
			SelectedOption = Option;
		}
	}

	if (SelectedOption.IsEmpty())
	{
		AvatarSelector->ClearSelection();
	}
	else
	{
		AvatarSelector->SetSelectedOption(SelectedOption);
	}
	ObservedAvatarLibraryRevision = AvatarManager->GetLibraryRevision();
}

FText UVPTrackingDashboard::BuildGuidanceText(
	EVPCalibrationState CalibrationState,
	EVPArmValidationStage ValidationStage,
	const FString& CalibrationStatus,
	const FString& ValidationStatus)
{
	if (IsCalibrationActive(CalibrationState) ||
		CalibrationState == EVPCalibrationState::Failed)
	{
		return FText::FromString(CalibrationStatus);
	}
	if (IsValidationActive(ValidationStage) ||
		ValidationStage == EVPArmValidationStage::Passed ||
		ValidationStage == EVPArmValidationStage::Failed)
	{
		return FText::FromString(ValidationStatus);
	}
	if (CalibrationState == EVPCalibrationState::Succeeded)
	{
		return FText::FromString(TEXT("2. 단계별 팔 검증을 시작하세요."));
	}
	return FText::FromString(TEXT("1. 중립 자세 캘리브레이션을 시작하세요."));
}

FText UVPTrackingDashboard::BuildDeleteAvatarConfirmationText(const FString& DisplayName)
{
	return FText::FromString(FString::Printf(
		TEXT("'%s' 아바타를 삭제하시겠습니까?\n\n")
		TEXT("• 보관함의 관리 복사본\n")
		TEXT("• 카메라 위치와 FOV\n")
		TEXT("• 캘리브레이션과 매핑 설정\n\n")
		TEXT("위 항목은 항상 함께 삭제됩니다.\n")
		TEXT("사용자가 가져온 원본 VRM 파일은 유지됩니다."),
		*DisplayName));
}

bool UVPTrackingDashboard::ShouldShowGuidancePanel(
	EVPCalibrationState CalibrationState,
	EVPArmValidationStage ValidationStage,
	float SuccessDisplayRemainingSeconds,
	float CalibrationFailureDisplayRemainingSeconds)
{
	return IsCalibrationActive(CalibrationState) ||
		(CalibrationState == EVPCalibrationState::Failed &&
			CalibrationFailureDisplayRemainingSeconds > 0.0f) ||
		IsValidationActive(ValidationStage) ||
		ValidationStage == EVPArmValidationStage::Failed ||
		SuccessDisplayRemainingSeconds > 0.0f;
}

FText UVPTrackingDashboard::BuildTrackingStatusText(
	bool bInPacketRateReady,
	float PacketsPerSecond,
	float InSecondsSinceLastPacket,
	bool bFaceTracked,
	bool bPoseTracked,
	bool bLeftArmTracked,
	bool bRightArmTracked)
{
	FString DataStatus = TEXT("데이터 연결 중");
	if (bInPacketRateReady)
	{
		if (InSecondsSinceLastPacket >= PacketSilenceTimeoutSeconds)
		{
			DataStatus = TEXT("데이터 끊김");
		}
		else
		{
			const int32 RoundedRate = FMath::Max(0, FMath::RoundToInt(PacketsPerSecond));
			DataStatus = PacketsPerSecond >= DelayedPacketRateThreshold
				? FString::Printf(TEXT("데이터 정상 %dfps"), RoundedRate)
				: FString::Printf(TEXT("데이터 지연 %dfps"), RoundedRate);
		}
	}

	return FText::FromString(FString::Printf(
		TEXT("%s · 얼굴 %s · 포즈 %s · 왼팔 %s · 오른팔 %s"),
		*DataStatus,
		bFaceTracked ? TEXT("추적") : TEXT("놓침"),
		bPoseTracked ? TEXT("추적") : TEXT("놓침"),
		bLeftArmTracked ? TEXT("추적") : TEXT("중립"),
		bRightArmTracked ? TEXT("추적") : TEXT("중립")));
}

void UVPTrackingDashboard::UpdatePacketRate(float DeltaSeconds)
{
	if (!Receiver)
	{
		PacketRateWindowElapsedSeconds = 0.0f;
		SecondsSinceLastPacket = 0.0f;
		DisplayedPacketRate = 0.0f;
		LastObservedPacketCount = INDEX_NONE;
		PacketRateWindowStartCount = 0;
		bPacketRateReady = false;
		return;
	}

	const int32 CurrentPacketCount = Receiver->GetPacketCount();
	if (LastObservedPacketCount == INDEX_NONE || CurrentPacketCount < LastObservedPacketCount)
	{
		LastObservedPacketCount = CurrentPacketCount;
		PacketRateWindowStartCount = CurrentPacketCount;
		PacketRateWindowElapsedSeconds = 0.0f;
		SecondsSinceLastPacket = 0.0f;
		DisplayedPacketRate = 0.0f;
		bPacketRateReady = false;
		return;
	}

	const float SafeDeltaSeconds = FMath::Max(0.0f, DeltaSeconds);
	SecondsSinceLastPacket = CurrentPacketCount > LastObservedPacketCount
		? 0.0f
		: SecondsSinceLastPacket + SafeDeltaSeconds;
	PacketRateWindowElapsedSeconds += SafeDeltaSeconds;

	if (PacketRateWindowElapsedSeconds >= PacketRateSampleSeconds)
	{
		DisplayedPacketRate = static_cast<float>(CurrentPacketCount - PacketRateWindowStartCount) /
			PacketRateWindowElapsedSeconds;
		PacketRateWindowStartCount = CurrentPacketCount;
		PacketRateWindowElapsedSeconds = 0.0f;
		bPacketRateReady = true;
	}

	LastObservedPacketCount = CurrentPacketCount;
}

void UVPTrackingDashboard::UpdateGuidanceVisibility(float DeltaSeconds)
{
	if (!GuidancePanel || !AnimInstance)
	{
		GuidanceSuccessRemainingSeconds = 0.0f;
		CalibrationFailureDisplayRemainingSeconds = 0.0f;
		PreviousCalibrationState = EVPCalibrationState::Idle;
		PreviousValidationStage = EVPArmValidationStage::Idle;
		if (GuidancePanel)
		{
			GuidancePanel->SetVisibility(ESlateVisibility::Collapsed);
		}
		return;
	}

	const EVPCalibrationState CalibrationState = AnimInstance->CalibrationState;
	const EVPArmValidationStage ValidationStage = AnimInstance->ArmValidationStage;
	if ((CalibrationState == EVPCalibrationState::Succeeded &&
		PreviousCalibrationState != EVPCalibrationState::Succeeded) ||
		(ValidationStage == EVPArmValidationStage::Passed &&
		PreviousValidationStage != EVPArmValidationStage::Passed))
	{
		GuidanceSuccessRemainingSeconds = GuidanceSuccessDisplaySeconds;
	}
	else
	{
		GuidanceSuccessRemainingSeconds = FMath::Max(
			0.0f,
			GuidanceSuccessRemainingSeconds - FMath::Max(0.0f, DeltaSeconds));
	}
	if (CalibrationState == EVPCalibrationState::Failed)
	{
		CalibrationFailureDisplayRemainingSeconds =
			PreviousCalibrationState != EVPCalibrationState::Failed
				? CalibrationFailureDisplaySeconds
				: FMath::Max(
					0.0f,
					CalibrationFailureDisplayRemainingSeconds - FMath::Max(0.0f, DeltaSeconds));
	}
	else
	{
		CalibrationFailureDisplayRemainingSeconds = 0.0f;
	}

	GuidancePanel->SetVisibility(ShouldShowGuidancePanel(
		CalibrationState,
		ValidationStage,
		GuidanceSuccessRemainingSeconds,
		CalibrationFailureDisplayRemainingSeconds)
		? ESlateVisibility::HitTestInvisible
		: ESlateVisibility::Collapsed);
	PreviousCalibrationState = CalibrationState;
	PreviousValidationStage = ValidationStage;
}

void UVPTrackingDashboard::UpdateBackgroundColorCommit(float DeltaSeconds)
{
	if (!bBackgroundColorCommitPending)
	{
		return;
	}

	BackgroundColorCommitRemainingSeconds -= FMath::Max(0.0f, DeltaSeconds);
	if (BackgroundColorCommitRemainingSeconds <= 0.0f)
	{
		bBackgroundColorCommitPending = false;
		if (BroadcastOutput)
		{
			BroadcastOutput->CommitBackgroundColor();
		}
	}
}

void UVPTrackingDashboard::UpdateAvatarExposureCommit(float DeltaSeconds)
{
	if (!bAvatarExposureCommitPending)
	{
		return;
	}

	AvatarExposureCommitRemainingSeconds -= FMath::Max(0.0f, DeltaSeconds);
	if (AvatarExposureCommitRemainingSeconds <= 0.0f)
	{
		bAvatarExposureCommitPending = false;
		if (BroadcastOutput)
		{
			BroadcastOutput->CommitAvatarExposure();
		}
	}
}

void UVPTrackingDashboard::UpdateOutputFPSCommit(float DeltaSeconds)
{
	if (!bOutputFPSCommitPending)
	{
		return;
	}

	OutputFPSCommitRemainingSeconds -= FMath::Max(0.0f, DeltaSeconds);
	if (OutputFPSCommitRemainingSeconds <= 0.0f)
	{
		bOutputFPSCommitPending = false;
		if (BroadcastOutput)
		{
			BroadcastOutput->CommitOutputFPS();
		}
	}
}

void UVPTrackingDashboard::RefreshBackgroundColorControls(const FLinearColor& LinearColor)
{
	if (!BackgroundRedSlider || !BackgroundGreenSlider || !BackgroundBlueSlider ||
		!BackgroundRedValueText || !BackgroundGreenValueText || !BackgroundBlueValueText ||
		!BackgroundColorHexText)
	{
		return;
	}

	const FColor SrgbColor = LinearColor.ToFColorSRGB();
	bUpdatingBackgroundColorControls = true;
	BackgroundRedSlider->SetValue(static_cast<float>(SrgbColor.R) / 255.0f);
	BackgroundGreenSlider->SetValue(static_cast<float>(SrgbColor.G) / 255.0f);
	BackgroundBlueSlider->SetValue(static_cast<float>(SrgbColor.B) / 255.0f);
	BackgroundRedValueText->SetText(FText::AsNumber(SrgbColor.R));
	BackgroundGreenValueText->SetText(FText::AsNumber(SrgbColor.G));
	BackgroundBlueValueText->SetText(FText::AsNumber(SrgbColor.B));
	BackgroundColorHexText->SetText(FText::FromString(TEXT("#") + SrgbColor.ToHex().Left(6)));
	bUpdatingBackgroundColorControls = false;
}

void UVPTrackingDashboard::RefreshAvatarExposureControl(float ExposureStops)
{
	if (!AvatarExposureSlider || !AvatarExposureValueText)
	{
		return;
	}

	const float Sanitized = AVPBroadcastOutput::SanitizeAvatarExposure(ExposureStops);
	bUpdatingAvatarExposureControl = true;
	AvatarExposureSlider->SetValue(Sanitized);
	AvatarExposureValueText->SetText(FText::FromString(
		FString::Printf(TEXT("%+.1f EV"), Sanitized)));
	bUpdatingAvatarExposureControl = false;
}

void UVPTrackingDashboard::RefreshOutputFPSControl(int32 FramesPerSecond)
{
	if (!OutputFPSSlider || !OutputFPSValueText)
	{
		return;
	}

	const int32 Sanitized = AVPBroadcastOutput::SanitizeOutputFPS(FramesPerSecond);
	bUpdatingOutputFPSControl = true;
	OutputFPSSlider->SetValue(static_cast<float>(Sanitized));
	OutputFPSValueText->SetText(FText::FromString(
		FString::Printf(TEXT("%d FPS"), Sanitized)));
	bUpdatingOutputFPSControl = false;
}

void UVPTrackingDashboard::ShowNoticeModal(
	const FString& Message,
	bool bIsError,
	const FString& TitleOverride)
{
	if (!ModalOverlay || !ModalTitleText || !ModalMessageText ||
		!ModalPrimaryButton || !ModalPrimaryButtonText || !ModalSecondaryButton)
	{
		return;
	}

	bDeleteConfirmationModal = false;
	ModalTitleText->SetText(FText::FromString(
		TitleOverride.IsEmpty() ? (bIsError ? TEXT("오류") : TEXT("안내")) : TitleOverride));
	ModalTitleText->SetColorAndOpacity(FSlateColor(
		bIsError ? TrackingLostColor : (TitleOverride.IsEmpty() ? AccentColor : TrackingWeakColor)));
	ModalMessageText->SetText(FText::FromString(Message));
	ModalPrimaryButtonText->SetText(FText::FromString(TEXT("확인")));
	ModalPrimaryButton->SetBackgroundColor(PrimaryButtonColor);
	ModalSecondaryButton->SetVisibility(ESlateVisibility::Collapsed);
	ModalOverlay->SetVisibility(ESlateVisibility::Visible);
}

void UVPTrackingDashboard::ShowDeleteConfirmationModal()
{
	if (!AvatarManager || AvatarManager->GetActiveAvatarId().IsEmpty())
	{
		ShowNoticeModal(TEXT("삭제할 아바타가 없습니다."), false);
		return;
	}
	if (!ModalOverlay || !ModalTitleText || !ModalMessageText ||
		!ModalPrimaryButton || !ModalPrimaryButtonText || !ModalSecondaryButton)
	{
		return;
	}

	bDeleteConfirmationModal = true;
	ModalTitleText->SetText(FText::FromString(TEXT("아바타 삭제")));
	ModalTitleText->SetColorAndOpacity(FSlateColor(TrackingLostColor));
	ModalMessageText->SetText(BuildDeleteAvatarConfirmationText(
		AvatarManager->GetActiveAvatarDisplayName()));
	ModalPrimaryButtonText->SetText(FText::FromString(TEXT("삭제")));
	ModalPrimaryButton->SetBackgroundColor(FLinearColor(0.62f, 0.08f, 0.10f, 1.0f));
	ModalSecondaryButton->SetVisibility(ESlateVisibility::Visible);
	ModalOverlay->SetVisibility(ESlateVisibility::Visible);
}

void UVPTrackingDashboard::HideModal()
{
	bDeleteConfirmationModal = false;
	if (ModalOverlay)
	{
		ModalOverlay->SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UVPTrackingDashboard::RefreshDashboard()
{
	if (!GuidanceText || !TrackingStatusText || !DiagnosticsText ||
		!ProfileStatusText || !CalibrationStatusText)
	{
		return;
	}

	if (AvatarStatusText)
	{
		AvatarStatusText->SetText(FText::FromString(
			AvatarManager
				? AvatarManager->GetStatusText()
				: TEXT("아바타 관리 기능 연결 대기")));
		AvatarStatusText->SetColorAndOpacity(FSlateColor(
			AvatarManager && AvatarManager->GetActiveAvatarId().IsEmpty()
				? MutedTextColor
				: TextColor));
	}

	if (BroadcastOutput && BroadcastStatusText && BackgroundModeButtonText)
	{
		const bool bBackgroundRemoved =
			BroadcastOutput->GetBackgroundMode() == EVPBroadcastBackgroundMode::BackgroundRemoved;
		const bool bAvatarReady = BroadcastOutput->IsAvatarCaptureReady();
		FString BroadcastStatus;
		if (bBackgroundRemoved)
		{
			BroadcastStatus = bAvatarReady
				? TEXT("송출 화면 · 아바타만 (배경 제거)")
				: TEXT("송출 화면 · 투명");
		}
		else
		{
			BroadcastStatus = bAvatarReady
				? TEXT("송출 화면 · 아바타 + 단색 배경")
				: TEXT("송출 화면 · 단색 배경");
		}
		BroadcastStatusText->SetText(FText::FromString(BroadcastStatus));
		BroadcastStatusText->SetColorAndOpacity(FSlateColor(MutedTextColor));
		BackgroundModeButtonText->SetText(FText::FromString(FString::Printf(
			TEXT("배경 모드: %s"),
			*BroadcastOutput->GetBackgroundModeLabel())));
		if (BackgroundColorUsageText)
		{
			BackgroundColorUsageText->SetText(FText::FromString(
				bBackgroundRemoved
					? TEXT("미리보기 배경색 · OBS에는 아바타만 송출")
					: TEXT("송출 배경색 · OBS에 아바타와 함께 송출")));
		}
		if (BackgroundColorSwatch)
		{
			BackgroundColorSwatch->SetBrushColor(BroadcastOutput->GetBackgroundColor());
		}
		RefreshBackgroundColorControls(BroadcastOutput->GetBackgroundColor());
		RefreshAvatarExposureControl(BroadcastOutput->GetAvatarExposureStops());
		RefreshOutputFPSControl(BroadcastOutput->GetOutputFPS());
		if (ObsFPSStatusText)
		{
			ObsFPSStatusText->SetText(FText::FromString(BroadcastOutput->GetObsFPSStatusText()));
		}
	}
	else if (BroadcastStatusText)
	{
		BroadcastStatusText->SetText(FText::FromString(TEXT("방송 출력 연결 대기")));
	}

	if (PoseStatusText)
	{
		const FVPTrackingFrame PoseFrame = Receiver
			? Receiver->GetLatestTrackingData()
			: FVPTrackingFrame();
		if (PoseFrame.bPoseTracked && PoseFrame.PoseLandmarks.Num() >= 15)
		{
			float WeakestConfidence = 0.0f;
			PoseStatusText->SetText(BuildPoseStatusText(PoseFrame, WeakestConfidence));
			PoseStatusText->SetColorAndOpacity(FSlateColor(ConfidenceColor(WeakestConfidence)));
		}
		else
		{
			PoseStatusText->SetText(FText::FromString(TEXT("원시 POSE\n인식되지 않음")));
			PoseStatusText->SetColorAndOpacity(FSlateColor(TrackingLostColor));
		}
	}

	const FString TransportDiagnostics = BuildTransportDiagnostics(
		Receiver,
		bPacketRateReady,
		DisplayedPacketRate);

	if (!AnimInstance)
	{
		SetDashboardEnabled(false);
		GuidanceText->SetText(FText::FromString(TEXT("VRM 아바타를 추가하거나 목록에서 선택하세요.")));
		TrackingStatusText->SetText(FText::FromString(TEXT("아바타 없음 · 트래킹과 캘리브레이션 비활성")));
		if (bTrackingSettingsPaletteActive)
		{
			DiagnosticsText->SetText(FText::FromString(BroadcastOutput
				? FString::Printf(
					TEXT("아바타가 없어도 선택한 배경색은 계속 송출됩니다.\n")
					TEXT("%s\n")
					TEXT("Spout 진단 · %s · 송신자 %s"),
					*TransportDiagnostics,
					BroadcastOutput->WasSpoutStartRequested() ? TEXT("시작 요청됨") : TEXT("대기"),
					*BroadcastOutput->GetSenderName())
				: FString::Printf(
					TEXT("%s\nSpout 진단 · 방송 출력 연결 대기"),
					*TransportDiagnostics)));
		}
		ProfileStatusText->SetText(FText::FromString(TEXT("활성 아바타 없음 · 프로필 기능 비활성")));
		CalibrationStatusText->SetText(FText::FromString(
			TEXT("활성 아바타 없음 · 캘리브레이션 비활성")));
		StageProgressBar->SetPercent(0.0f);
		return;
	}
	SetDashboardEnabled(true);

	GuidanceText->SetText(BuildGuidanceText(
		AnimInstance->CalibrationState,
		AnimInstance->ArmValidationStage,
		AnimInstance->CalibrationStatus,
		AnimInstance->ArmValidationStatus));

	float Progress = AnimInstance->ArmValidationProgress;
	if (IsCalibrationActive(AnimInstance->CalibrationState))
	{
		Progress = AnimInstance->CalibrationState == EVPCalibrationState::CountingDown
			? 1.0f - AnimInstance->CalibrationRemainingSeconds /
				FMath::Max(0.1f, AnimInstance->CalibrationCountdownSeconds)
			: 0.0f;
	}
	else if (AnimInstance->CalibrationState == EVPCalibrationState::Succeeded &&
		AnimInstance->ArmValidationStage == EVPArmValidationStage::Idle)
	{
		Progress = 1.0f;
	}
	StageProgressBar->SetPercent(FMath::Clamp(Progress, 0.0f, 1.0f));

	const FVPTrackingFrame Frame = Receiver
		? Receiver->GetLatestTrackingData()
		: AnimInstance->TrackingData;
	const int32 Packets = Receiver ? Receiver->GetPacketCount() : 0;
	const int32 ProtocolMismatches = Receiver ? Receiver->GetProtocolMismatchCount() : 0;
	TrackingStatusText->SetText(ProtocolMismatches > 0 && Packets == 0
		? FText::FromString(TEXT("트래커와 앱 버전이 맞지 않습니다."))
		: BuildTrackingStatusText(
			bPacketRateReady,
			DisplayedPacketRate,
			SecondsSinceLastPacket,
			Frame.bFaceTracked,
			Frame.bPoseTracked,
			AnimInstance->bLeftUpperArmTracked,
			AnimInstance->bRightUpperArmTracked));

	if (bTrackingSettingsPaletteActive)
	{
		DiagnosticsText->SetText(FText::FromString(FString::Printf(
			TEXT("왼팔  입력 %6.1f°  출력 %6.1f°  신뢰도 %.2f\n")
			TEXT("오른팔 입력 %6.1f°  출력 %6.1f°  신뢰도 %.2f\n")
			TEXT("필터 이상치 거부 %d회\n")
			TEXT("%s\n")
			TEXT("Spout 진단 · %s · 송신자 %s"),
			AnimInstance->LeftUpperArmInputAngle,
			AnimInstance->LeftUpperArmOutputAngle,
			AnimInstance->LeftUpperArmConfidence,
			AnimInstance->RightUpperArmInputAngle,
			AnimInstance->RightUpperArmOutputAngle,
			AnimInstance->RightUpperArmConfidence,
			AnimInstance->RejectedUpperArmInputCount,
			*TransportDiagnostics,
			BroadcastOutput && BroadcastOutput->WasSpoutStartRequested()
				? TEXT("시작 요청됨")
				: TEXT("대기"),
			BroadcastOutput ? *BroadcastOutput->GetSenderName() : TEXT("연결 대기"))));
	}

	ProfileStatusText->SetText(FText::FromString(FString::Printf(
		TEXT("아바타 프로필 · 캘리브레이션 %s"),
		AnimInstance->bHasNeutralCalibration ? TEXT("완료") : TEXT("필요"))));
	CalibrationStatusText->SetText(FText::FromString(
		AnimInstance->CalibrationState == EVPCalibrationState::Failed
			? FString(TEXT("캘리브레이션 실패 · C로 다시 시작하세요."))
			: FString::Printf(
				TEXT("캘리브레이션: %s"),
				AnimInstance->bHasNeutralCalibration ? TEXT("완료") : TEXT("필요"))));
	CalibrationButtonText->SetText(FText::FromString(
		IsCalibrationActive(AnimInstance->CalibrationState)
			? TEXT("캘리브레이션 취소 (C)")
			: TEXT("3초 캘리브레이션 시작 (C)")));
	ValidationButtonText->SetText(FText::FromString(
		IsValidationActive(AnimInstance->ArmValidationStage)
			? TEXT("단계별 검증 취소")
			: TEXT("중립 → 왼팔 → 중립 → 오른팔")));
	SwapButtonText->SetText(FText::FromString(FString::Printf(
		TEXT("좌우 교환: %s"), AnimInstance->bSwapUpperArms ? TEXT("켜짐") : TEXT("꺼짐"))));
	LeftInvertButtonText->SetText(FText::FromString(FString::Printf(
		TEXT("왼팔 반전: %s"), AnimInstance->bInvertLeftUpperArm ? TEXT("켜짐") : TEXT("꺼짐"))));
	RightInvertButtonText->SetText(FText::FromString(FString::Printf(
		TEXT("오른팔 반전: %s"), AnimInstance->bInvertRightUpperArm ? TEXT("켜짐") : TEXT("꺼짐"))));
}

void UVPTrackingDashboard::HandleCalibrationClicked()
{
	ToggleNeutralCalibration();
}

void UVPTrackingDashboard::HandleBroadcastPaletteClicked()
{
	SetTrackingSettingsPaletteActive(false);
}

void UVPTrackingDashboard::HandleTrackingSettingsPaletteClicked()
{
	SetTrackingSettingsPaletteActive(true);
}

void UVPTrackingDashboard::ToggleNeutralCalibration()
{
	AnimInstance = AvatarManager ? AvatarManager->GetActiveAnimInstance() : nullptr;
	if (!AnimInstance || IsModalOpen()) return;
	if (IsCalibrationActive(AnimInstance->CalibrationState))
		AnimInstance->CancelNeutralCalibration();
	else
		AnimInstance->StartNeutralCalibration();
}

void UVPTrackingDashboard::HandleValidationClicked()
{
	if (!AnimInstance) return;
	if (IsValidationActive(AnimInstance->ArmValidationStage))
		AnimInstance->CancelArmValidation();
	else
		AnimInstance->StartArmValidation();
}

void UVPTrackingDashboard::HandleSaveProfileClicked()
{
	if (!AnimInstance) return;
	const bool bSaved = AnimInstance->SaveTrackingProfile();
	ProfileStatusText->SetText(FText::FromString(
		bSaved ? TEXT("아바타 프로필을 저장했습니다.") : TEXT("프로필 저장에 실패했습니다.")));
}

void UVPTrackingDashboard::HandleResetProfileClicked()
{
	if (!AnimInstance) return;
	AnimInstance->ResetTrackingProfile();
}

void UVPTrackingDashboard::HandleSwapClicked()
{
	if (!AnimInstance) return;
	AnimInstance->SetUpperArmMappingOptions(
		!AnimInstance->bSwapUpperArms,
		AnimInstance->bInvertLeftUpperArm,
		AnimInstance->bInvertRightUpperArm);
}

void UVPTrackingDashboard::HandleLeftInvertClicked()
{
	if (!AnimInstance) return;
	AnimInstance->SetUpperArmMappingOptions(
		AnimInstance->bSwapUpperArms,
		!AnimInstance->bInvertLeftUpperArm,
		AnimInstance->bInvertRightUpperArm);
}

void UVPTrackingDashboard::HandleRightInvertClicked()
{
	if (!AnimInstance) return;
	AnimInstance->SetUpperArmMappingOptions(
		AnimInstance->bSwapUpperArms,
		AnimInstance->bInvertLeftUpperArm,
		!AnimInstance->bInvertRightUpperArm);
}

void UVPTrackingDashboard::HandleBackgroundModeClicked()
{
	if (!BroadcastOutput) return;
	BroadcastOutput->SetBackgroundMode(
		BroadcastOutput->GetBackgroundMode() == EVPBroadcastBackgroundMode::BackgroundRemoved
			? EVPBroadcastBackgroundMode::SolidColor
			: EVPBroadcastBackgroundMode::BackgroundRemoved);
}

void UVPTrackingDashboard::HandleGreenBackgroundClicked()
{
	if (BroadcastOutput)
	{
		bBackgroundColorCommitPending = false;
		BroadcastOutput->SetBackgroundColorHex(TEXT("00FF00"));
	}
}

void UVPTrackingDashboard::HandleBlueBackgroundClicked()
{
	if (BroadcastOutput)
	{
		bBackgroundColorCommitPending = false;
		BroadcastOutput->SetBackgroundColorHex(TEXT("0099FF"));
	}
}

void UVPTrackingDashboard::HandleMagentaBackgroundClicked()
{
	if (BroadcastOutput)
	{
		bBackgroundColorCommitPending = false;
		BroadcastOutput->SetBackgroundColorHex(TEXT("FF00FF"));
	}
}

void UVPTrackingDashboard::HandleBackgroundSliderChanged(float Value)
{
	(void)Value;
	if (bUpdatingBackgroundColorControls || !BroadcastOutput ||
		!BackgroundRedSlider || !BackgroundGreenSlider || !BackgroundBlueSlider)
	{
		return;
	}

	const uint8 Red = static_cast<uint8>(FMath::Clamp(
		FMath::RoundToInt(BackgroundRedSlider->GetValue() * 255.0f), 0, 255));
	const uint8 Green = static_cast<uint8>(FMath::Clamp(
		FMath::RoundToInt(BackgroundGreenSlider->GetValue() * 255.0f), 0, 255));
	const uint8 Blue = static_cast<uint8>(FMath::Clamp(
		FMath::RoundToInt(BackgroundBlueSlider->GetValue() * 255.0f), 0, 255));
	BroadcastOutput->PreviewBackgroundColor(AVPBroadcastOutput::FromSrgb8(Red, Green, Blue));
	RefreshBackgroundColorControls(BroadcastOutput->GetBackgroundColor());
	bBackgroundColorCommitPending = true;
	BackgroundColorCommitRemainingSeconds = BackgroundColorCommitDelaySeconds;
}

void UVPTrackingDashboard::HandleAvatarExposureChanged(float Value)
{
	if (bUpdatingAvatarExposureControl || !BroadcastOutput)
	{
		return;
	}

	BroadcastOutput->PreviewAvatarExposure(Value);
	RefreshAvatarExposureControl(BroadcastOutput->GetAvatarExposureStops());
	bAvatarExposureCommitPending = true;
	AvatarExposureCommitRemainingSeconds = BackgroundColorCommitDelaySeconds;
}

void UVPTrackingDashboard::HandleOutputFPSChanged(float Value)
{
	if (bUpdatingOutputFPSControl || !BroadcastOutput)
	{
		return;
	}

	const int32 FramesPerSecond = AVPBroadcastOutput::SanitizeOutputFPS(
		FMath::RoundToInt(Value));
	BroadcastOutput->PreviewOutputFPS(FramesPerSecond);
	RefreshOutputFPSControl(BroadcastOutput->GetOutputFPS());
	bOutputFPSCommitPending = true;
	OutputFPSCommitRemainingSeconds = OutputFPSCommitDelaySeconds;
}

void UVPTrackingDashboard::HandleAddAvatarClicked()
{
	if (AvatarManager)
	{
		AvatarManager->OpenAvatarFileDialog();
	}
}

void UVPTrackingDashboard::HandleDeleteAvatarClicked()
{
	ShowDeleteConfirmationModal();
}

void UVPTrackingDashboard::HandleModalPrimaryClicked()
{
	const bool bShouldDeleteAvatar = bDeleteConfirmationModal;
	HideModal();
	if (bShouldDeleteAvatar && AvatarManager)
	{
		AvatarManager->DeleteActiveAvatar();
		AnimInstance = AvatarManager->GetActiveAnimInstance();
		SetDashboardEnabled(AvatarManager->HasActiveAvatar());
		RefreshDashboard();
	}
}

void UVPTrackingDashboard::HandleModalSecondaryClicked()
{
	HideModal();
}

void UVPTrackingDashboard::HandleResetCameraClicked()
{
	if (BroadcastOutput && BroadcastOutput->IsAvatarCaptureReady())
	{
		BroadcastOutput->ResetCameraToFullBody();
	}
}

void UVPTrackingDashboard::HandleAvatarSelectionChanged(
	FString SelectedItem,
	ESelectInfo::Type SelectionType)
{
	if (!AvatarManager || SelectionType == ESelectInfo::Direct)
	{
		return;
	}

	if (const FString* AvatarId = AvatarOptionToId.Find(SelectedItem))
	{
		AvatarManager->SelectAvatar(*AvatarId);
	}
}
