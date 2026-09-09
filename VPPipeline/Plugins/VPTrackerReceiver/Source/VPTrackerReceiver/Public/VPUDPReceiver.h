#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Common/UdpSocketReceiver.h"
#include "HAL/CriticalSection.h"
#include "VPTrackingData.h"
#include "VPUDPReceiver.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnTrackingDataReceived, const FVPTrackingFrame&, Data);

/** Game-thread snapshot of tracking transport latency. */
struct VPTRACKERRECEIVER_API FVPTrackingLatencySnapshot
{
	bool bHasSamples = false;
	int32 DeliveredFrameCount = 0;
	int32 InvalidSampleCount = 0;
	float LatestCaptureToReceiveMs = 0.0f;
	float LatestCaptureToGameThreadMs = 0.0f;
	float AverageCaptureToGameThreadMs = 0.0f;
	float P95CaptureToGameThreadMs = 0.0f;
	float MaxCaptureToGameThreadMs = 0.0f;
};

/** Thread-safe single-slot handoff that always retains the newest frame. */
class VPTRACKERRECEIVER_API FVPTrackingLatestFrameMailbox
{
public:
	/** Returns true when an older pending frame was superseded. */
	bool Push(const FVPTrackingFrame& Frame);
	bool Pop(FVPTrackingFrame& OutFrame);
	void Reset();

private:
	FCriticalSection Mutex;
	FVPTrackingFrame PendingFrame;
	bool bHasPendingFrame = false;
};

/**
 * UDP Receiver Component
 * Listens for binary tracking packets from Python vp-tracker/sender.py
 * Parses the packet on the network thread, delivers to game thread via TQueue
 */
UCLASS(ClassGroup = (VPPipeline), meta = (BlueprintSpawnableComponent))
class VPTRACKERRECEIVER_API UVPUDPReceiver : public UActorComponent
{
	GENERATED_BODY()

public:
	UVPUDPReceiver();

	/** UDP port to listen on (must match sender.py port) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Pipeline")
	int32 ListenPort = 7000;

	/** Fired on game thread when new tracking data arrives */
	UPROPERTY(BlueprintAssignable, Category = "VP Pipeline")
	FOnTrackingDataReceived OnTrackingDataReceived;

	/** Get the most recent tracking frame (game thread safe) */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	FVPTrackingFrame GetLatestTrackingData() const;

	/** Total packets received since BeginPlay */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	int32 GetPacketCount() const { return PacketCount.Load(); }

	/** Packets rejected because they violated the VPTP schema-3 wire contract */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	int32 GetRejectedPacketCount() const { return RejectedPacketCount.Load(); }

	/** Valid packets rejected before the game-thread handoff. */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	int32 GetDroppedPacketCount() const { return DroppedPacketCount.Load(); }

	/** Pending frames replaced by a newer valid frame before game-thread delivery. */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	int32 GetSupersededFrameCount() const { return SupersededFrameCount.Load(); }

	/** Packets from a recognizable VP tracking sender using another schema. */
	UFUNCTION(BlueprintCallable, Category = "VP Pipeline")
	int32 GetProtocolMismatchCount() const { return ProtocolMismatchPacketCount.Load(); }

	/** Capture/submission-to-Unreal latency for frames applied on the game thread. */
	FVPTrackingLatencySnapshot GetLatencySnapshot() const { return LatencySnapshot; }

	/** Seconds without data before the latest frame becomes invalid */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VP Pipeline", meta = (ClampMin = "0.05", ClampMax = "5.0"))
	float TrackingTimeoutSeconds = 0.5f;

	/** Strict VPTP schema-3 decoder exposed for automation tests and diagnostics. */
	static bool DecodePacket(const uint8* RawData, int32 DataLen, FVPTrackingFrame& OutFrame);

	/** Pure latency helpers exposed for automation tests. */
	static bool IsLatencySampleValid(double LatencyMilliseconds);
	static float CalculatePercentile95(const TArray<float>& Samples);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType,
	                           FActorComponentTickFunction* ThisTickFunction) override;

private:
	FSocket* Socket = nullptr;
	FUdpSocketReceiver* UDPReceiver = nullptr;

	/** Latest-only network-thread -> game-thread handoff. */
	FVPTrackingLatestFrameMailbox LatestFrameMailbox;

	/** Latest frame on game thread */
	FVPTrackingFrame LatestFrame;

	/** Cross-thread counters */
	TAtomic<int32> PacketCount{0};
	TAtomic<int32> RejectedPacketCount{0};
	TAtomic<int32> DroppedPacketCount{0};
	TAtomic<int32> SupersededFrameCount{0};
	TAtomic<int32> ProtocolMismatchPacketCount{0};
	double LastFrameReceiveTimeSeconds = 0.0;
	bool bTrackingWasFresh = false;
	double LastStatisticsRefreshSeconds = 0.0;
	double LastStatisticsLogSeconds = 0.0;
	int32 LastStatisticsLogPacketCount = 0;
	int32 DeliveredFrameCount = 0;
	int32 LatencySampleCount = 0;
	int32 InvalidLatencySampleCount = 0;
	double TotalCaptureToGameThreadMs = 0.0;
	float MaxCaptureToGameThreadMs = 0.0f;
	int32 NextLatencySampleIndex = 0;
	TArray<float> RecentCaptureToGameThreadMs;
	FVPTrackingLatencySnapshot LatencySnapshot;

	/** Pre-allocated parse buffer (avoid per-frame allocation) */
	FVPTrackingFrame ParseBuffer;

	/** ARKit blendshape names in the order sent by Python */
	static const TArray<FName>& GetBlendshapeNames();

	/** Called on network thread when UDP data arrives */
	void OnDataReceived(const FArrayReaderPtr& Data, const FIPv4Endpoint& Endpoint);
	void RecordLatencySample(const FVPTrackingFrame& Frame, double GameThreadTimestampMilliseconds);
	void RefreshLatencySnapshot();
	void LogPeriodicStatistics(double NowSeconds);

};
