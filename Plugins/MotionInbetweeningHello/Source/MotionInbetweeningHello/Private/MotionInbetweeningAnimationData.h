#pragma once

#include "CoreMinimal.h"
#include "Misc/FrameRate.h"

class USkeleton;

namespace MotionInbetweening
{
// Integration boundary: absolute parent-local transforms in Unreal coordinates
// (centimeters, normalized XYZW quaternions), sampled uniformly including both endpoints.
// This is the demo's internal contract, not an agreed model interchange format.
struct FBoneSamples
{
	FName BoneName;
	TArray<FVector3f> Positions;
	TArray<FQuat4f> Rotations;
	TArray<FVector3f> Scales;
};

struct FAnimationSamples
{
	FFrameRate FrameRate = FFrameRate(30, 1);
	int32 NumSamples = 0;
	TArray<FBoneSamples> Bones;
};

// Replace this producer with mapped model results; keep the asset writer unchanged.
bool BuildMannyTestData(const USkeleton& Skeleton, FAnimationSamples& OutData, FString& OutError);
bool ValidateAnimationData(const USkeleton& Skeleton, const FAnimationSamples& Data, FString& OutError);
}
