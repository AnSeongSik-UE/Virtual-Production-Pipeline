#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SaveGame.h"
#include "VPAvatarManager.generated.h"

class AVPBroadcastOutput;
class UDataTable;
class USceneComponent;
class USkeletalMeshComponent;
class UVPAnimInstance;
class UVPUDPReceiver;
class UVrmAssetListObject;
class UVrmDropFilesComponent;
class UVrmLoaderComponent;

enum class EVPAvatarNoticeSeverity : uint8
{
	Info,
	Error
};

USTRUCT(BlueprintType)
struct FVPAvatarLibraryEntry
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FString AvatarId;

	UPROPERTY(SaveGame)
	FString DisplayName;

	UPROPERTY(SaveGame)
	FString ManagedFileName = TEXT("model.vrm");
};

UCLASS()
class UVPAvatarLibrarySaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(SaveGame)
	int32 SchemaVersion = 1;

	UPROPERTY(SaveGame)
	FString LastSelectedAvatarId;

	UPROPERTY(SaveGame)
	TMap<FString, FVPAvatarLibraryEntry> Entries;
};

/** Owns the user-managed VRM library and the one active runtime avatar. */
UCLASS()
class VPPIPELINE_API AVPAvatarManager : public AActor
{
	GENERATED_BODY()

public:
	AVPAvatarManager();

	virtual void Tick(float DeltaSeconds) override;
	virtual void BeginPlay() override;

	void Initialize(AVPBroadcastOutput* InBroadcastOutput);
	bool OpenAvatarFileDialog();
	bool AddAvatarFromFile(const FString& SourcePath);
	bool SelectAvatar(const FString& AvatarId);
	bool DeleteActiveAvatar();

	TArray<FString> GetAvatarIds() const;
	FString GetAvatarDisplayName(const FString& AvatarId) const;
	const FString& GetActiveAvatarId() const { return ActiveAvatarId; }
	FString GetActiveAvatarDisplayName() const;
	const FString& GetStatusText() const { return StatusText; }
	const FString& GetUserNoticeText() const { return UserNoticeText; }
	EVPAvatarNoticeSeverity GetUserNoticeSeverity() const { return UserNoticeSeverity; }
	int32 GetUserNoticeRevision() const { return UserNoticeRevision; }
	bool IsLoading() const { return bLoadPending; }
	int32 GetLibraryRevision() const { return LibraryRevision; }
	bool HasActiveAvatar() const;
	UVPAnimInstance* GetActiveAnimInstance() const;

	static FString BuildAvatarId(const TArray<uint8>& FileData);
	static bool IsValidAvatarId(const FString& AvatarId);
	static bool IsSupportedAvatarPath(const FString& FilePath);
	static FString BuildAlreadyRegisteredNotice(const FString& DisplayName);
	static bool IsActiveAvatarStateComplete(
		bool bHasAvatarId,
		bool bHasAssetList,
		bool bHasSkeletalMesh,
		bool bHasAnimInstance);

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<USkeletalMeshComponent> RuntimeMesh;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UVrmLoaderComponent> VrmLoader;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UVrmDropFilesComponent> DropFiles;

	/** Owns the application's single UDP tracking socket and its receive thread. */
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UVPUDPReceiver> Receiver;

	/** Shared ARKit-to-VRM morph target mapping used by every runtime avatar. */
	UPROPERTY()
	TObjectPtr<UDataTable> BlendshapeMappingTable;

	UPROPERTY(Transient)
	TObjectPtr<UVrmAssetListObject> ActiveAssetList;

	UPROPERTY(Transient)
	TObjectPtr<UVrmAssetListObject> PendingAssetTemplate;

	UPROPERTY(Transient)
	TObjectPtr<AVPBroadcastOutput> BroadcastOutput;

	TMap<FString, FVPAvatarLibraryEntry> Entries;
	FString LastSelectedAvatarId;
	FString ActiveAvatarId;
	FString PendingAvatarId;
	FString StatusText = TEXT("아바타 없음 · VRM 파일을 추가하세요.");
	FString UserNoticeText;
	EVPAvatarNoticeSeverity UserNoticeSeverity = EVPAvatarNoticeSeverity::Info;
	bool bLoadPending = false;
	bool bPendingEntryIsNew = false;
	int32 LibraryRevision = 0;
	int32 UserNoticeRevision = 0;

	static constexpr int64 MaximumAvatarFileBytes = 256ll * 1024ll * 1024ll;
	static FString GetLibraryRoot();
	static FString GetManagedAvatarPath(const FVPAvatarLibraryEntry& Entry);
	bool LoadLibrary();
	bool SaveLibrary() const;
	void ReconcilePersistentAvatarData();
	int32 CountOrphanedManagedAvatarDirectories() const;
	UVrmAssetListObject* CreateVrmAssetTemplate() const;
	void BeginLoad(const FString& AvatarId, bool bNewEntry);
	void FailPendingLoad(const FString& Reason);
	void PublishUserNotice(const FString& Message, EVPAvatarNoticeSeverity Severity);
	void ClearActiveAvatar();
	bool DeleteManagedAvatarFile(const FVPAvatarLibraryEntry& Entry) const;
	static FName ResolveHumanoidBoneName(const UVrmAssetListObject* AssetList, const FString& HumanoidName);

	UFUNCTION()
	void HandleDroppedFile(FString FileName);

	UFUNCTION()
	void HandleVrmLoaded(UVrmAssetListObject* AssetList);
};
