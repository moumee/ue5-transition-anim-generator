#include "MotionInbetweeningHelloModule.h"

#include "MotionInbetweeningAnimationData.h"
#include "MotionInbetweeningAnimSequence.h"
#include "MotionInbetweeningMapping.h"
#include "MotionInbetweeningOutputMapping.h"
#include "MotionInbetweeningInference.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "Animation/AnimSequence.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "ISettingsModule.h"
#include "Editor.h"
#include "Engine/SkeletalMesh.h"
#include "Framework/Commands/UIAction.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Modules/ModuleManager.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY(LogMotionInbetweeningHello);

#define LOCTEXT_NAMESPACE "MotionInbetweeningHello"

void FMotionInbetweeningHelloModule::StartupModule()
{
	// ToolMenus calls RegisterMenus when the Editor UI is ready.
	UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMotionInbetweeningHelloModule::RegisterMenus));

	UE_LOG(LogMotionInbetweeningHello, Log, TEXT("[MotionInbetweeningHello] Module started."));
}

void FMotionInbetweeningHelloModule::ShutdownModule()
{
	MotionInbetweening::CancelMannyInference();
	MotionInbetweening::ShutdownMannyInputMapping();
	MotionInbetweening::CancelMannyOutputMapping();
	// Remove callbacks and owned entries before this module instance is destroyed.
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	UE_LOG(LogMotionInbetweeningHello, Log, TEXT("[MotionInbetweeningHello] Module shut down."));
}

void FMotionInbetweeningHelloModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);
	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	FToolMenuSection& Section = ToolsMenu->FindOrAddSection("MotionInbetweeningHello");
	Section.AddMenuEntry(
		"MotionInbetweeningHello.Test",
		LOCTEXT("TestMenuLabel", "Motion In-betweening Plugin Test"),
		LOCTEXT("TestMenuTooltip", "Show a test notification and write a message to the Output Log."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FMotionInbetweeningHelloModule::ExecuteTestCommand)));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.GenerateMannyAnimation",
		LOCTEXT("GenerateMenuLabel", "Generate Manny Test Animation"),
		LOCTEXT("GenerateMenuTooltip", "Create and save a new 2-second Manny animation from test data, then open its preview."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FMotionInbetweeningHelloModule::ExecuteGenerateAnimationCommand)));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.ExportMannyInput",
		LOCTEXT("ExportMannyInputLabel", "Export Selected Manny Model Input"),
		LOCTEXT("ExportMannyInputTooltip", "Export the selected Manny Animation Sequence through the team's input mapper. This prepares model input; it does not generate a transition."),
		FSlateIcon(),
		FUIAction(FExecuteAction::CreateRaw(this, &FMotionInbetweeningHelloModule::ExecuteExportMannyInputCommand),
			FCanExecuteAction::CreateLambda([] { return !MotionInbetweening::IsMannyInputMappingRunning(); })));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.MappingSettings",
		LOCTEXT("MappingSettingsLabel", "Motion In-betweening Settings"),
		LOCTEXT("MappingSettingsTooltip", "Set the Python executable, optional model statistics, and input sampling options."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FMotionInbetweeningHelloModule::ExecuteMappingSettingsCommand)));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.ImportMannyOutput",
		LOCTEXT("ImportMannyOutputLabel", "Import Model Output as Manny Animation"),
		LOCTEXT("ImportMannyOutputTooltip", "Select a model-output JSON, restore its Manny pose, save a new Animation Sequence and open the preview."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FMotionInbetweeningHelloModule::ExecuteImportMannyOutputCommand),
			FCanExecuteAction::CreateLambda([] { return !MotionInbetweening::IsMannyOutputMappingRunning() && !MotionInbetweening::IsMannyInferenceRunning(); })));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.RunInference",
		LOCTEXT("RunInferenceLabel", "Generate Manny Transition from Model Inputs"),
		LOCTEXT("RunInferenceTooltip", "Choose start and destination model_input.json files, run the configured model, and save its Manny animation."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateRaw(this, &FMotionInbetweeningHelloModule::ExecuteMannyInferenceCommand),
			FCanExecuteAction::CreateLambda([] { return !MotionInbetweening::IsMannyInferenceRunning() && !MotionInbetweening::IsMannyOutputMappingRunning(); })));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.CancelInference",
		LOCTEXT("CancelInferenceLabel", "Cancel Manny Transition Generation"),
		LOCTEXT("CancelInferenceTooltip", "Stop the current inference process tree or its output import."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&MotionInbetweening::CancelMannyInference),
			FCanExecuteAction::CreateStatic(&MotionInbetweening::IsMannyInferenceRunning)));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.OpenInferenceJob",
		LOCTEXT("OpenInferenceJobLabel", "Open Last Manny Transition Job"),
		LOCTEXT("OpenInferenceJobTooltip", "Open the request, model logs and final import receipt."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([] {
			FPlatformProcess::ExploreFolder(*MotionInbetweening::GetLastInferenceResult().OutputDirectory);
		}), FCanExecuteAction::CreateLambda([] { return !MotionInbetweening::GetLastInferenceResult().OutputDirectory.IsEmpty(); })));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.CancelOutputMapping",
		LOCTEXT("CancelOutputMappingLabel", "Cancel Manny Output Import"),
		LOCTEXT("CancelOutputMappingTooltip", "Stop the active model-output conversion before asset creation."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&MotionInbetweening::CancelMannyOutputMapping),
			FCanExecuteAction::CreateLambda([] { return MotionInbetweening::IsMannyOutputMappingRunning() && !MotionInbetweening::IsMannyInferenceRunning(); })));
	Section.AddMenuEntry(
		"MotionInbetweeningHello.CancelMapping",
		LOCTEXT("CancelMappingLabel", "Cancel Manny Input Export"),
		LOCTEXT("CancelMappingTooltip", "Stop the active input-mapping process."),
		FSlateIcon(), FUIAction(FExecuteAction::CreateStatic(&MotionInbetweening::CancelMannyInputMapping),
			FCanExecuteAction::CreateStatic(&MotionInbetweening::IsMannyInputMappingRunning)));
}

