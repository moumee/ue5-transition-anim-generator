#pragma once

#include "CoreMinimal.h"

namespace MotionInbetweening
{
struct FInferenceOptions
{
	FString MappingPythonExecutable;
	FString InferencePythonExecutable;
	FString InferenceScript;
	FString ModelConfigFile;
	FString StatisticsFile;
	int32 TransitionFrames = 20;
	int32 TimeoutSeconds = 300;
	bool bOpenAsset = true;
};

struct FInferenceResult
{
	bool bSuccess = false;
	FString Stage = TEXT("idle");
	FString RequestId;
	FString OutputDirectory;
	FString ImportDirectory;
	FString AssetPath;
	FString Error;
	int32 SampleCount = 0;
};

FInferenceOptions GetInferenceOptions();
bool StartMannyInference(const FString& StartInput, const FString& EndInput, const FInferenceOptions& Options,
	FString& OutDirectory, FString& OutError);
bool IsMannyInferenceRunning();
const FInferenceResult& GetLastInferenceResult();
void CancelMannyInference();
}
