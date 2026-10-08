#include "MotionInbetweeningInference.h"

#include "MotionInbetweeningHelloModule.h"
#include "MotionInbetweeningMappingSettings.h"
#include "MotionInbetweeningOutputMapping.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
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
#include "Widgets/Notifications/SNotificationList.h"

namespace MotionInbetweening
{
namespace
{
FProcHandle InferenceProcess;
FTSTicker::FDelegateHandle InferenceTicker;
FInferenceResult LastInference;
FInferenceOptions ActiveOptions;
bool bRunning = false;
double StartedAt = 0;
TWeakPtr<SNotificationItem> ProgressNotification;

bool SaveJson(const FString& Path, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void RecordStatus()
{
	if (LastInference.OutputDirectory.IsEmpty()) return;
	TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
	Record->SetBoolField(TEXT("success"), LastInference.bSuccess);
	Record->SetStringField(TEXT("stage"), LastInference.Stage);
	Record->SetStringField(TEXT("request_id"), LastInference.RequestId);
	Record->SetStringField(TEXT("import_directory"), LastInference.ImportDirectory);
	Record->SetStringField(TEXT("asset_path"), LastInference.AssetPath);
	Record->SetStringField(TEXT("error"), LastInference.Error);
	Record->SetNumberField(TEXT("sample_count"), LastInference.SampleCount);
	if (!SaveJson(LastInference.OutputDirectory / TEXT("pipeline_result.json"), Record))
		UE_LOG(LogMotionInbetweeningHello, Warning, TEXT("Could not write inference receipt: %s"), *LastInference.OutputDirectory);
}

void UpdateNotification(const FString& Text, bool bFinished)
{
	if (auto Item = ProgressNotification.Pin())
	{
		Item->SetText(FText::FromString(Text.Left(700)));
		if (bFinished)
		{
			Item->SetCompletionState(LastInference.bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
			Item->ExpireAndFadeout();
			ProgressNotification.Reset();
		}
	}
}

void CloseWorker(bool bTerminate)
{
	if (!InferenceProcess.IsValid()) return;
	if (bTerminate) FPlatformProcess::TerminateProc(InferenceProcess, true);
	FPlatformProcess::CloseProc(InferenceProcess);
	InferenceProcess.Reset();
}

void Finish(const FString& Stage, const FString& Error = FString())
{
	bRunning = false;
	LastInference.Stage = Stage;
	LastInference.Error = Error;
	LastInference.bSuccess = Stage == TEXT("completed");
	if (!LastInference.bSuccess) { LastInference.AssetPath.Reset(); LastInference.SampleCount = 0; }
	RecordStatus();
	const FString Message = LastInference.bSuccess ? TEXT("Manny transition saved: ") + LastInference.AssetPath
		: TEXT("Manny inference failed: ") + Error;
	if (Stage == TEXT("failed"))
	{
		UE_LOG(LogMotionInbetweeningHello, Error, TEXT("%s | %s"), *Message, *LastInference.OutputDirectory);
	}
	else
	{
		UE_LOG(LogMotionInbetweeningHello, Display, TEXT("%s | %s"), *Message, *LastInference.OutputDirectory);
	}
	UpdateNotification(Message, true);
}

bool TickInference(float)
{
	if (!bRunning) return false;
	if (LastInference.Stage == TEXT("importing"))
	{
		if (IsMannyOutputMappingRunning()) return true;
		const auto& Result = GetLastOutputMappingResult();
		if (Result.OutputDirectory != LastInference.ImportDirectory)
			Finish(TEXT("failed"), TEXT("Output import result does not belong to this inference job."));
		else if (!Result.bSuccess) Finish(TEXT("failed"), Result.Error);
		else
		{
			LastInference.AssetPath = Result.AssetPath;
			LastInference.SampleCount = Result.SampleCount;
			Finish(TEXT("completed"));
		}
		InferenceTicker.Reset();
		return false;
	}
	if (FPlatformProcess::IsProcRunning(InferenceProcess))
	{
		if (FPlatformTime::Seconds() - StartedAt < ActiveOptions.TimeoutSeconds) return true;
		CloseWorker(true);
		Finish(TEXT("failed"), FString::Printf(TEXT("Inference timed out after %d seconds."), ActiveOptions.TimeoutSeconds));
		InferenceTicker.Reset();
		return false;
	}
	int32 ExitCode = -1;
	FPlatformProcess::GetProcReturnCode(InferenceProcess, &ExitCode);
	CloseWorker(false);
	FString Text, Error, RequestId;
	TSharedPtr<FJsonObject> Result;
	bool bSucceeded = false;
	if (FFileHelper::LoadFileToString(Text, *(LastInference.OutputDirectory / TEXT("result.json")))
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result) && Result)
	{
		Result->TryGetBoolField(TEXT("success"), bSucceeded);
		Result->TryGetStringField(TEXT("error"), Error);
		Result->TryGetStringField(TEXT("request_id"), RequestId);
	}
	if (ExitCode != 0 || !bSucceeded || RequestId != LastInference.RequestId)
	{
		if (Error.IsEmpty()) Error = FString::Printf(TEXT("Inference worker exited with code %d or returned an invalid receipt. Inspect the job folder."), ExitCode);
		Finish(TEXT("failed"), Error);
		InferenceTicker.Reset();
		return false;
	}
	FOutputMappingOptions Import;
	Import.PythonExecutable = ActiveOptions.MappingPythonExecutable;
	Import.StatisticsFile = ActiveOptions.StatisticsFile;
	Import.bOpenAsset = ActiveOptions.bOpenAsset;
	Import.bFromInference = true;
	if (!StartMannyOutputMapping(LastInference.OutputDirectory / TEXT("model_output.json"), Import, LastInference.ImportDirectory, Error))
	{
		Finish(TEXT("failed"), Error);
		InferenceTicker.Reset();
		return false;
	}
	LastInference.Stage = TEXT("importing");
	RecordStatus();
	UpdateNotification(TEXT("Model output validated. Creating Manny animation..."), false);
	return true;
}
}

FInferenceOptions GetInferenceOptions()
{
	const auto* Settings = GetDefault<UMotionInbetweeningMappingSettings>();
	FInferenceOptions Options;
	Options.MappingPythonExecutable = Settings->PythonExecutable.FilePath;
	Options.InferencePythonExecutable = Settings->InferencePythonExecutable.FilePath.IsEmpty()
		? Options.MappingPythonExecutable : Settings->InferencePythonExecutable.FilePath;
	Options.InferenceScript = Settings->InferenceScript.FilePath;
	Options.ModelConfigFile = Settings->InferenceModelConfig.FilePath;
	Options.StatisticsFile = Settings->TrainingStatistics.FilePath;
	Options.TransitionFrames = Settings->TransitionFrames;
	Options.TimeoutSeconds = Settings->InferenceTimeoutSeconds;
	return Options;
}

bool StartMannyInference(const FString& StartInput, const FString& EndInput, const FInferenceOptions& Options,
	FString& OutDirectory, FString& OutError)
{
	OutDirectory.Reset(); OutError.Reset();
	if (bRunning || IsMannyOutputMappingRunning())
	{
		OutError = TEXT("Wait for or cancel the current inference/output import first.");
		return false;
	}
	LastInference = FInferenceResult();
	const auto FailStart = [&OutError](const FString& Error)
	{
		OutError = Error;
		LastInference.Stage = TEXT("failed");
		LastInference.Error = Error;
		RecordStatus();
		return false;
	};
	if (!FPaths::FileExists(Options.MappingPythonExecutable) || !FPaths::FileExists(Options.InferencePythonExecutable))
		return FailStart(TEXT("Configure the mapping Python and inference Python executables in Motion In-betweening Settings."));
	if (!FPaths::FileExists(Options.InferenceScript) || !FPaths::FileExists(Options.ModelConfigFile))
		return FailStart(TEXT("Set the team's Inference Script and Model Config JSON in Motion In-betweening Settings. The model entry point has not been bundled."));
	if (!FPaths::FileExists(StartInput) || !FPaths::FileExists(EndInput))
		return FailStart(TEXT("Select exported model_input.json files for the start and destination motions."));
	if (Options.TransitionFrames < 1 || Options.TransitionFrames > 240 || Options.TimeoutSeconds < 1 || Options.TimeoutSeconds > 3600)
		return FailStart(TEXT("Use 1-240 transition samples and a 1-3600 second inference timeout."));
	const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("MotionInbetweeningHello"));
	if (!Plugin) return FailStart(TEXT("Cannot locate MotionInbetweeningHello."));
	const FString Worker = FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir() / TEXT("Scripts/run_manny_inference.py"));
	if (!FPaths::FileExists(Worker)) return FailStart(TEXT("The inference bridge script is missing."));
	LastInference.RequestId = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	OutDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/Inference") / LastInference.RequestId);
	if (!IFileManager::Get().MakeDirectory(*OutDirectory, true)) return FailStart(TEXT("Cannot create the inference job directory."));
	LastInference.OutputDirectory = OutDirectory;
	if (IFileManager::Get().Copy(*(OutDirectory / TEXT("start_input.json")), *StartInput, false) != COPY_OK
		|| IFileManager::Get().Copy(*(OutDirectory / TEXT("end_input.json")), *EndInput, false) != COPY_OK)
		return FailStart(TEXT("Cannot preserve the selected model inputs in the job folder."));
	TSharedRef<FJsonObject> Job = MakeShared<FJsonObject>();
	Job->SetStringField(TEXT("request_id"), LastInference.RequestId);
	Job->SetStringField(TEXT("output_directory"), OutDirectory);
	Job->SetStringField(TEXT("mapping_directory"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Tools/MannyLafanMapping")));
	Job->SetStringField(TEXT("inference_python"), FPaths::ConvertRelativePathToFull(Options.InferencePythonExecutable));
	Job->SetStringField(TEXT("inference_script"), FPaths::ConvertRelativePathToFull(Options.InferenceScript));
	Job->SetStringField(TEXT("model_config_file"), FPaths::ConvertRelativePathToFull(Options.ModelConfigFile));
	Job->SetStringField(TEXT("statistics_file"), Options.StatisticsFile);
	Job->SetNumberField(TEXT("transition_frames"), Options.TransitionFrames);
	const FString JobFile = OutDirectory / TEXT("job.json");
	if (!SaveJson(JobFile, Job)) return FailStart(TEXT("Cannot write the inference job."));
	const FString Arguments = FString::Printf(TEXT("-X utf8 -B \"%s\" --job \"%s\""), *Worker, *JobFile);
	InferenceProcess = FPlatformProcess::CreateProc(*Options.MappingPythonExecutable, *Arguments,
		false, true, true, nullptr, 0, *FPaths::ProjectDir(), nullptr);
	if (!InferenceProcess.IsValid()) return FailStart(TEXT("Cannot start the mapping Python executable."));
	ActiveOptions = Options;
	StartedAt = FPlatformTime::Seconds();
	bRunning = true;
	LastInference.Stage = TEXT("running");
	RecordStatus();
	InferenceTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickInference), 0.1f);
	if (!IsRunningCommandlet() && FSlateApplication::IsInitialized())
	{
		FNotificationInfo Info(FText::FromString(TEXT("Running transition inference... Cancel from the Tools menu.")));
		Info.bFireAndForget = false;
		Info.ExpireDuration = 8;
		ProgressNotification = FSlateNotificationManager::Get().AddNotification(Info);
		if (auto Item = ProgressNotification.Pin()) Item->SetCompletionState(SNotificationItem::CS_Pending);
	}
	UE_LOG(LogMotionInbetweeningHello, Display, TEXT("Manny inference started: %s"), *OutDirectory);
	return true;
}

bool IsMannyInferenceRunning() { return bRunning; }
const FInferenceResult& GetLastInferenceResult() { return LastInference; }

void CancelMannyInference()
{
	if (!bRunning) return;
	if (InferenceTicker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(InferenceTicker); InferenceTicker.Reset(); }
	// Import can finish between its ticker and ours. Preserve the real saved
	// asset/failure instead of replacing that completed result with cancellation.
	if (LastInference.Stage == TEXT("importing") && !IsMannyOutputMappingRunning())
	{
		TickInference(0);
		return;
	}
	CloseWorker(true);
	if (LastInference.Stage == TEXT("importing") && IsMannyOutputMappingRunning()
		&& GetLastOutputMappingResult().OutputDirectory == LastInference.ImportDirectory)
		CancelMannyOutputMapping();
	Finish(TEXT("cancelled"), TEXT("Inference was cancelled."));
}
}
