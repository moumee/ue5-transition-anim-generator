#include "MotionInbetweeningPlayback.h"

#include "MotionInbetweeningInference.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetData.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "PropertyCustomizationHelpers.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace MotionInbetweening
{
class SPlaybackCheckPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SPlaybackCheckPanel) {} SLATE_END_ARGS()
	void Construct(const FArguments&)
	{
		Frames = GetInferenceOptions().TransitionFrames;
		ChildSlot
		[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(12)
				[SNew(STextBlock).Text(FText::FromString(TEXT("Compare an existing generated transition with its original clips. Creates a playback clip, continuous preview and a three-state preview ABP. This checks playback boundaries; it does not generate new model output."))).AutoWrapText(true)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)[Picker(TEXT("Source animation"), Source)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)[Picker(TEXT("Destination animation"), Destination)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)[Picker(TEXT("Generated result (includes context)"), Generated)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)
				[SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(FText::FromString(TEXT("Generated frames (N)")))]
					+ SHorizontalBox::Slot().FillWidth(1)[SNew(SSpinBox<int32>).MinValue(5).MaxValue(60).Value_Lambda([this] { return Frames; }).OnValueChanged_Lambda([this](int32 V) { Frames = V; })]]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)[TimeInput(TEXT("Source departure (seconds)"), Departure)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)[TimeInput(TEXT("Destination arrival (seconds)"), Arrival)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)
				[SNew(STextBlock).Text(FText::FromString(TEXT("Use the times from the generation request. Legacy exports usually depart at min(1 second, source duration) and arrive at 0. Preview includes 0.3 seconds of source and 0.5 seconds of destination. Originals are preserved; boundary differences remain visible."))).AutoWrapText(true)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12)
				[SNew(SButton).Text(FText::FromString(TEXT("Build playback check"))).OnClicked(this, &SPlaybackCheckPanel::Build)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12, 4)
				[SNew(STextBlock).Text_Lambda([this] { return FText::FromString(Status); }).AutoWrapText(true)]
				+ SVerticalBox::Slot().AutoHeight().Padding(12)
				[SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Open continuous preview"))).IsEnabled_Lambda([this] { return Preview.IsValid(); }).OnClicked_Lambda([this] { Open(Preview.Get()); return FReply::Handled(); })]
					+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0)[SNew(SButton).Text(FText::FromString(TEXT("Open preview ABP"))).IsEnabled_Lambda([this] { return Blueprint.IsValid(); }).OnClicked_Lambda([this] { Open(Blueprint.Get()); return FReply::Handled(); })]
					+ SHorizontalBox::Slot().AutoWidth()[SNew(SButton).Text(FText::FromString(TEXT("Open report folder"))).IsEnabled_Lambda([this] { return !Report.IsEmpty(); }).OnClicked_Lambda([this] { FPlatformProcess::ExploreFolder(*FPaths::GetPath(Report)); return FReply::Handled(); })]]
			]
		];
	}
private:
	FSoftObjectPath Source, Destination, Generated;
	int32 Frames = 15;
	float Departure = 1.0f, Arrival = 0.0f;
	FString Status, Report;
	TWeakObjectPtr<UAnimSequence> Preview;
	TWeakObjectPtr<UAnimBlueprint> Blueprint;
	TSharedRef<SWidget> Picker(const TCHAR* Label, FSoftObjectPath& Path)
	{
		FSoftObjectPath* Field = &Path;
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FText::FromString(Label))]
			+ SVerticalBox::Slot().AutoHeight()[SNew(SObjectPropertyEntryBox).AllowedClass(UAnimSequence::StaticClass())
				.ObjectPath_Lambda([Field] { return Field->ToString(); })
				.OnObjectChanged_Lambda([Field](const FAssetData& Asset) { *Field = Asset.GetSoftObjectPath(); })];
	}
	TSharedRef<SWidget> TimeInput(const TCHAR* Label, float& Time)
	{
		float* Field = &Time;
		return SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1)[SNew(STextBlock).Text(FText::FromString(Label))]
			+ SHorizontalBox::Slot().FillWidth(1)[SNew(SSpinBox<float>).MinValue(0.0f).MaxValue(3600.0f)
				.Value_Lambda([Field] { return *Field; }).OnValueChanged_Lambda([Field](float V) { *Field = V; })];
	}
	static void Open(UObject* Object)
	{
		if (Object && GEditor) GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Object);
	}
	FReply Build()
	{
		Preview.Reset(); Blueprint.Reset(); Report.Reset();
		FPlaybackCheckRequest Request;
		Request.Source = Cast<UAnimSequence>(Source.TryLoad());
		Request.Destination = Cast<UAnimSequence>(Destination.TryLoad());
		Request.FullResult = Cast<UAnimSequence>(Generated.TryLoad());
		Request.TransitionFrames = Frames; Request.DepartureSeconds = Departure; Request.ArrivalSeconds = Arrival;
		FPlaybackCheckResult Result;
		if (CreatePlaybackCheck(Request, Result, Status))
		{
			Status = Result.Summary; Preview = Result.Combined; Blueprint = Result.Blueprint;
		}
		Report = Result.ReportFile;
		return FReply::Handled();
	}
};

void OpenPlaybackCheckWindow()
{
	TSharedRef<SWindow> Window = SNew(SWindow).Title(FText::FromString(TEXT("Motion Transition - Playback Boundary Check")))
		.ClientSize(FVector2D(760, 620)).SupportsMaximize(true).SupportsMinimize(false)
		[SNew(SPlaybackCheckPanel)];
	FSlateApplication::Get().AddWindow(Window);
}
}
