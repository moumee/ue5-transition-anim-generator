#pragma once

#include "CoreMinimal.h"

class UAnimSequence;

namespace MotionInbetweening
{
struct FMappingOptions
{
	FString PythonExecutable;
	FString StatisticsFile;
	int32 MaxSamples = 31;
	float MaxRootStepCm = 15.0f;
	float MaxJointStepDegrees = 60.0f;
	bool bOpenOutputFolder = true;
};

struct FMappingResult
{
	bool bSuccess = false;
	bool bNormalized = false;
	int32 SampleCount = 0;
	FString OutputDirectory;
	FString Error;
};

FMappingOptions GetMappingOptions();
bool StartMannyInputMapping(UAnimSequence* Animation, const FMappingOptions& Options, FString& OutDirectory, FString& OutError);
bool IsMannyInputMappingRunning();
const FMappingResult& GetLastMappingResult();
void CancelMannyInputMapping();
void ShutdownMannyInputMapping();
}