void FMotionInbetweeningHelloModule::ExecuteMappingSettingsCommand()
{
	FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings")).ShowViewer(TEXT("Project"), TEXT("Plugins"), TEXT("MannyInputMapping"));
}

void FMotionInbetweeningHelloModule::ExecuteImportMannyOutputCommand()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::TryGet();
	FString Directory, Error;
	if (Desktop)
	{
		TArray<FString> Files;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		if (!Desktop->OpenFileDialog(Parent, TEXT("Select model-output JSON"), FPaths::ProjectSavedDir(), TEXT(""),
			TEXT("JSON files (*.json)|*.json"), 0, Files) || Files.Num() != 1) return;
		if (MotionInbetweening::StartMannyOutputMapping(Files[0], MotionInbetweening::GetOutputMappingOptions(), Directory, Error)) return;
	}
	else Error = TEXT("The file picker is unavailable on this platform.");
	UE_LOG(LogMotionInbetweeningHello, Warning, TEXT("Manny output import could not start: %s"), *Error);
	FNotificationInfo Info(FText::FromString(Error));
	Info.ExpireDuration = 10;
	FSlateNotificationManager::Get().AddNotification(Info);
}

void FMotionInbetweeningHelloModule::ExecuteMannyInferenceCommand()
{
	const auto Options = MotionInbetweening::GetInferenceOptions();
	FString Error, Directory;
	if (!FPaths::FileExists(Options.InferenceScript) || !FPaths::FileExists(Options.ModelConfigFile))
	{
		Error = TEXT("Set the team's Inference Script and Model Config in the Inference settings first.");
		ExecuteMappingSettingsCommand();
	}
	else if (IDesktopPlatform* Desktop = FDesktopPlatformModule::TryGet())
	{
		TArray<FString> StartFiles, EndFiles;
		const void* Parent = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
		const FString InputDirectory = FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/InputMapping");
		if (!Desktop->OpenFileDialog(Parent, TEXT("1. Select START motion model_input.json (last 10 samples)"), InputDirectory,
			TEXT(""), TEXT("JSON files (*.json)|*.json"), 0, StartFiles) || StartFiles.Num() != 1) return;
		if (!Desktop->OpenFileDialog(Parent, TEXT("2. Select DESTINATION motion model_input.json (first sample)"), InputDirectory,
			TEXT(""), TEXT("JSON files (*.json)|*.json"), 0, EndFiles) || EndFiles.Num() != 1) return;
		if (MotionInbetweening::StartMannyInference(StartFiles[0], EndFiles[0], Options, Directory, Error)) return;
	}
	else Error = TEXT("The file picker is unavailable on this platform.");
	UE_LOG(LogMotionInbetweeningHello, Warning, TEXT("Manny inference could not start: %s"), *Error);
	FNotificationInfo Info(FText::FromString(Error));
	Info.ExpireDuration = 12;
	FSlateNotificationManager::Get().AddNotification(Info);
}

