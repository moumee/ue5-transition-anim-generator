#include "MotionInbetweeningOutputMapping.h"

#include "MotionInbetweeningAnimationData.h"
#include "MotionInbetweeningAnimSequence.h"
#include "MotionInbetweeningHelloModule.h"
#include "MotionInbetweeningInference.h"
#include "MotionInbetweeningMappingSettings.h"
#include "MotionInbetweeningOutputData.h"
#include "Animation/AnimSequence.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace MotionInbetweening
{
namespace
{
FProcHandle OutputProcess;
FTSTicker::FDelegateHandle OutputTicker;
FOutputMappingResult LastOutput;
double StartTime = 0;
bool bOpenAssetOnSuccess = false;

bool SaveJson(const FString& Filename, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void RecordOutcome()
{
	TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
	Record->SetBoolField(TEXT("success"), LastOutput.bSuccess);
	Record->SetStringField(TEXT("asset_path"), LastOutput.AssetPath);
	Record->SetStringField(TEXT("error"), LastOutput.Error);
	Record->SetNumberField(TEXT("sample_count"), LastOutput.SampleCount);
	if (!SaveJson(LastOutput.OutputDirectory / TEXT("asset_result.json"), Record))
		UE_LOG(LogMotionInbetweeningHello, Warning, TEXT("Could not write output import receipt: %s"), *LastOutput.OutputDirectory);
}

bool TickOutputMapping(float)
{
	if (!OutputProcess.IsValid()) return false;
	if (FPlatformProcess::IsProcRunning(OutputProcess))
	{
		if (FPlatformTime::Seconds() - StartTime < 120) return true;
		FPlatformProcess::TerminateProc(OutputProcess, true);
		LastOutput.Error = TEXT("Output conversion timed out after 120 seconds.");
	}
	else
	{
		int32 ExitCode = -1;
		FPlatformProcess::GetProcReturnCode(OutputProcess, &ExitCode);
		FString Text;
		TSharedPtr<FJsonObject> Result;
		bool bSucceeded = false;
		if (FFileHelper::LoadFileToString(Text, *(LastOutput.OutputDirectory / TEXT("result.json")))
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result) && Result)
		{
			Result->TryGetBoolField(TEXT("success"), bSucceeded);
			Result->TryGetStringField(TEXT("error"), LastOutput.Error);
		}
		if (ExitCode == 0 && bSucceeded)
		{
			USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, OutputMannyMeshPath);
			FAnimationSamples Data;
			if (!Mesh) LastOutput.Error = TEXT("Cannot load the Manny output preview mesh.");
			else if (LoadMannyLocalPose(LastOutput.OutputDirectory / TEXT("manny_pose.json"), *Mesh, Data, LastOutput.Error))
			{
				if (UAnimSequence* Animation = CreateAndSaveAnimation(*Mesh, Data, LastOutput.Error,
					TEXT("/Game/MotionInbetweening/Generated/AN_Manny_Output")))
				{
					LastOutput.bSuccess = true;
					LastOutput.AssetPath = Animation->GetPathName();
					LastOutput.SampleCount = Data.NumSamples;
					if (bOpenAssetOnSuccess && !IsRunningCommandlet() && GEditor)
					{
						if (auto* Editor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()) Editor->OpenEditorForAsset(Animation);
					}
				}
			}
		}
		else if (LastOutput.Error.IsEmpty())
		{
			LastOutput.Error = FString::Printf(TEXT("Output worker exited with code %d. Inspect the job folder and Python setting."), ExitCode);
		}
	}
	FPlatformProcess::CloseProc(OutputProcess);
	OutputProcess.Reset();
	OutputTicker.Reset();
	RecordOutcome();
	const FString Message = LastOutput.bSuccess ? TEXT("Manny animation saved: ") + LastOutput.AssetPath
		: TEXT("Manny output import failed: ") + LastOutput.Error;
	if (LastOutput.bSuccess)
	{
		UE_LOG(LogMotionInbetweeningHello, Display, TEXT("%s | %s"), *Message, *LastOutput.OutputDirectory);
	}
	else
	{
		UE_LOG(LogMotionInbetweeningHello, Error, TEXT("%s | %s"), *Message, *LastOutput.OutputDirectory);
	}
	if (!IsRunningCommandlet() && FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(Message.Left(700)));
		Info.ExpireDuration = LastOutput.bSuccess ? 7 : 12;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
	return false;
}
}

FOutputMappingOptions GetOutputMappingOptions()
{
	const auto* Settings = GetDefault<UMotionInbetweeningMappingSettings>();
	FOutputMappingOptions Options;
	Options.PythonExecutable = Settings->PythonExecutable.FilePath;
	Options.StatisticsFile = Settings->TrainingStatistics.FilePath;
	return Options;
}

