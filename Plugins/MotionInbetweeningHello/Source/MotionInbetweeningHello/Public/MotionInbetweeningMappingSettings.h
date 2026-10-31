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

	UPROPERTY(Config, EditAnywhere, Category="Inference", meta=(FilePathFilter="py", DisplayName="Inference Script", ToolTip="Team-provided Python entry point accepting --request request.json. See Tools/MotionInference/README.md."))
	FFilePath InferenceScript;

	UPROPERTY(Config, EditAnywhere, Category="Inference", meta=(FilePathFilter="json", DisplayName="Model Config", ToolTip="Backend-specific JSON with checkpoint paths and model options. The backend resolves relative model paths from this file's directory."))
	FFilePath InferenceModelConfig;

	UPROPERTY(Config, EditAnywhere, Category="Inference", meta=(FilePathFilter="exe", DisplayName="Inference Python", ToolTip="Python environment with the model dependencies. Leave empty to use Python Executable above."))
	FFilePath InferencePythonExecutable;

	UPROPERTY(Config, EditAnywhere, Category="Inference", meta=(ClampMin="1", ClampMax="240", DisplayName="Transition Samples", ToolTip="Samples strictly between the final context sample and destination target. Output has 10 context + this count + 1 target samples."))
	int32 TransitionFrames = 20;

	UPROPERTY(Config, EditAnywhere, Category="Inference", meta=(ClampMin="1", ClampMax="3600", DisplayName="Inference Timeout (seconds)"))
	int32 InferenceTimeoutSeconds = 300;

	UPROPERTY(Config, EditAnywhere, Category="Sampling", meta=(ClampMin="10", ClampMax="300", DisplayName="Maximum Samples", ToolTip="Export from the beginning at 30 fps. The context length is 10. 31 samples cover one second."))
	int32 MaxSamples = 31;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category="Validation", meta=(ClampMin="0.01", DisplayName="Maximum Root Step (cm per sample)"))
	float MaxRootStepCm = 15.0f;

	UPROPERTY(Config, EditAnywhere, AdvancedDisplay, Category="Validation", meta=(ClampMin="0.01", DisplayName="Maximum Joint Step (degrees per sample)"))
	float MaxJointStepDegrees = 60.0f;

	UPROPERTY(Config, EditAnywhere, Category="Output")
	bool bOpenOutputFolder = true;
};
