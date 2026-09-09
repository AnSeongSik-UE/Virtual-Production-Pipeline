#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "RHI.h"

class UTextureRenderTarget2D;

class VPBROADCASTRENDERER_API FVPBroadcastRendererModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
};

namespace VPBroadcastRenderer
{
	/** Enqueue one full-screen GPU pass after the scene capture command. */
	VPBROADCASTRENDERER_API void EnqueueAvatarComposite(
		UTextureRenderTarget2D* AvatarTexture,
		UTextureRenderTarget2D* OutputTexture,
		const FLinearColor& BackgroundColor,
		float ExposureStops,
		bool bRemoveBackground,
		ERHIFeatureLevel::Type FeatureLevel);
}