bool StartMannyOutputMapping(const FString& SourceFile, const FOutputMappingOptions& Options, FString& OutDirectory, FString& OutError)
{
	OutDirectory.Reset(); OutError.Reset();
	if (IsMannyOutputMappingRunning()) { OutError = TEXT("An output import is already running."); return false; }
	if (IsMannyInferenceRunning() && !Options.bFromInference) { OutError = TEXT("The inference pipeline currently owns output importing."); return false; }
	LastOutput = FOutputMappingResult();
	const auto FailStart = [&OutError](const FString& Error)
	{
		OutError = Error;
		LastOutput.Error = Error;
		if (!LastOutput.OutputDirectory.IsEmpty()) RecordOutcome();
		return false;
	};
	if (!FPaths::FileExists(Options.PythonExecutable))
	{
		return FailStart(TEXT("Set Python Executable in Motion In-betweening Settings (Python 3.11+ with NumPy)."));
	}
	if (!FPaths::FileExists(SourceFile)) return FailStart(TEXT("Select an existing model-output JSON file."));
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, OutputMannyMeshPath);
	if (!Mesh || !Mesh->GetSkeleton()) return FailStart(TEXT("The project's Manny Simple mesh and skeleton are required."));
	const FString Mapping = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Tools/MannyLafanMapping"));
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("MotionInbetweeningHello"));
	if (!Plugin || !FPaths::FileExists(Mapping / TEXT("convert_lafan_output_to_manny.py")))
	{
		return FailStart(TEXT("The plugin or team's LAFAN-to-Manny mapping tools are missing."));
	}
	const FString Worker = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Scripts/prepare_manny_output.py"));
	if (!FPaths::FileExists(Worker)) return FailStart(TEXT("The output adapter script is missing."));
	OutDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/OutputMapping") /
		FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!IFileManager::Get().MakeDirectory(*OutDirectory, true)) return FailStart(TEXT("Cannot create the output job directory."));
	LastOutput.OutputDirectory = OutDirectory;
	if (IFileManager::Get().Copy(*(OutDirectory / TEXT("source_model_output.json")), *SourceFile, false) != COPY_OK)
	{
		return FailStart(TEXT("Cannot preserve a copy of the model output in the job folder."));
	}
	if (!ExportMannyReferenceSkeleton(*Mesh, OutDirectory / TEXT("skeleton.json"), OutError)) return FailStart(OutError);
	TSharedRef<FJsonObject> Job = MakeShared<FJsonObject>();
	Job->SetStringField(TEXT("source_file"), FPaths::ConvertRelativePathToFull(SourceFile));
	Job->SetStringField(TEXT("mapping_directory"), Mapping);
	Job->SetStringField(TEXT("output_directory"), OutDirectory);
	Job->SetStringField(TEXT("statistics_file"), Options.StatisticsFile);
	const FString JobFile = OutDirectory / TEXT("job.json");
	if (!SaveJson(JobFile, Job)) return FailStart(TEXT("Cannot write the output job settings."));
	const FString Arguments = FString::Printf(TEXT("\"%s\" --job \"%s\""), *Worker, *JobFile);
	OutputProcess = FPlatformProcess::CreateProc(*Options.PythonExecutable, *Arguments,
		false, true, true, nullptr, 0, *FPaths::ProjectDir(), nullptr);
	if (!OutputProcess.IsValid()) return FailStart(TEXT("Cannot start the configured Python executable."));
	bOpenAssetOnSuccess = Options.bOpenAsset;
	StartTime = FPlatformTime::Seconds();
	OutputTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickOutputMapping), 0.1f);
	UE_LOG(LogMotionInbetweeningHello, Display, TEXT("Manny output import started: %s | %s"), *SourceFile, *OutDirectory);
	return true;
}

bool IsMannyOutputMappingRunning() { return OutputProcess.IsValid(); }
const FOutputMappingResult& GetLastOutputMappingResult() { return LastOutput; }

void CancelMannyOutputMapping()
{
	if (OutputTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(OutputTicker); OutputTicker.Reset(); }
	if (OutputProcess.IsValid())
	{
		FPlatformProcess::TerminateProc(OutputProcess, true);
		FPlatformProcess::CloseProc(OutputProcess);
		OutputProcess.Reset();
		LastOutput.bSuccess = false;
		LastOutput.Error = TEXT("Output import was cancelled.");
		RecordOutcome();
		UE_LOG(LogMotionInbetweeningHello, Display, TEXT("%s | %s"), *LastOutput.Error, *LastOutput.OutputDirectory);
	}
}
}
