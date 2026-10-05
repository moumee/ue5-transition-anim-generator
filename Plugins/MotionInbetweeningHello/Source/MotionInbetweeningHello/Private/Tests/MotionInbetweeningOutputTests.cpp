#if WITH_DEV_AUTOMATION_TESTS

#include "MotionInbetweeningAnimationData.h"
#include "MotionInbetweeningOutputData.h"
#include "MotionInbetweeningOutputMapping.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/IAnimationSequenceCompiler.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
void VerifyOutputAsset(FAutomationTestBase& Test, const FString& Directory, const FString& AssetPath)
{
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, MotionInbetweening::OutputMannyMeshPath);
	UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, *AssetPath);
	if (!Test.TestNotNull(TEXT("Imported animation loads"), Animation) || !Test.TestNotNull(TEXT("Manny Simple loads"), Mesh)) return;
	Test.TestTrue(TEXT("Manny skeleton matches"), Animation->GetSkeleton() == Mesh->GetSkeleton());
	Test.TestTrue(TEXT("Simple preview mesh persisted"), Animation->GetPreviewMesh() == Mesh);
	Test.TestFalse(TEXT("Root motion remains disabled"), Animation->bEnableRootMotion);
	FString Error;
	MotionInbetweening::FAnimationSamples Expected;
	if (!Test.TestTrue(TEXT("Load source local poses"), MotionInbetweening::LoadMannyLocalPose(Directory / TEXT("manny_pose.json"), *Mesh, Expected, Error))) return;
	const IAnimationDataModel* Model = Animation->GetDataModel();
	if (!Test.TestNotNull(TEXT("Animation data model exists"), Model)) return;
	Test.TestEqual(TEXT("All 89 mesh tracks"), Model->GetNumBoneTracks(), 89);
	Test.TestEqual(TEXT("Sample count preserved"), Model->GetNumberOfKeys(), Expected.NumSamples);
	Test.TestEqual(TEXT("Intervals exclude inclusive end sample"), Model->GetNumberOfFrames(), Expected.NumSamples - 1);
	Test.TestTrue(TEXT("Frame rate preserved"), Model->GetFrameRate() == Expected.FrameRate);
	Test.TestTrue(TEXT("Duration preserved"), FMath::IsNearlyEqual(Animation->GetPlayLength(), (Expected.NumSamples - 1) / Expected.FrameRate.AsDecimal(), 0.00001));
	UE::Anim::IAnimSequenceCompilingManager::FinishCompilation({ Animation });
	double MaxPlaybackPositionError = 0, MaxPlaybackRotationError = 0, MaxUnnormalizedAngle = 0;
	double MaxRawRotationError = 0, MaxRawPositionError = 0;
	FString WorstBone;
	for (const auto& Track : Expected.Bones)
	{
		TArray<FTransform> Keys;
		Model->GetBoneTrackTransforms(Track.BoneName, Keys);
		if (!Test.TestEqual(TEXT("Track key count preserved"), Keys.Num(), Expected.NumSamples)) continue;
		const FSkeletonPoseBoneIndex BoneIndex(Mesh->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(Track.BoneName));
		for (int32 Index = 0; Index < Keys.Num(); ++Index)
		{
			const FTransform ExpectedPose(FQuat(Track.Rotations[Index]), FVector(Track.Positions[Index]), FVector(Track.Scales[Index]));
			Test.TestTrue(TEXT("Saved local keys equal converted poses"), Keys[Index].Equals(ExpectedPose, 0.00001));
			FTransform Playback;
			Animation->GetBoneTransform(Playback, BoneIndex, FAnimExtractContext(Index / Expected.FrameRate.AsDecimal()), false);
			MaxPlaybackPositionError = FMath::Max(MaxPlaybackPositionError, FVector::Distance(Playback.GetTranslation(), ExpectedPose.GetTranslation()));
			MaxUnnormalizedAngle = FMath::Max(MaxUnnormalizedAngle, FMath::RadiansToDegrees(Playback.GetRotation().AngularDistance(ExpectedPose.GetRotation())));
			// Float storage slightly changes quaternion length. Normalize before measuring
			// orientation, and use atan2 to avoid acos precision loss near zero error.
			const FQuat Delta = (Playback.GetRotation().GetNormalized() * ExpectedPose.GetRotation().GetNormalized().Inverse()).GetNormalized();
			const double RotationError = FMath::RadiansToDegrees(2.0 * FMath::Atan2(FMath::Sqrt(Delta.X * Delta.X + Delta.Y * Delta.Y + Delta.Z * Delta.Z), FMath::Abs(Delta.W)));
			if (RotationError > MaxPlaybackRotationError) { MaxPlaybackRotationError = RotationError; WorstBone = Track.BoneName.ToString(); }
			FTransform RawPlayback;
			Animation->GetBoneTransform(RawPlayback, BoneIndex, FAnimExtractContext(Index / Expected.FrameRate.AsDecimal()), true);
			MaxRawPositionError = FMath::Max(MaxRawPositionError, FVector::Distance(RawPlayback.GetTranslation(), ExpectedPose.GetTranslation()));
			const FQuat RawDelta = (RawPlayback.GetRotation().GetNormalized() * ExpectedPose.GetRotation().GetNormalized().Inverse()).GetNormalized();
			MaxRawRotationError = FMath::Max(MaxRawRotationError, FMath::RadiansToDegrees(2.0 * FMath::Atan2(FMath::Sqrt(RawDelta.X * RawDelta.X + RawDelta.Y * RawDelta.Y + RawDelta.Z * RawDelta.Z), FMath::Abs(RawDelta.W))));
		}
	}
	Test.TestTrue(TEXT("Raw playback positions within 0.001 cm"), MaxRawPositionError < 0.001);
	Test.TestTrue(TEXT("Raw playback rotations within 0.001 degree"), MaxRawRotationError < 0.001);
	Test.TestTrue(TEXT("Compressed playback positions within 0.01 cm"), MaxPlaybackPositionError < 0.01);
	// UE 5.8 ACL defaults target 0.01 cm at a 3 cm virtual vertex radius:
	// 2 * asin(0.01 / (2 * 3)) = 0.191 degrees, not a 0.1-degree rotation cap.
	// Keep an explicit 0.2-degree smoke-test bound; this is not visual approval.
	Test.TestTrue(TEXT("Compressed playback rotations within 0.2 degree"), MaxPlaybackRotationError < 0.2);
	Test.AddInfo(FString::Printf(TEXT("Output asset %s | max playback error: %.6f cm, %.6f deg | %s"),
		*AssetPath, MaxPlaybackPositionError, MaxPlaybackRotationError, *Directory));
	Test.AddInfo(FString::Printf(TEXT("Rotation diagnostic: worst bone %s, unnormalized acos metric %.6f deg"), *WorstBone, MaxUnnormalizedAngle));
	Test.AddInfo(FString::Printf(TEXT("Raw playback errors: %.9f cm, %.9f deg"), MaxRawPositionError, MaxRawRotationError));
}

