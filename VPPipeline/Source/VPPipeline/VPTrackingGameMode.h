#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "VPTrackingGameMode.generated.h"

class UVPTrackingDashboard;
class UVPBroadcastPreview;
class AVPBroadcastOutput;
class AVPAvatarManager;

UCLASS()
class VPPIPELINE_API AVPTrackingHUD : public AHUD
{
	GENERATED_BODY()

public:
	AVPTrackingHUD();

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UVPTrackingDashboard> Dashboard;

	UPROPERTY(Transient)
	TObjectPtr<UVPBroadcastPreview> BroadcastPreview;

	UPROPERTY(Transient)
	TObjectPtr<AVPBroadcastOutput> BroadcastOutput;

	UPROPERTY(Transient)
	TObjectPtr<AVPAvatarManager> AvatarManager;

	bool bDashboardVisible = true;
	bool bWorldRenderingWasDisabled = false;

	UFUNCTION()
	void ToggleDashboard();

	UFUNCTION()
	void ToggleCalibration();
};

UCLASS()
class VPPIPELINE_API AVPTrackingGameModeBase : public AGameModeBase
{
	GENERATED_BODY()

public:
	AVPTrackingGameModeBase();
};
