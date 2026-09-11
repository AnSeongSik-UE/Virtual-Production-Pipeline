#include "VPAvatarManager.h"
#include "VPAvatarFileDialog.h"

#include "VPBroadcastOutput.h"
#include "VPRuntimeAvatarAnimInstance.h"
#include "VPAnimInstance.h"
#include "VPUDPReceiver.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Blueprint.h"
#include "Engine/DataTable.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Hash/Blake3.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Char.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"
#include "Widgets/SWindow.h"
#include "LoaderBPFunctionLibrary.h"
#include "VrmAssetListObject.h"
#include "VrmDropFiles.h"
#include "VrmLoaderComponent.h"
#include "VrmMetaObject.h"
#include "VrmRuntimeSettings.h"

namespace
{
const TCHAR* AvatarLibrarySlot = TEXT("VPAvatarLibrary");
constexpr int32 AvatarLibraryUserIndex = 0;
}

AVPAvatarManager::AVPAvatarManager()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;
	RuntimeMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("RuntimeAvatarMesh"));
	RuntimeMesh->SetupAttachment(SceneRoot);
	RuntimeMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RuntimeMesh->SetGenerateOverlapEvents(false);
	RuntimeMesh->SetLightingChannels(false, true, false);
	RuntimeMesh->SetVisibility(false, true);
	RuntimeMesh->SetHiddenInGame(true, true);

	VrmLoader = CreateDefaultSubobject<UVrmLoaderComponent>(TEXT("VrmLoader"));
	VrmLoader->PrimaryComponentTick.bCanEverTick = true;
	VrmLoader->PrimaryComponentTick.bStartWithTickEnabled = false;
	DropFiles = CreateDefaultSubobject<UVrmDropFilesComponent>(TEXT("VrmDropFiles"));
	Receiver = CreateDefaultSubobject<UVPUDPReceiver>(TEXT("TrackingReceiver"));

	static ConstructorHelpers::FObjectFinder<UDataTable> BlendshapeMappingAsset(
		TEXT("/Game/Data/VRoidBlendshapeMapping.VRoidBlendshapeMapping"));
	BlendshapeMappingTable = BlendshapeMappingAsset.Object;
}

void AVPAvatarManager::BeginPlay()
{
	Super::BeginPlay();
	VrmLoader->OnFinishLoad.AddDynamic(this, &AVPAvatarManager::HandleVrmLoaded);
	DropFiles->OnDropFiles.AddDynamic(this, &AVPAvatarManager::HandleDroppedFile);
	LoadLibrary();
	const int32 OrphanedDirectoryCount = CountOrphanedManagedAvatarDirectories();
	if (OrphanedDirectoryCount > 0)
	{
		PublishUserNotice(
			FString::Printf(
				TEXT("보관함에 등록되지 않은 관리 VRM 폴더가 %d개 있습니다. 자동 삭제하지 않았습니다."),
				OrphanedDirectoryCount),
			EVPAvatarNoticeSeverity::Info);
	}
	ClearActiveAvatar();
	if (!LastSelectedAvatarId.IsEmpty() && Entries.Contains(LastSelectedAvatarId))
	{
		SelectAvatar(LastSelectedAvatarId);
	}
}

void AVPAvatarManager::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	FileDialog.Reset();
	Super::EndPlay(EndPlayReason);
}

void AVPAvatarManager::Initialize(AVPBroadcastOutput* InBroadcastOutput)
{
	BroadcastOutput = InBroadcastOutput;
	ReconcilePersistentAvatarData();
	if (BroadcastOutput && ActiveAvatarId.IsEmpty())
	{
		BroadcastOutput->ClearCapturedAvatar();
	}
}

void AVPAvatarManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (FileDialog && FileDialog->IsComplete())
	{
		const FString SelectedFile = FileDialog->GetPath();
		const uint32 DialogError = FileDialog->GetError();
		FileDialog.Reset();
		if (!bEndingPlay && !SelectedFile.IsEmpty()) { AddAvatarFromFile(SelectedFile); }
		else if (!bEndingPlay && DialogError != 0)
		{
			PublishUserNotice(FString::Printf(TEXT("VRM 선택창을 열지 못했습니다. 오류: %u"), DialogError), EVPAvatarNoticeSeverity::Error);
		}
	}
	UVPAnimInstance* RuntimeAnim = GetActiveAnimInstance();
	if (RuntimeAnim && Receiver)
	{
		RuntimeAnim->TrackingData = Receiver->GetLatestTrackingData();
	}
}

