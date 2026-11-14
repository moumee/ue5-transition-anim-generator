#if WITH_DEV_AUTOMATION_TESTS
#include "MotionInbetweeningPlayback.h"
#include "MotionInbetweeningOutputData.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimNode_StateMachine.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace PlaybackTests
{
FString RecordDirectory()
{
	FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("MIB_PLAYBACK_RECORD_DIR"));
	return Directory.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/PlaybackCheckTests") : Directory;
}

bool Play(FAutomationTestBase& Test, UAnimBlueprint* BP, USkeletalMesh* Mesh, float Dt)
{
	FPreviewScene Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false));
	auto* Component = NewObject<USkeletalMeshComponent>();
	Component->SetSkeletalMesh(Mesh);
	Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);
	Component->bEnableUpdateRateOptimizations = false;
	Component->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Scene.AddComponent(Component, FTransform::Identity);
	Component->SetAnimInstanceClass(BP->GeneratedClass);
	Component->InitAnim(true);
	UAnimInstance* Instance = Component->GetAnimInstance();
	if (!Test.TestNotNull(TEXT("Generated ABP instantiates"), Instance)) return false;
	TSet<FName> States;
	FName Previous;
	TArray<FName> Order;
	for (int32 Tick = 0; Tick < FMath::CeilToInt(3.0f / Dt); ++Tick)
	{
		Component->TickAnimation(Dt, false);
		Component->RefreshBoneTransforms();
		const FName State = Instance->GetCurrentStateName(0);
		States.Add(State);
		if (State != Previous) { Order.Add(State); Previous = State; }
		for (const FTransform& Pose : Component->GetBoneSpaceTransforms())
			if (Pose.ContainsNaN()) { Test.AddError(TEXT("Playback produced a non-finite pose.")); return false; }
	}
	Test.TestTrue(TEXT("Source state played"), States.Contains(TEXT("Source")));
	Test.TestTrue(TEXT("Cropped bridge state played"), States.Contains(TEXT("Bridge")));
	Test.TestTrue(TEXT("Destination state played"), States.Contains(TEXT("Destination")));
	Test.TestEqual(TEXT("Preview remains at final destination"), Instance->GetCurrentStateName(0), FName(TEXT("Destination")));
	Test.TestEqual(TEXT("Exactly three states without restarting source context"), Order.Num(), 3);
	if (Order.Num() == 3)
	{
		Test.TestEqual(TEXT("State 1"), Order[0], FName(TEXT("Source")));
		Test.TestEqual(TEXT("State 2"), Order[1], FName(TEXT("Bridge")));
		Test.TestEqual(TEXT("State 3"), Order[2], FName(TEXT("Destination")));
	}
	Test.AddInfo(FString::Printf(TEXT("Preview states checked at dt=%.6f"), Dt));
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaybackCreateTest, "MotionInbetweeningUX.Playback.Create",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPlaybackCreateTest::RunTest(const FString&)
{
	using namespace MotionInbetweening;
	auto* Mesh = LoadObject<USkeletalMesh>(nullptr, OutputMannyMeshPath);
	FPlaybackCheckRequest Request;
	Request.Source = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/ControlRig/Characters/Mannequins/Animations/Manny/MM_Walk_Fwd.MM_Walk_Fwd"));
	Request.Destination = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/ControlRig/Characters/Mannequins/Animations/Manny/MM_Run_Fwd.MM_Run_Fwd"));
	Request.FullResult = LoadObject<UAnimSequence>(nullptr, TEXT("/Game/MotionInbetweening/Generated/AN_Manny_Output23.AN_Manny_Output23"));
	if (!TestNotNull(TEXT("Manny"), Mesh) || !TestNotNull(TEXT("Walk"), Request.Source)
		|| !TestNotNull(TEXT("Jog"), Request.Destination) || !TestNotNull(TEXT("Real generated result"), Request.FullResult)) return false;
	Request.TransitionFrames = 20;
	Request.DepartureSeconds = FMath::Min(1.0, FMath::FloorToDouble(Request.Source->GetPlayLength() * 30.0 + 1e-5) / 30.0);
	Request.DestinationPreviewIntervals = FMath::Min(15, FMath::FloorToInt(Request.Destination->GetPlayLength() * 30.0 + 1e-5));
	FString Error;
	FPlaybackCheckData Data;
	if (!TestTrue(*FString::Printf(TEXT("Build playback data: %s"), *Error), BuildPlaybackCheckData(Request, *Mesh, Data, Error)))
	{ AddError(Error); return false; }
	TestEqual(TEXT("Bridge retains source anchor + N generated + target"), Data.Bridge.NumSamples, 22);
	TestTrue(TEXT("Bridge duration is N+1 intervals"), FMath::IsNearlyEqual((Data.Bridge.NumSamples - 1) / 30.0, 0.7, 1e-6));
	TestEqual(TEXT("Combined joins do not duplicate endpoint keys"), Data.Combined.NumSamples, 10 + 21 + Request.DestinationPreviewIntervals);
	TestEqual(TEXT("All mesh bones are sampled"), Data.Bridge.Bones.Num(), Mesh->GetRefSkeleton().GetNum());
	const auto* FullModel = Request.FullResult->GetDataModel();
	for (const auto& Track : Data.Bridge.Bones)
	{
		if (!FullModel->IsValidBoneTrackName(Track.BoneName)) continue;
		const FTransform First = FullModel->GetBoneTrackTransform(Track.BoneName, FFrameNumber(9));
		const FTransform Last = FullModel->GetBoneTrackTransform(Track.BoneName, FFrameNumber(30));
		TestTrue(TEXT("First bridge key is source anchor, not context start"), FVector(Track.Positions[0]).Equals(First.GetTranslation(), 0.001));
		TestTrue(TEXT("Last bridge key preserves target"), FVector(Track.Positions.Last()).Equals(Last.GetTranslation(), 0.001));
	}
	auto Bad = Request; Bad.TransitionFrames = 19;
	FPlaybackCheckData Rejected;
	TestFalse(TEXT("Wrong N fails instead of silently trimming"), BuildPlaybackCheckData(Bad, *Mesh, Rejected, Error));
	Bad = Request; Bad.DepartureSeconds = 0.1;
	TestFalse(TEXT("Short source context rejected"), BuildPlaybackCheckData(Bad, *Mesh, Rejected, Error));
	Bad = Request; Bad.ArrivalSeconds = Request.Destination->GetPlayLength();
	TestFalse(TEXT("Missing destination continuation rejected"), BuildPlaybackCheckData(Bad, *Mesh, Rejected, Error));
	FPlaybackCheckResult Result;
	if (!TestTrue(TEXT("Creates clips, compiled ABP and report"), CreatePlaybackCheck(Request, Result, Error))) { AddError(Error); return false; }
	TestNotNull(TEXT("Combined clip saved"), Result.Combined);
	TestNotNull(TEXT("ABP saved"), Result.Blueprint);
	TestEqual(TEXT("Runtime parent is the engine AnimInstance"), Result.Blueprint->ParentClass.Get(), UAnimInstance::StaticClass());
	TestTrue(TEXT("Report exists"), IFileManager::Get().FileExists(*Result.ReportFile));
	IFileManager::Get().MakeDirectory(*PlaybackTests::RecordDirectory(), true);
	TestTrue(TEXT("Save reload handoff"), FFileHelper::SaveStringToFile(Result.ReportFile,
		*(PlaybackTests::RecordDirectory() / TEXT("created-report.txt")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
	AddInfo(Result.Summary);
	AddInfo(TEXT("MIB_PLAYBACK_REPORT=") + Result.ReportFile);
	PlaybackTests::Play(*this, Result.Blueprint, Mesh, 1.0f / 30.0f);
	PlaybackTests::Play(*this, Result.Blueprint, Mesh, 1.0f / 60.0f);
	PlaybackTests::Play(*this, Result.Blueprint, Mesh, 0.12f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPlaybackReloadTest, "MotionInbetweeningUX.Playback.Reload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPlaybackReloadTest::RunTest(const FString&)
{
	FString ReportPath, Text;
	if (!TestTrue(TEXT("Creation handoff exists"), FFileHelper::LoadFileToString(ReportPath,
		*(PlaybackTests::RecordDirectory() / TEXT("created-report.txt"))))) return false;
	if (!TestTrue(TEXT("Report reloads"), FFileHelper::LoadFileToString(Text, *ReportPath))) return false;
	TSharedPtr<FJsonObject> Report;
	if (!TestTrue(TEXT("Report JSON valid"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Report))) return false;
	auto* BP = LoadObject<UAnimBlueprint>(nullptr, *Report->GetStringField(TEXT("blueprint_asset")));
	auto* Bridge = LoadObject<UAnimSequence>(nullptr, *Report->GetStringField(TEXT("bridge_asset")));
	auto* Preview = LoadObject<UAnimSequence>(nullptr, *Report->GetStringField(TEXT("combined_asset")));
	auto* Mesh = LoadObject<USkeletalMesh>(nullptr, MotionInbetweening::OutputMannyMeshPath);
	if (!TestNotNull(TEXT("Saved ABP reloads"), BP) || !TestNotNull(TEXT("Bridge reloads"), Bridge)
		|| !TestNotNull(TEXT("Continuous preview reloads"), Preview) || !TestNotNull(TEXT("Manny loads"), Mesh)) return false;
	TestEqual(TEXT("Cropped key count persists"), Bridge->GetDataModel()->GetNumberOfKeys(), static_cast<int32>(Report->GetNumberField(TEXT("bridge_samples"))));
	TestTrue(TEXT("Cropped duration persists"), FMath::IsNearlyEqual(Bridge->GetPlayLength(), Report->GetNumberField(TEXT("bridge_seconds")), 1e-5));
	TestEqual(TEXT("Combined key count persists"), Preview->GetDataModel()->GetNumberOfKeys(), static_cast<int32>(Report->GetNumberField(TEXT("combined_samples"))));
	TestEqual(TEXT("No plugin native runtime parent"), BP->ParentClass.Get(), UAnimInstance::StaticClass());
	return PlaybackTests::Play(*this, BP, Mesh, 1.0f / 60.0f);
}
#endif
