#pragma once

#include "CoreMinimal.h"

class UAnimSequence;
class USkeletalMesh;

namespace MotionInbetweening
{
struct FAnimationSamples;

// Paths verified in this project's Content, deliberately Manny-specific for the demo.
inline constexpr const TCHAR* MannyMeshPath = TEXT("/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny.SKM_Manny");
inline constexpr const TCHAR* TestAssetFolder = TEXT("/Game/MotionInbetweening/Tests");

USkeletalMesh* LoadMannyMesh(FString& OutError);
UAnimSequence* CreateAndSaveAnimation(USkeletalMesh& PreviewMesh, const FAnimationSamples& Data, FString& OutError,
	const FString& AssetBasePath = TEXT("/Game/MotionInbetweening/Tests/AN_Manny_Test"));
}