FString AVPAvatarManager::GetLibraryRoot()
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Avatars"));
}

FString AVPAvatarManager::GetManagedAvatarPath(const FVPAvatarLibraryEntry& Entry)
{
	if (!IsValidAvatarId(Entry.AvatarId) || Entry.ManagedFileName != TEXT("model.vrm"))
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(
		GetLibraryRoot() / Entry.AvatarId / TEXT("model.vrm"));
}

FString AVPAvatarManager::BuildAvatarId(const TArray<uint8>& FileData)
{
	if (FileData.IsEmpty())
	{
		return FString();
	}
	return LexToString(FBlake3::HashBuffer(FileData.GetData(), FileData.Num())).ToLower();
}

bool AVPAvatarManager::IsValidAvatarId(const FString& AvatarId)
{
	if (AvatarId.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : AvatarId)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}
	return true;
}

bool AVPAvatarManager::IsSupportedAvatarPath(const FString& FilePath)
{
	return FPaths::GetExtension(FilePath, true).Equals(TEXT(".vrm"), ESearchCase::IgnoreCase);
}

bool AVPAvatarManager::IsActiveAvatarStateComplete(
	bool bHasAvatarId,
	bool bHasAssetList,
	bool bHasSkeletalMesh,
	bool bHasAnimInstance)
{
	return bHasAvatarId && bHasAssetList && bHasSkeletalMesh && bHasAnimInstance;
}

bool AVPAvatarManager::CalculateHumanoidFramingBounds(
	const FBox& RenderBounds,
	const FVector& Head,
	const FVector& LeftHand,
	const FVector& RightHand,
	const FVector& LeftFoot,
	const FVector& RightFoot,
	FBox& OutBounds)
{
	if (!RenderBounds.IsValid || Head.ContainsNaN() || LeftHand.ContainsNaN() ||
		RightHand.ContainsNaN() || LeftFoot.ContainsNaN() || RightFoot.ContainsNaN())
	{
		return false;
	}

	const float FootZ = FMath::Min(LeftFoot.Z, RightFoot.Z);
	const float BodyHeight = Head.Z - FootZ;
	if (!FMath::IsFinite(BodyHeight) || BodyHeight < 20.0f)
	{
		return false;
	}

	const FVector FeetCenter = (LeftFoot + RightFoot) * 0.5f;
	const FVector BodyCenter = (Head + FeetCenter) * 0.5f;
	const float MaxHorizontalRadius = BodyHeight * 0.65f;
	const float BoneMargin = BodyHeight * 0.04f;
	const float RawBoneMinX = FMath::Min(
		FMath::Min3(Head.X, LeftHand.X, RightHand.X),
		FMath::Min(LeftFoot.X, RightFoot.X));
	const float RawBoneMaxX = FMath::Max(
		FMath::Max3(Head.X, LeftHand.X, RightHand.X),
		FMath::Max(LeftFoot.X, RightFoot.X));
	const float RequiredMinX = FMath::Max(BodyCenter.X - MaxHorizontalRadius, RawBoneMinX - BoneMargin);
	const float RequiredMaxX = FMath::Min(BodyCenter.X + MaxHorizontalRadius, RawBoneMaxX + BoneMargin);
	const float MinX = FMath::Clamp(RenderBounds.Min.X, BodyCenter.X - MaxHorizontalRadius, RequiredMinX);
	const float MaxX = FMath::Clamp(RenderBounds.Max.X, RequiredMaxX, BodyCenter.X + MaxHorizontalRadius);

	const float MinZ = FMath::Clamp(
		RenderBounds.Min.Z,
		FootZ - BodyHeight * 0.08f,
		FootZ - BodyHeight * 0.02f);
	const float MaxZ = FMath::Clamp(
		RenderBounds.Max.Z,
		Head.Z + BodyHeight * 0.12f,
		Head.Z + BodyHeight * 0.35f);
	const float DepthRadius = BodyHeight * 0.12f;

	OutBounds = FBox(
		FVector(MinX, BodyCenter.Y - DepthRadius, MinZ),
		FVector(MaxX, BodyCenter.Y + DepthRadius, MaxZ));
	return OutBounds.IsValid && !OutBounds.GetExtent().IsNearlyZero();
}

