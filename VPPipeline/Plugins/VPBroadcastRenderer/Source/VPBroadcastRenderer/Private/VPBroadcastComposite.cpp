#include "VPBroadcastRenderer.h"

#include "Engine/TextureRenderTarget2D.h"
#include "GlobalShader.h"
#include "PixelShaderUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"

class FVPBroadcastCompositePS final : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVPBroadcastCompositePS);
	SHADER_USE_PARAMETER_STRUCT(FVPBroadcastCompositePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, AvatarTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, AvatarSampler)
		SHADER_PARAMETER(FVector2f, InverseOutputSize)
		SHADER_PARAMETER(FVector4f, BackgroundColor)
		SHADER_PARAMETER(float, ExposureScale)
		SHADER_PARAMETER(uint32, RemoveBackground)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(
	FVPBroadcastCompositePS,
	"/Plugin/VPBroadcastRenderer/Private/VPBroadcastComposite.usf",
	"MainPS",
	SF_Pixel);

namespace VPBroadcastRenderer
{
void EnqueueAvatarComposite(
	UTextureRenderTarget2D* AvatarTexture,
	UTextureRenderTarget2D* OutputTexture,
	const FLinearColor& BackgroundColor,
	float ExposureStops,
	bool bRemoveBackground,
	ERHIFeatureLevel::Type FeatureLevel)
{
	check(IsInGameThread());
	if (!AvatarTexture || !OutputTexture)
	{
		return;
	}

	FTextureRenderTargetResource* AvatarResource =
		AvatarTexture->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* OutputResource =
		OutputTexture->GameThread_GetRenderTargetResource();
	if (!AvatarResource || !OutputResource)
	{
		return;
	}

	const FIntPoint OutputSize(OutputResource->GetSizeX(), OutputResource->GetSizeY());
	const FVector4f LinearBackground(BackgroundColor);
	const float ExposureScale = FMath::Exp2(FMath::Clamp(ExposureStops, -2.0f, 2.0f));

	ENQUEUE_RENDER_COMMAND(VPBroadcastComposite)(
		[AvatarResource, OutputResource, OutputSize, LinearBackground, ExposureScale,
		bRemoveBackground, FeatureLevel]
		(FRHICommandListImmediate& RHICmdList)
		{
			if (!AvatarResource->TextureRHI.IsValid() ||
				!OutputResource->TextureRHI.IsValid() ||
				OutputSize.X <= 0 || OutputSize.Y <= 0)
			{
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGTextureRef AvatarRDG = GraphBuilder.RegisterExternalTexture(
				CreateRenderTarget(AvatarResource->TextureRHI, TEXT("VPAvatarHDR")));
			FRDGTextureRef OutputRDG = GraphBuilder.RegisterExternalTexture(
				CreateRenderTarget(OutputResource->TextureRHI, TEXT("VPBroadcastOutput")));

			FVPBroadcastCompositePS::FParameters* Parameters =
				GraphBuilder.AllocParameters<FVPBroadcastCompositePS::FParameters>();
			Parameters->AvatarTexture = AvatarRDG;
			Parameters->AvatarSampler =
				TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp>::GetRHI();
			Parameters->InverseOutputSize = FVector2f(
				1.0f / static_cast<float>(OutputSize.X),
				1.0f / static_cast<float>(OutputSize.Y));
			Parameters->BackgroundColor = LinearBackground;
			Parameters->ExposureScale = ExposureScale;
			Parameters->RemoveBackground = bRemoveBackground ? 1u : 0u;
			Parameters->RenderTargets[0] = FRenderTargetBinding(
				OutputRDG,
				ERenderTargetLoadAction::ENoAction);

			FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);
			TShaderMapRef<FVPBroadcastCompositePS> PixelShader(ShaderMap);
			FPixelShaderUtils::AddFullscreenPass(
				GraphBuilder,
				ShaderMap,
				RDG_EVENT_NAME("VP Broadcast Tone Composite %dx%d", OutputSize.X, OutputSize.Y),
				PixelShader,
				Parameters,
				FIntRect(FIntPoint::ZeroValue, OutputSize));
			GraphBuilder.Execute();
		});
}
}
