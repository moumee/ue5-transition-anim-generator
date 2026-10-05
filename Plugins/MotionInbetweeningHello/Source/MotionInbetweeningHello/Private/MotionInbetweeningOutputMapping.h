#pragma once

#include "CoreMinimal.h"

namespace MotionInbetweening
{
struct FOutputMappingOptions
{
	FString PythonExecutable;
	FString StatisticsFile;
	bool bOpenAsset = true;
};
struct FOutputMappingResult
{
	bool bSuccess = false;
	FString OutputDirectory;
	FString AssetPath;
	FString Error;
	int32 SampleCount = 0;
};
FOutputMappingOptions GetOutputMappingOptions();
bool StartMannyOutputMapping(const FString& SourceFile, const FOutputMappingOptions& Options, FString& OutDirectory, FString& OutError);
bool IsMannyOutputMappingRunning();
const FOutputMappingResult& GetLastOutputMappingResult();
void CancelMannyOutputMapping();
}