class FWaitForOutput : public IAutomationLatentCommand
{
public:
	FWaitForOutput(FAutomationTestBase* InTest, FString InSource, MotionInbetweening::FOutputMappingOptions InOptions)
		: Test(InTest), Source(MoveTemp(InSource)), Options(MoveTemp(InOptions)), Deadline(FPlatformTime::Seconds() + 130) {}
	bool Update() override
	{
		if (MotionInbetweening::IsMannyOutputMappingRunning())
		{
			if (FPlatformTime::Seconds() < Deadline) return false;
			MotionInbetweening::CancelMannyOutputMapping(); Test->AddError(TEXT("Output test timed out.")); return true;
		}
		const auto Result = MotionInbetweening::GetLastOutputMappingResult();
		if (!Test->TestTrue(*FString::Printf(TEXT("Output import succeeds: %s"), *Result.Error), Result.bSuccess)) return true;
		Test->TestEqual(TEXT("31 model samples imported"), Result.SampleCount, 31);
		VerifyOutputAsset(*Test, Result.OutputDirectory, Result.AssetPath);
		if (FirstAsset.IsEmpty())
		{
			FirstAsset = Result.AssetPath;
			FString Directory, Error;
			if (!Test->TestTrue(TEXT("Import a second time"), MotionInbetweening::StartMannyOutputMapping(Source, Options, Directory, Error))) return true;
			Deadline = FPlatformTime::Seconds() + 130;
			return false;
		}
		Test->TestTrue(TEXT("Repeated import creates a separate asset"), FirstAsset != Result.AssetPath);
#if PLATFORM_WINDOWS
		// An existing JSON file passes the path check but cannot be launched as Python.
		// Exercise this after a successful import to detect stale success/asset state.
		auto Bad = Options;
		Bad.PythonExecutable = Source;
		FString FailedDirectory, Error;
		Test->AddExpectedMessage(TEXT("CreateProc failed:"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, false);
		Test->AddExpectedMessage(TEXT("URL:"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1, false);
		if (!Test->TestFalse(TEXT("Reject an existing non-executable Python file"),
			MotionInbetweening::StartMannyOutputMapping(Source, Bad, FailedDirectory, Error)))
		{
			MotionInbetweening::CancelMannyOutputMapping();
			return true;
		}
		const auto Failed = MotionInbetweening::GetLastOutputMappingResult();
		Test->TestFalse(TEXT("Failed start clears previous success"), Failed.bSuccess);
		Test->TestTrue(TEXT("Failed start clears previous asset"), Failed.AssetPath.IsEmpty());
		Test->TestEqual(TEXT("Failed start clears previous sample count"), Failed.SampleCount, 0);
		Test->TestFalse(TEXT("Failed start leaves no active worker"), MotionInbetweening::IsMannyOutputMappingRunning());
		Test->TestFalse(TEXT("Failed start identifies a new job"), FailedDirectory.IsEmpty());
		Test->TestEqual(TEXT("Last result belongs to the failed job"), Failed.OutputDirectory, FailedDirectory);
		FString ReceiptText, ReceiptError;
		TSharedPtr<FJsonObject> Receipt;
		bool bReceiptSuccess = true;
		if (Test->TestTrue(TEXT("Failed start writes a receipt"),
			FFileHelper::LoadFileToString(ReceiptText, *(FailedDirectory / TEXT("asset_result.json")))
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ReceiptText), Receipt) && Receipt))
		{
			Test->TestTrue(TEXT("Failed receipt declares failure"), Receipt->TryGetBoolField(TEXT("success"), bReceiptSuccess) && !bReceiptSuccess);
			Test->TestTrue(TEXT("Failed receipt includes the start error"), Receipt->TryGetStringField(TEXT("error"), ReceiptError) && !ReceiptError.IsEmpty() && ReceiptError == Error);
		}
		Test->AddInfo(FString::Printf(TEXT("Verified failed-start receipt after successful import: %s"), *FailedDirectory));
#endif
		return true;
	}
private:
	FAutomationTestBase* Test;
	FString Source, FirstAsset;
	MotionInbetweening::FOutputMappingOptions Options;
	double Deadline;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMannyOutputTest, "MotionInbetweening.MannyOutputMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMannyOutputTest::RunTest(const FString& Parameters)
{
	const FString Source = IPluginManager::Get().FindPlugin(TEXT("MotionInbetweeningHello"))->GetBaseDir() / TEXT("Tests/Fixtures/context_model_output.json");
	auto Options = MotionInbetweening::GetOutputMappingOptions(); Options.bOpenAsset = false;
	FString Directory, Error;
	auto Bad = Options; Bad.PythonExecutable.Reset();
	TestFalse(TEXT("Reject missing Python"), MotionInbetweening::StartMannyOutputMapping(Source, Bad, Directory, Error));
	TestFalse(TEXT("Reject missing source file"), MotionInbetweening::StartMannyOutputMapping(TEXT(""), Options, Directory, Error));
	if (!TestTrue(TEXT("Start real Context output import"), MotionInbetweening::StartMannyOutputMapping(Source, Options, Directory, Error)))
	{
		AddError(Error); return false;
	}
	TestFalse(TEXT("Reject concurrent output import"), MotionInbetweening::StartMannyOutputMapping(Source, Options, Directory, Error));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForOutput(this, Source, Options));
	return true;
}

// Run separately in a fresh editor process after the import test has created its assets.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMannyOutputReloadTest, "MotionInbetweeningReload.OutputAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMannyOutputReloadTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/OutputMapping");
	TArray<FString> Receipts;
	IFileManager::Get().FindFilesRecursive(Receipts, *Root, TEXT("asset_result.json"), true, false);
	int32 Count = 0;
	for (const FString& Receipt : Receipts)
	{
		FString Text, AssetPath;
		TSharedPtr<FJsonObject> Result;
		bool bSuccess = false;
		if (!FFileHelper::LoadFileToString(Text, *Receipt)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Result) || !Result
			|| !Result->TryGetBoolField(TEXT("success"), bSuccess) || !bSuccess
			|| !Result->TryGetStringField(TEXT("asset_path"), AssetPath)) continue;
		VerifyOutputAsset(*this, FPaths::GetPath(Receipt), AssetPath);
		++Count;
	}
	TestTrue(TEXT("At least two imported assets survived restart"), Count >= 2);
	AddInfo(FString::Printf(TEXT("Verified %d reloaded model-output assets."), Count));
	return true;
}

#endif