bool AVPAvatarManager::TryGetHumanoidFramingBounds(FBox& OutBounds) const
{
	if (!RuntimeMesh || !ActiveAssetList || !RuntimeMesh->GetSkeletalMeshAsset())
	{
		return false;
	}

	const FName HeadBone = ResolveHumanoidBoneName(ActiveAssetList, TEXT("head"));
	const FName LeftHandBone = ResolveHumanoidBoneName(ActiveAssetList, TEXT("leftHand"));
	const FName RightHandBone = ResolveHumanoidBoneName(ActiveAssetList, TEXT("rightHand"));
	const FName LeftFootBone = ResolveHumanoidBoneName(ActiveAssetList, TEXT("leftFoot"));
	const FName RightFootBone = ResolveHumanoidBoneName(ActiveAssetList, TEXT("rightFoot"));
	const FName RequiredBones[] = {HeadBone, LeftHandBone, RightHandBone, LeftFootBone, RightFootBone};
	for (const FName BoneName : RequiredBones)
	{
		if (BoneName.IsNone() || RuntimeMesh->GetBoneIndex(BoneName) == INDEX_NONE)
		{
			return false;
		}
	}

	return CalculateHumanoidFramingBounds(
		RuntimeMesh->Bounds.GetBox(),
		RuntimeMesh->GetBoneLocation(HeadBone),
		RuntimeMesh->GetBoneLocation(LeftHandBone),
		RuntimeMesh->GetBoneLocation(RightHandBone),
		RuntimeMesh->GetBoneLocation(LeftFootBone),
		RuntimeMesh->GetBoneLocation(RightFootBone),
		OutBounds);
}

FString AVPAvatarManager::BuildAlreadyRegisteredNotice(const FString& DisplayName)
{
	return FString::Printf(
		TEXT("'%s' 아바타는 이미 등록되어 있습니다.\n기존 보관함 항목으로 전환합니다."),
		*DisplayName);
}

bool AVPAvatarManager::OpenAvatarFileDialog()
{
	if (bEndingPlay) { return false; }
	if (FileDialog)
	{
		FileDialog->RequestForeground();
		return true;
	}
	if (bLoadPending) { return false; }
#if PLATFORM_WINDOWS
	FileDialog = MakeShared<FVPAvatarFileDialog>();
	return true;
#else
	return false;
#endif
}

