#pragma once

#include "CoreMinimal.h"
#include "MotionInbetweeningAnimationData.h"

class UAnimSequence;
class UAnimBlueprint;
class USkeletalMesh;
class FJsonObject;

namespace MotionInbetweening
{
struct FPlaybackCheckRequest
{
	UAnimSequence* Source = nullptr;
	UAnimSequence* Destination = nullptr;
	UAnimSequence* FullResult = nullptr;
	int32 TransitionFrames = 15;
	double DepartureSeconds = 1.0;
	double ArrivalSeconds = 0.0;
	int32 DestinationPreviewIntervals = 15;
};

struct FPlaybackCheckData
{
	FAnimationSamples SourceContext;
	FAnimationSamples Bridge;
	FAnimationSamples DestinationContext;
	FAnimationSamples Combined;
	TSharedPtr<FJsonObject> Diagnostics;
};

struct FPlaybackCheckResult
{
	UAnimSequence* Combined = nullptr;
	UAnimBlueprint* Blueprint = nullptr;
	FString ReportFile;
	FString Summary;
};

// Diagnostic producer only: never changes input clips or silently corrects a seam.
bool BuildPlaybackCheckData(const FPlaybackCheckRequest& Request, USkeletalMesh& Mesh,
	FPlaybackCheckData& OutData, FString& OutError);
bool CreatePlaybackCheck(const FPlaybackCheckRequest& Request, FPlaybackCheckResult& OutResult, FString& OutError);
UAnimBlueprint* CreatePlaybackPreviewBlueprint(USkeletalMesh& Mesh, UAnimSequence& Source,
	UAnimSequence& Bridge, UAnimSequence& Destination, FString& OutError);
void OpenPlaybackCheckWindow();
}
