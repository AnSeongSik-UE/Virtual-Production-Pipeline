#include "VPTrackingGameMode.h"

#include "VPAvatarManager.h"
#include "VPBroadcastOutput.h"
#include "VPBroadcastPreview.h"
#include "VPTrackingDashboard.h"
#include "Blueprint/UserWidget.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"

AVPTrackingHUD::AVPTrackingHUD()
{
	PrimaryActorTick.bCanEverTick = false;
}

void AVPTrackingHUD::BeginPlay()
{
	Super::BeginPlay();

	APlayerController* PlayerController = GetOwningPlayerController();
	if (!PlayerController || !PlayerController->IsLocalPlayerController())
	{
		return;
	}

	if (UWorld* World = GetWorld())
	{
		BroadcastOutput = World->SpawnActor<AVPBroadcastOutput>();
	}
	if (!BroadcastOutput)
	{
		UE_LOG(LogTemp, Error, TEXT("[VPBroadcast] Failed to spawn broadcast output."));
	}
	if (UWorld* World = GetWorld())
	{
		AvatarManager = World->SpawnActor<AVPAvatarManager>();
	}
	if (AvatarManager)
	{
		AvatarManager->Initialize(BroadcastOutput);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[VPAvatar] Failed to spawn avatar manager."));
	}

	BroadcastPreview = CreateWidget<UVPBroadcastPreview>(
		PlayerController,
		UVPBroadcastPreview::StaticClass());
	if (BroadcastPreview)
	{
		BroadcastPreview->SetBroadcastOutput(BroadcastOutput);
		BroadcastPreview->AddToViewport(-100);
	}

	Dashboard = CreateWidget<UVPTrackingDashboard>(PlayerController, UVPTrackingDashboard::StaticClass());
	if (!Dashboard)
	{
		UE_LOG(LogTemp, Error, TEXT("[VPTrackingDashboard] Failed to create dashboard widget."));
		return;
	}
	Dashboard->SetAvatarManager(AvatarManager);

	Dashboard->AddToViewport(100);
	PlayerController->bShowMouseCursor = true;
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
	PlayerController->SetInputMode(InputMode);

	EnableInput(PlayerController);
	if (InputComponent)
	{
		InputComponent->BindKey(EKeys::F1, IE_Pressed, this, &AVPTrackingHUD::ToggleDashboard);
		InputComponent->BindKey(EKeys::C, IE_Pressed, this, &AVPTrackingHUD::ToggleCalibration);
	}

	if (GEngine && GEngine->GameViewport)
	{
		bWorldRenderingWasDisabled = GEngine->GameViewport->bDisableWorldRendering;
		GEngine->GameViewport->bDisableWorldRendering = true;
	}

	UE_LOG(LogTemp, Log,
		TEXT("[VPTrackingDashboard] Dashboard and shared broadcast preview added. F1 toggles operator UI."));
}

void AVPTrackingHUD::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (GEngine && GEngine->GameViewport)
	{
		GEngine->GameViewport->bDisableWorldRendering = bWorldRenderingWasDisabled;
	}
	Super::EndPlay(EndPlayReason);
}

void AVPTrackingHUD::ToggleDashboard()
{
	if (!Dashboard)
	{
		return;
	}
	bDashboardVisible = !bDashboardVisible;
	Dashboard->SetVisibility(bDashboardVisible
		? ESlateVisibility::Visible
		: ESlateVisibility::Collapsed);
}

void AVPTrackingHUD::ToggleCalibration()
{
	if (Dashboard && Dashboard->CanUseCalibrationShortcut())
	{
		Dashboard->ToggleNeutralCalibration();
	}
}

AVPTrackingGameModeBase::AVPTrackingGameModeBase()
{
	HUDClass = AVPTrackingHUD::StaticClass();
}