bool AVPAvatarManager::AddAvatarFromFile(const FString& SourcePath)
{
	if (bLoadPending)
	{
		StatusText = TEXT("다른 아바타를 불러오는 중입니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Info);
		return false;
	}
	const FString FullSourcePath = FPaths::ConvertRelativePathToFull(SourcePath);
	const int64 FileSize = IFileManager::Get().FileSize(*FullSourcePath);
	if (!IsSupportedAvatarPath(FullSourcePath) || FileSize <= 0 ||
		FileSize > MaximumAvatarFileBytes ||
		!ULoaderBPFunctionLibrary::IsValidVRM4UFile(FullSourcePath))
	{
		StatusText = TEXT("유효한 256MB 이하 VRM 파일만 추가할 수 있습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
		return false;
	}

	TArray<uint8> FileData;
	if (!FFileHelper::LoadFileToArray(FileData, *FullSourcePath))
	{
		StatusText = TEXT("VRM 파일을 읽을 수 없습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
		return false;
	}
	const FString AvatarId = BuildAvatarId(FileData);
	if (!IsValidAvatarId(AvatarId))
	{
		StatusText = TEXT("VRM 파일 식별자를 만들 수 없습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
		return false;
	}
	if (Entries.Contains(AvatarId))
	{
		StatusText = TEXT("이미 등록된 아바타입니다. 기존 항목을 불러옵니다.");
		PublishUserNotice(
			BuildAlreadyRegisteredNotice(GetAvatarDisplayName(AvatarId)),
			EVPAvatarNoticeSeverity::Info);
		return SelectAvatar(AvatarId);
	}

	FVPAvatarLibraryEntry Entry;
	Entry.AvatarId = AvatarId;
	Entry.DisplayName = FPaths::GetBaseFilename(FullSourcePath);
	const FString ManagedPath = GetManagedAvatarPath(Entry);
	if (ManagedPath.IsEmpty() ||
		!IFileManager::Get().MakeDirectory(*FPaths::GetPath(ManagedPath), true) ||
		IFileManager::Get().Copy(*ManagedPath, *FullSourcePath, true, true) != COPY_OK)
	{
		StatusText = TEXT("아바타 보관함에 VRM을 복사하지 못했습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
		return false;
	}

	Entries.Add(AvatarId, Entry);
	++LibraryRevision;
	BeginLoad(AvatarId, true);
	return true;
}

bool AVPAvatarManager::SelectAvatar(const FString& AvatarId)
{
	if (bLoadPending || !IsValidAvatarId(AvatarId) || !Entries.Contains(AvatarId))
	{
		return false;
	}
	if (AvatarId == ActiveAvatarId)
	{
		return true;
	}
	BeginLoad(AvatarId, false);
	return true;
}

UVrmAssetListObject* AVPAvatarManager::CreateVrmAssetTemplate() const
{
	UClass* TemplateClass = nullptr;
	if (const UVrmRuntimeSettings* Settings = GetDefault<UVrmRuntimeSettings>())
	{
		const FString TemplateObjectPath = Settings->AssetListObject.ToString();
		if (!TemplateObjectPath.IsEmpty())
		{
			// Cooked builds strip the UBlueprint object but retain its generated class.
			// Load the class first so runtime VRM materials work outside the editor.
			TemplateClass = StaticLoadClass(
				UVrmAssetListObject::StaticClass(),
				nullptr,
				*(TemplateObjectPath + TEXT("_C")));
		}
		if (!TemplateClass)
		{
			if (UObject* TemplateObject = Settings->AssetListObject.TryLoad())
			{
				if (const UBlueprint* TemplateBlueprint = Cast<UBlueprint>(TemplateObject))
				{
					if (TemplateBlueprint->GeneratedClass &&
						TemplateBlueprint->GeneratedClass->IsChildOf(UVrmAssetListObject::StaticClass()))
					{
						TemplateClass = TemplateBlueprint->GeneratedClass;
					}
				}
				else if (UClass* LoadedClass = Cast<UClass>(TemplateObject);
					LoadedClass && LoadedClass->IsChildOf(UVrmAssetListObject::StaticClass()))
				{
					TemplateClass = LoadedClass;
				}
			}
		}
	}
	if (!TemplateClass)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[VPAvatar] VRM4U runtime asset template is unavailable. Ensure /VRM4U is cooked."));
		return nullptr;
	}

	UVrmAssetListObject* Template = NewObject<UVrmAssetListObject>(
		GetTransientPackage(),
		TemplateClass);
	if (!Template || !Template->MtoonLitSet)
	{
		UE_LOG(LogTemp, Error,
			TEXT("[VPAvatar] VRM4U MToon runtime material set is unavailable."));
		return nullptr;
	}
	return Template;
}

void AVPAvatarManager::BeginLoad(const FString& AvatarId, bool bNewEntry)
{
	const FVPAvatarLibraryEntry* Entry = Entries.Find(AvatarId);
	if (!Entry)
	{
		return;
	}
	const FString ManagedPath = GetManagedAvatarPath(*Entry);
	const int64 ManagedFileSize = ManagedPath.IsEmpty()
		? INDEX_NONE : IFileManager::Get().FileSize(*ManagedPath);
	if (ManagedPath.IsEmpty() || ManagedFileSize <= 0 ||
		ManagedFileSize > MaximumAvatarFileBytes)
	{
		StatusText = TEXT("보관함의 VRM 파일이 없거나 허용 크기를 벗어났습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
		return;
	}
	{
		TArray<uint8> ManagedFileData;
		if (!FFileHelper::LoadFileToArray(ManagedFileData, *ManagedPath) ||
			BuildAvatarId(ManagedFileData) != AvatarId)
		{
			StatusText = TEXT("보관함 VRM의 내용과 아바타 ID가 일치하지 않습니다. 파일을 다시 추가하세요.");
			PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
			return;
		}
	}

	PendingAvatarId = AvatarId;
	bPendingEntryIsNew = bNewEntry;
	bLoadPending = true;
	StatusText = FString::Printf(TEXT("%s 불러오는 중…"), *Entry->DisplayName);
	PendingAssetTemplate = CreateVrmAssetTemplate();
	if (!PendingAssetTemplate)
	{
		FailPendingLoad(TEXT("VRM4U 런타임 재질이 설치 또는 배포본에 포함되지 않았습니다."));
		return;
	}
	ULoaderBPFunctionLibrary::VRMSetLoadMaterialType(EVRMImportMaterialType::VRMIMT_MToon);
	if (!VrmLoader->LoadVRMFileAsync(PendingAssetTemplate, ManagedPath))
	{
		FailPendingLoad(TEXT("VRM 비동기 로딩을 시작하지 못했습니다."));
	}
}

FName AVPAvatarManager::ResolveHumanoidBoneName(
	const UVrmAssetListObject* AssetList,
	const FString& HumanoidName)
{
	if (!AssetList || !AssetList->VrmMetaObject)
	{
		return NAME_None;
	}
	for (const TPair<FString, FString>& Pair : AssetList->VrmMetaObject->humanoidBoneTable)
	{
		if (Pair.Key.Equals(HumanoidName, ESearchCase::IgnoreCase))
		{
			return FName(*Pair.Value);
		}
	}
	return NAME_None;
}

void AVPAvatarManager::HandleVrmLoaded(UVrmAssetListObject* AssetList)
{
	if (!bLoadPending)
	{
		return;
	}
	if (!AssetList || !AssetList->SkeletalMesh)
	{
		FailPendingLoad(TEXT("VRM에서 Skeletal Mesh를 만들지 못했습니다."));
		return;
	}

	const FName HeadBone = ResolveHumanoidBoneName(AssetList, TEXT("head"));
	const FName NeckBone = ResolveHumanoidBoneName(AssetList, TEXT("neck"));
	const FName LeftArmBone = ResolveHumanoidBoneName(AssetList, TEXT("leftUpperArm"));
	const FName RightArmBone = ResolveHumanoidBoneName(AssetList, TEXT("rightUpperArm"));
	const FName LeftLowerArmBone = ResolveHumanoidBoneName(AssetList, TEXT("leftLowerArm"));
	const FName RightLowerArmBone = ResolveHumanoidBoneName(AssetList, TEXT("rightLowerArm"));
	const FReferenceSkeleton& RefSkeleton = AssetList->SkeletalMesh->GetRefSkeleton();
	if (HeadBone.IsNone() || LeftArmBone.IsNone() || RightArmBone.IsNone() ||
		RefSkeleton.FindBoneIndex(HeadBone) == INDEX_NONE ||
		RefSkeleton.FindBoneIndex(LeftArmBone) == INDEX_NONE ||
		RefSkeleton.FindBoneIndex(RightArmBone) == INDEX_NONE)
	{
		FailPendingLoad(TEXT("머리와 양쪽 상완 휴머노이드 본이 모두 필요합니다."));
		return;
	}

	RuntimeMesh->SetAnimInstanceClass(nullptr);
	RuntimeMesh->SetSkeletalMesh(AssetList->SkeletalMesh, true);
	RuntimeMesh->SetRelativeTransform(FTransform::Identity);
	RuntimeMesh->SetAnimInstanceClass(UVPRuntimeAvatarAnimInstance::StaticClass());
	UVPRuntimeAvatarAnimInstance* RuntimeAnim =
		Cast<UVPRuntimeAvatarAnimInstance>(RuntimeMesh->GetAnimInstance());
	if (!RuntimeAnim)
	{
		ClearActiveAvatar();
		FailPendingLoad(TEXT("런타임 트래킹 AnimInstance를 만들지 못했습니다."));
		return;
	}

	RuntimeAnim->bAutoFindReceiver = false;
	RuntimeAnim->AvatarTrackingProfileId = PendingAvatarId;
	RuntimeAnim->HeadBoneName = HeadBone;
	RuntimeAnim->NeckBoneName = NeckBone;
	RuntimeAnim->LeftUpperArmBoneName = LeftArmBone;
	RuntimeAnim->RightUpperArmBoneName = RightArmBone;
	RuntimeAnim->LeftLowerArmBoneName = LeftLowerArmBone;
	RuntimeAnim->RightLowerArmBoneName = RightLowerArmBone;
	RuntimeAnim->ConfigureReferencePoseArmLiftAxes();
	RuntimeAnim->BlendshapeMappingTable = BlendshapeMappingTable;
	RuntimeAnim->LoadTrackingProfile();

	ActiveAssetList = AssetList;
	ActiveAvatarId = PendingAvatarId;
	LastSelectedAvatarId = ActiveAvatarId;
	RuntimeMesh->SetHiddenInGame(false, true);
	RuntimeMesh->SetVisibility(true, true);
	if (BroadcastOutput)
	{
		BroadcastOutput->SetCapturedAvatar(this, ActiveAvatarId);
	}
	const FString DisplayName = GetActiveAvatarDisplayName();
	PendingAvatarId.Reset();
	PendingAssetTemplate = nullptr;
	bLoadPending = false;
	bPendingEntryIsNew = false;
	StatusText = FString::Printf(TEXT("아바타 사용 중 · %s"), *DisplayName);
	++LibraryRevision;
	SaveLibrary();
}

void AVPAvatarManager::FailPendingLoad(const FString& Reason)
{
	if (bPendingEntryIsNew)
	{
		if (const FVPAvatarLibraryEntry* Entry = Entries.Find(PendingAvatarId))
		{
			DeleteManagedAvatarFile(*Entry);
		}
		Entries.Remove(PendingAvatarId);
		++LibraryRevision;
	}
	PendingAvatarId.Reset();
	PendingAssetTemplate = nullptr;
	bLoadPending = false;
	bPendingEntryIsNew = false;
	StatusText = Reason;
	PublishUserNotice(Reason, EVPAvatarNoticeSeverity::Error);
}

void AVPAvatarManager::PublishUserNotice(
	const FString& Message,
	EVPAvatarNoticeSeverity Severity)
{
	UserNoticeText = Message;
	UserNoticeSeverity = Severity;
	++UserNoticeRevision;
}

void AVPAvatarManager::ClearActiveAvatar()
{
	if (RuntimeMesh)
	{
		if (UVPAnimInstance* RuntimeAnim = Cast<UVPAnimInstance>(RuntimeMesh->GetAnimInstance()))
		{
			RuntimeAnim->CancelNeutralCalibration();
			RuntimeAnim->CancelArmValidation();
		}
		RuntimeMesh->SetHiddenInGame(true, true);
		RuntimeMesh->SetVisibility(false, true);
		RuntimeMesh->SetAnimInstanceClass(nullptr);
		RuntimeMesh->SetSkeletalMesh(nullptr, true);
		RuntimeMesh->EmptyOverrideMaterials();
	}
	ActiveAssetList = nullptr;
	ActiveAvatarId.Reset();
	if (BroadcastOutput)
	{
		BroadcastOutput->ClearCapturedAvatar();
	}
}

bool AVPAvatarManager::DeleteActiveAvatar()
{
	if (bLoadPending || ActiveAvatarId.IsEmpty())
	{
		StatusText = TEXT("삭제할 활성 아바타가 없습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Info);
		return false;
	}
	const FString DeletedId = ActiveAvatarId;
	const FVPAvatarLibraryEntry Entry = Entries.FindChecked(DeletedId);
	if (!DeleteManagedAvatarFile(Entry))
	{
		StatusText = TEXT("관리 VRM 파일을 삭제하지 못했습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
		return false;
	}

	ClearActiveAvatar();
	Entries.Remove(DeletedId);
	LastSelectedAvatarId.Reset();
	const bool bTrackingProfileDeleted =
		UVPAnimInstance::DeleteTrackingProfileById(DeletedId);
	const bool bCameraProfileDeleted = !BroadcastOutput ||
		BroadcastOutput->DeleteCameraProfile(DeletedId);
	++LibraryRevision;
	const bool bLibrarySaved = SaveLibrary();
	if (!bTrackingProfileDeleted || !bCameraProfileDeleted || !bLibrarySaved)
	{
		StatusText = TEXT("아바타는 삭제했지만 일부 설정을 디스크에서 정리하지 못했습니다. 재실행 시 다시 정리합니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
		return false;
	}
	StatusText = TEXT("아바타와 카메라·캘리브레이션·매핑 설정을 삭제했습니다.");
	return true;
}

bool AVPAvatarManager::DeleteManagedAvatarFile(const FVPAvatarLibraryEntry& Entry) const
{
	if (!IsValidAvatarId(Entry.AvatarId) || Entry.ManagedFileName != TEXT("model.vrm"))
	{
		return false;
	}
	const FString ManagedPath = GetManagedAvatarPath(Entry);
	const FString ExpectedPath = FPaths::ConvertRelativePathToFull(
		GetLibraryRoot() / Entry.AvatarId / TEXT("model.vrm"));
	if (ManagedPath.IsEmpty() || !ManagedPath.Equals(ExpectedPath, ESearchCase::IgnoreCase))
	{
		return false;
	}
	if (IFileManager::Get().FileExists(*ManagedPath) &&
		!IFileManager::Get().Delete(*ManagedPath, false, true, true))
	{
		return false;
	}
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(ManagedPath), false, false);
	return true;
}

bool AVPAvatarManager::LoadLibrary()
{
	if (!UGameplayStatics::DoesSaveGameExist(AvatarLibrarySlot, AvatarLibraryUserIndex))
	{
		return false;
	}
	const UVPAvatarLibrarySaveGame* SaveGame = Cast<UVPAvatarLibrarySaveGame>(
		UGameplayStatics::LoadGameFromSlot(AvatarLibrarySlot, AvatarLibraryUserIndex));
	if (!SaveGame || SaveGame->SchemaVersion != 1)
	{
		return false;
	}
	bool bRemovedInvalidEntry = false;
	for (const TPair<FString, FVPAvatarLibraryEntry>& Pair : SaveGame->Entries)
	{
		if (Pair.Key == Pair.Value.AvatarId && IsValidAvatarId(Pair.Key) &&
			Pair.Value.ManagedFileName == TEXT("model.vrm") &&
			IFileManager::Get().FileExists(*GetManagedAvatarPath(Pair.Value)))
		{
			Entries.Add(Pair.Key, Pair.Value);
		}
		else
		{
			bRemovedInvalidEntry = true;
		}
	}
	const bool bClearedInvalidLastSelected =
		!SaveGame->LastSelectedAvatarId.IsEmpty() &&
		!Entries.Contains(SaveGame->LastSelectedAvatarId);
	LastSelectedAvatarId = Entries.Contains(SaveGame->LastSelectedAvatarId)
		? SaveGame->LastSelectedAvatarId : FString();
	++LibraryRevision;
	if ((bRemovedInvalidEntry || bClearedInvalidLastSelected) && !SaveLibrary())
	{
		StatusText = TEXT("보관함의 손상된 항목을 정리했지만 저장하지 못했습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
	}
	return true;
}

int32 AVPAvatarManager::CountOrphanedManagedAvatarDirectories() const
{
	TArray<FString> DirectoryNames;
	IFileManager::Get().FindFiles(
		DirectoryNames,
		*(GetLibraryRoot() / TEXT("*")),
		false,
		true);

	int32 Count = 0;
	for (const FString& DirectoryName : DirectoryNames)
	{
		if (IsValidAvatarId(DirectoryName) && !Entries.Contains(DirectoryName) &&
			IFileManager::Get().FileExists(
				*(GetLibraryRoot() / DirectoryName / TEXT("model.vrm"))))
		{
			++Count;
		}
	}
	return Count;
}

void AVPAvatarManager::ReconcilePersistentAvatarData()
{
	TSet<FString> ValidAvatarIds;
	for (const TPair<FString, FVPAvatarLibraryEntry>& Pair : Entries)
	{
		ValidAvatarIds.Add(Pair.Key);
	}

	int32 RemovedTrackingProfileCount = 0;
	const bool bTrackingProfilesSaved = UVPAnimInstance::PruneManagedTrackingProfiles(
		ValidAvatarIds,
		RemovedTrackingProfileCount);
	int32 RemovedCameraProfileCount = 0;
	const bool bCameraProfilesSaved = !BroadcastOutput ||
		BroadcastOutput->PruneCameraProfiles(ValidAvatarIds, RemovedCameraProfileCount);
	if (!bTrackingProfilesSaved || !bCameraProfilesSaved)
	{
		StatusText = TEXT("삭제된 아바타의 저장 설정을 정리하지 못했습니다.");
		PublishUserNotice(StatusText, EVPAvatarNoticeSeverity::Error);
	}
	else if (RemovedTrackingProfileCount > 0 || RemovedCameraProfileCount > 0)
	{
		UE_LOG(LogTemp, Log,
			TEXT("[VPAvatar] Pruned %d tracking and %d camera orphan profiles."),
			RemovedTrackingProfileCount,
			RemovedCameraProfileCount);
	}
}

bool AVPAvatarManager::SaveLibrary() const
{
	UVPAvatarLibrarySaveGame* SaveGame = Cast<UVPAvatarLibrarySaveGame>(
		UGameplayStatics::CreateSaveGameObject(UVPAvatarLibrarySaveGame::StaticClass()));
	if (!SaveGame)
	{
		return false;
	}
	SaveGame->SchemaVersion = 1;
	SaveGame->LastSelectedAvatarId = LastSelectedAvatarId;
	SaveGame->Entries = Entries;
	return UGameplayStatics::SaveGameToSlot(SaveGame, AvatarLibrarySlot, AvatarLibraryUserIndex);
}

TArray<FString> AVPAvatarManager::GetAvatarIds() const
{
	TArray<FString> Result;
	Entries.GetKeys(Result);
	Result.Sort([&](const FString& Left, const FString& Right)
	{
		return GetAvatarDisplayName(Left) < GetAvatarDisplayName(Right);
	});
	return Result;
}

FString AVPAvatarManager::GetAvatarDisplayName(const FString& AvatarId) const
{
	const FVPAvatarLibraryEntry* Entry = Entries.Find(AvatarId);
	return Entry ? Entry->DisplayName : FString();
}

FString AVPAvatarManager::GetActiveAvatarDisplayName() const
{
	return GetAvatarDisplayName(ActiveAvatarId);
}

UVPAnimInstance* AVPAvatarManager::GetActiveAnimInstance() const
{
	return HasActiveAvatar()
		? Cast<UVPAnimInstance>(RuntimeMesh->GetAnimInstance()) : nullptr;
}

bool AVPAvatarManager::HasActiveAvatar() const
{
	return IsActiveAvatarStateComplete(
		!ActiveAvatarId.IsEmpty(),
		ActiveAssetList != nullptr,
		RuntimeMesh && RuntimeMesh->GetSkeletalMeshAsset() != nullptr,
		RuntimeMesh && Cast<UVPAnimInstance>(RuntimeMesh->GetAnimInstance()) != nullptr);
}

void AVPAvatarManager::HandleDroppedFile(FString FileName)
{
	AddAvatarFromFile(FileName);
	FTimerHandle FocusTimerHandle;
	GetWorldTimerManager().SetTimer(
		FocusTimerHandle,
		FTimerDelegate::CreateWeakLambda(this, []()
	{
		if (GEngine && GEngine->GameViewport)
		{
			if (const TSharedPtr<SWindow> GameWindow = GEngine->GameViewport->GetWindow())
			{
				GameWindow->BringToFront(true);
				GameWindow->HACK_ForceToFront();
			}
		}
		}),
		0.1f,
		false);
}
