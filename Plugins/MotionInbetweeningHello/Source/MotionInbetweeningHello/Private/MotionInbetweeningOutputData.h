#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
namespace MotionInbetweening
{
struct FAnimationSamples;
inline constexpr const TCHAR* OutputMannyMeshPath = TEXT("/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple");
bool ExportMannyReferenceSkeleton(const USkeletalMesh& Mesh, const FString& Filename, FString& OutError);
bool LoadMannyLocalPose(const FString& Filename, const USkeletalMesh& Mesh, FAnimationSamples& OutData, FString& OutError);
}