void FMotionInbetweeningHelloModule::ExecuteExportMannyInputCommand()
{
	TArray<FAssetData> SelectedAssets;
	FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().GetSelectedAssets(SelectedAssets);
	UAnimSequence* Animation = SelectedAssets.Num() == 1 ? Cast<UAnimSequence>(SelectedAssets[0].GetAsset()) : nullptr;
	FString Directory, Error;
	if (!MotionInbetweening::StartMannyInputMapping(Animation, MotionInbetweening::GetMappingOptions(), Directory, Error))
	{
		UE_LOG(LogMotionInbetweeningHello, Warning, TEXT("Manny input export could not start: %s"), *Error);
		FNotificationInfo Info(FText::FromString(Error));
		Info.ExpireDuration = 10.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

void FMotionInbetweeningHelloModule::ExecuteTestCommand()
{
	FNotificationInfo NotificationInfo(LOCTEXT("TestNotification", "Motion In-betweening plugin is working!"));
	NotificationInfo.ExpireDuration = 3.0f;
	NotificationInfo.FadeOutDuration = 0.2f;
	NotificationInfo.bFireAndForget = true;
	FSlateNotificationManager::Get().AddNotification(NotificationInfo);

	UE_LOG(LogMotionInbetweeningHello, Log, TEXT("[MotionInbetweeningHello] Test command executed."));
}

void FMotionInbetweeningHelloModule::ExecuteGenerateAnimationCommand()
{
	UE_LOG(LogMotionInbetweeningHello, Log, TEXT("Generate Manny test animation requested."));
	FString Error;
	UAnimSequence* Sequence = nullptr;
	if (USkeletalMesh* Mesh = MotionInbetweening::LoadMannyMesh(Error))
	{
		MotionInbetweening::FAnimationSamples Samples;
		if (MotionInbetweening::BuildMannyTestData(*Mesh->GetSkeleton(), Samples, Error))
		{
			Sequence = MotionInbetweening::CreateAndSaveAnimation(*Mesh, Samples, Error);
		}
	}

	if (!Sequence)
	{
		UE_LOG(LogMotionInbetweeningHello, Error, TEXT("Animation generation failed: %s"), *Error);
		FNotificationInfo Info(FText::Format(LOCTEXT("GenerationFailed", "Manny animation failed: {0}"), FText::FromString(Error)));
		Info.ExpireDuration = 8.0f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}

	UAssetEditorSubsystem* AssetEditor = GEditor ? GEditor->GetEditorSubsystem<UAssetEditorSubsystem>() : nullptr;
	const bool bOpened = AssetEditor && AssetEditor->OpenEditorForAsset(Sequence);
	if (!bOpened)
	{
		UE_LOG(LogMotionInbetweeningHello, Warning, TEXT("Saved %s, but could not open Animation Editor. Open the asset from the Content Browser."), *Sequence->GetPathName());
	}
	else
	{
		UE_LOG(LogMotionInbetweeningHello, Log, TEXT("Opened Animation Editor for %s with Manny preview mesh."), *Sequence->GetPathName());
	}
	FNotificationInfo Info(FText::Format(LOCTEXT("GenerationSaved", "Saved {0}. {1}"), FText::FromString(Sequence->GetName()),
		bOpened ? LOCTEXT("PreviewOpened", "Manny preview opened.") : LOCTEXT("PreviewNotOpened", "Open it from the Content Browser.")));
	Info.ExpireDuration = 5.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMotionInbetweeningHelloModule, MotionInbetweeningHello)
