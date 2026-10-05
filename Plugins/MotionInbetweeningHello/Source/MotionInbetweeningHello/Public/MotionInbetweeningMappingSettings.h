#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Engine/EngineTypes.h"
#include "MotionInbetweeningMappingSettings.generated.h"

UCLASS(Config=EditorPerProjectUserSettings, meta=(DisplayName="Motion In-betweening"))
class UMotionInbetweeningMappingSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	virtual FName GetContainerName() const override { return TEXT("Project"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	virtual FName GetSectionName() const override { return TEXT("MannyInputMapping"); }

	// User-local paths are saved under Saved/Config, not shared project configuration.
	UPROPERTY(Config, EditAnywhere, Category="Python", meta=(FilePathFilter="exe", DisplayName="Python Executable", ToolTip="Python 3.11 or later with the team mapping requirements installed (NumPy)."))
	FFilePath PythonExecutable;

	UPROPERTY(Config, EditAnywhere, Category="Model", meta=(FilePathFilter="pkl", DisplayName="Training Statistics", ToolTip="Statistics for the model in use. Optional for input export; required when importing normalized model output. Ignored for unnormalized output."))
	FFilePath TrainingStatistics;

	UPROPERTY(Config, EditAnywhere, Category="Sampling", meta=(ClampMin="10", ClampMax="300", DisplayName="Maximum Samples", ToolTip="Export from the beginning at 30 fps. The context length is 10. 31 samples cover one second."))
	int32 MaxSamples = 31;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category="Validation", meta=(ClampMin="0.01", DisplayName="Maximum Root Step (cm per sample)"))
	float MaxRootStepCm = 15.0f;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category="Validation", meta=(ClampMin="0.01", DisplayName="Maximum Joint Step (degrees per sample)"))
	float MaxJointStepDegrees = 60.0f;

	UPROPERTY(Config, EditAnywhere, Category="Output")
	bool bOpenOutputFolder = true;
};
