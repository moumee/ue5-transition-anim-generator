#include "MotionInbetweeningMapping.h"

#include "MotionInbetweeningHelloModule.h"
#include "MotionInbetweeningMappingSettings.h"
#include "Animation/AnimSequence.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "IPythonScriptPlugin.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace MotionInbetweening
{
namespace
{
FProcHandle WorkerProcess;
FTSTicker::FDelegateHandle WorkerTicker;
FMappingResult LastResult;
double StartTime = 0;
bool bOpenFolderOnSuccess = false;

void Notify(const FString& Message, bool bSuccess)
{
	if (IsRunningCommandlet() || !FSlateApplication::IsInitialized()) return;
	FNotificationInfo Info(FText::FromString(Message));
	Info.ExpireDuration = bSuccess ? 7.0f : 12.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

FString PythonString(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("\\\\"));
	Value.ReplaceInline(TEXT("'"), TEXT("\\'"));
	Value.ReplaceInline(TEXT("\r"), TEXT("\\r"));
	Value.ReplaceInline(TEXT("\n"), TEXT("\\n"));
	return TEXT("'") + Value + TEXT("'");
}

bool ReadJson(const FString& Filename, TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	return FFileHelper::LoadFileToString(Text, *Filename)
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
}

bool TickMapping(float)
{
	if (!WorkerProcess.IsValid()) return false;
	if (FPlatformProcess::IsProcRunning(WorkerProcess))
	{
		if (FPlatformTime::Seconds() - StartTime < 120.0) return true;
		FPlatformProcess::TerminateProc(WorkerProcess, true);
		LastResult.Error = TEXT("Mapping timed out after 120 seconds. See the job folder and verify the Python environment.");
	}
	else
	{
		int32 ExitCode = -1;
		FPlatformProcess::GetProcReturnCode(WorkerProcess, &ExitCode);
		TSharedPtr<FJsonObject> Result;
		bool bWorkerSucceeded = false;
		if (ReadJson(LastResult.OutputDirectory / TEXT("result.json"), Result))
		{
			Result->TryGetBoolField(TEXT("success"), bWorkerSucceeded);
			Result->TryGetStringField(TEXT("error"), LastResult.Error);
			Result->TryGetBoolField(TEXT("normalization_applied"), LastResult.bNormalized);
			double SampleCount = 0;
			Result->TryGetNumberField(TEXT("sample_count"), SampleCount);
			LastResult.SampleCount = static_cast<int32>(SampleCount);
		}
		LastResult.bSuccess = ExitCode == 0 && bWorkerSucceeded
			&& FPaths::FileExists(LastResult.OutputDirectory / TEXT("model_input.json"));
		if (!LastResult.bSuccess && LastResult.Error.IsEmpty())
		{
			LastResult.Error = FString::Printf(TEXT("Python mapping exited with code %d. Check the configured Python executable and the job folder."), ExitCode);
		}
	}
	FPlatformProcess::CloseProc(WorkerProcess);
	WorkerProcess.Reset();
	WorkerTicker.Reset();
	if (LastResult.bSuccess)
	{
		const FString Message = FString::Printf(TEXT("Manny input saved: %d samples x 135 (%s)."),
			LastResult.SampleCount, LastResult.bNormalized ? TEXT("normalized") : TEXT("aligned, not normalized"));
		UE_LOG(LogMotionInbetweeningHello, Display, TEXT("%s %s"), *Message, *LastResult.OutputDirectory);
		Notify(Message, true);
		if (bOpenFolderOnSuccess && !IsRunningCommandlet()) FPlatformProcess::ExploreFolder(*LastResult.OutputDirectory);
	}
	else
	{
		UE_LOG(LogMotionInbetweeningHello, Error, TEXT("Manny input mapping failed: %s | %s"), *LastResult.Error, *LastResult.OutputDirectory);
		Notify(TEXT("Manny input mapping failed: ") + LastResult.Error.Left(500), false);
	}
	return false;
}
}

FMappingOptions GetMappingOptions()
{
	const auto* Settings = GetDefault<UMotionInbetweeningMappingSettings>();
	FMappingOptions Options;
	Options.PythonExecutable = Settings->PythonExecutable.FilePath;
	Options.StatisticsFile = Settings->TrainingStatistics.FilePath;
	Options.MaxSamples = Settings->MaxSamples;
	Options.MaxRootStepCm = Settings->MaxRootStepCm;
	Options.MaxJointStepDegrees = Settings->MaxJointStepDegrees;
	Options.bOpenOutputFolder = Settings->bOpenOutputFolder;
	return Options;
}

bool StartMannyInputMapping(UAnimSequence* Animation, const FMappingOptions& Options, FString& OutDirectory, FString& OutError)
{
	OutDirectory.Reset();
	OutError.Reset();
	if (IsMannyInputMappingRunning()) { OutError = TEXT("An input export is already running."); return false; }
	if (!Animation) { OutError = TEXT("Select one Manny Animation Sequence in the Content Browser."); return false; }
	if (!FPaths::FileExists(Options.PythonExecutable))
	{
		OutError = TEXT("Set Python Executable in Project Settings > Plugins > Motion In-betweening (Python 3.11+ with NumPy).");
		return false;
	}
	if (!Options.StatisticsFile.IsEmpty() && !FPaths::FileExists(Options.StatisticsFile))
	{
		OutError = TEXT("The training statistics file does not exist. Select the model's train_stats_context.pkl, or clear it for unnormalized output.");
		return false;
	}
	if (Options.MaxSamples < 10 || Options.MaxSamples > 300 || !FMath::IsFinite(Options.MaxRootStepCm)
		|| !FMath::IsFinite(Options.MaxJointStepDegrees) || Options.MaxRootStepCm <= 0 || Options.MaxJointStepDegrees <= 0)
	{
		OutError = TEXT("Use 10-300 samples and positive, finite continuity limits in the mapping settings.");
		return false;
	}
	if (FMath::FloorToInt(Animation->GetPlayLength() * 30.0) + 1 < 10)
	{
		OutError = TEXT("The animation is too short for a 10-frame context at 30 fps.");
		return false;
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/ControlRig/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple"));
	if (!Mesh || !Mesh->GetSkeleton() || Animation->GetSkeleton() != Mesh->GetSkeleton())
	{
		OutError = TEXT("The selected animation must use this project's Manny skeleton.");
		return false;
	}
	const FString MappingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Tools/MannyLafanMapping"));
	if (!FPaths::FileExists(MappingDirectory / TEXT("convert_manny_sequence_135.py")))
	{
		OutError = TEXT("The team mapping tools are missing from Tools/MannyLafanMapping. Fetch the input-mapping branch first.");
		return false;
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MotionInbetweeningHello"));
	if (!Plugin.IsValid()) { OutError = TEXT("Cannot locate the MotionInbetweeningHello plugin."); return false; }
	const FString Scripts = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Scripts"));
	const FString ExportScript = Scripts / TEXT("export_manny_input.py");
	const FString WorkerScript = Scripts / TEXT("prepare_manny_input.py");
	if (!FPaths::FileExists(ExportScript) || !FPaths::FileExists(WorkerScript))
	{
		OutError = TEXT("The plugin's mapping adapter scripts are missing.");
		return false;
	}
	IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
	if (!Python || (!Python->IsPythonInitialized() && !Python->ForceEnablePythonAtRuntime()) || !Python->IsPythonInitialized())
	{
		OutError = TEXT("Unreal Python could not start. Check the Python Script Plugin in the Output Log.");
		return false;
	}
	OutDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/InputMapping") /
		(Animation->GetName() + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	if (!IFileManager::Get().MakeDirectory(*OutDirectory, true))
	{
		OutError = TEXT("Could not create the input export directory.");
		return false;
	}
	TSharedRef<FJsonObject> Job = MakeShared<FJsonObject>();
	Job->SetStringField(TEXT("animation"), Animation->GetPathName());
	Job->SetStringField(TEXT("mapping_directory"), MappingDirectory);
	Job->SetStringField(TEXT("output_directory"), OutDirectory);
	Job->SetStringField(TEXT("statistics_file"), Options.StatisticsFile);
	Job->SetNumberField(TEXT("max_samples"), Options.MaxSamples);
	Job->SetNumberField(TEXT("max_root_step_cm"), Options.MaxRootStepCm);
	Job->SetNumberField(TEXT("max_joint_step_degrees"), Options.MaxJointStepDegrees);
	const FString JobFile = OutDirectory / TEXT("job.json");
	FString JobText;
	FJsonSerializer::Serialize(Job, TJsonWriterFactory<>::Create(&JobText));
	if (!FFileHelper::SaveStringToFile(JobText, *JobFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("Could not write the input export job.");
		return false;
	}
	FPythonCommandEx Command;
	Command.Flags |= EPythonCommandFlags::Unattended;
	Command.Command = TEXT("import runpy; runpy.run_path(") + PythonString(ExportScript)
		+ TEXT(")[\"export_job\"](") + PythonString(JobFile) + TEXT(")");
	if (!Python->ExecPythonCommandEx(Command))
	{
		OutError = TEXT("Manny pose extraction failed. The Output Log contains the Python error.");
		return false;
	}
	const FString Arguments = FString::Printf(TEXT("\"%s\" --job \"%s\""), *WorkerScript, *JobFile);
	WorkerProcess = FPlatformProcess::CreateProc(*Options.PythonExecutable, *Arguments,
		false, true, true, nullptr, 0, *FPaths::ProjectDir(), nullptr);
	if (!WorkerProcess.IsValid())
	{
		OutError = TEXT("Could not start the configured Python executable.");
		return false;
	}
	LastResult = FMappingResult();
	LastResult.OutputDirectory = OutDirectory;
	bOpenFolderOnSuccess = Options.bOpenOutputFolder;
	StartTime = FPlatformTime::Seconds();
	WorkerTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickMapping), 0.1f);
	UE_LOG(LogMotionInbetweeningHello, Display, TEXT("Manny input mapping started: %s | %s"), *Animation->GetPathName(), *OutDirectory);
	Notify(TEXT("Preparing Manny model input..."), true);
	return true;
}

bool IsMannyInputMappingRunning() { return WorkerProcess.IsValid(); }
const FMappingResult& GetLastMappingResult() { return LastResult; }

void CancelMannyInputMapping()
{
	if (WorkerTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(WorkerTicker); WorkerTicker.Reset(); }
	if (WorkerProcess.IsValid())
	{
		FPlatformProcess::TerminateProc(WorkerProcess, true);
		FPlatformProcess::CloseProc(WorkerProcess);
		WorkerProcess.Reset();
		LastResult.bSuccess = false;
		LastResult.Error = TEXT("Input mapping was cancelled.");
		UE_LOG(LogMotionInbetweeningHello, Display, TEXT("%s %s"), *LastResult.Error, *LastResult.OutputDirectory);
	}
}

void ShutdownMannyInputMapping() { CancelMannyInputMapping(); }
}
