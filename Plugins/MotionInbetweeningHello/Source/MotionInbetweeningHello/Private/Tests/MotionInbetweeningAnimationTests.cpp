#if WITH_DEV_AUTOMATION_TESTS

#include "MotionInbetweeningAnimationData.h"
#include "MotionInbetweeningAnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/IAnimationSequenceCompiler.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/AutomationTest.h"
#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMotionInbetweeningInputTest, "MotionInbetweening.InputValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMotionInbetweeningInputTest::RunTest(const FString& Parameters)
{
	FString Error;
	USkeletalMesh* Mesh = MotionInbetweening::LoadMannyMesh(Error);
	if (!TestNotNull(Error.IsEmpty() ? TEXT("Manny loads") : *Error, Mesh)) return false;
	MotionInbetweening::FAnimationSamples Data;
	if (!TestTrue(TEXT("Build test data against actual skeleton"), MotionInbetweening::BuildMannyTestData(*Mesh->GetSkeleton(), Data, Error))) return false;
	TestEqual(TEXT("Inclusive sample count"), Data.NumSamples, 61);
	TestEqual(TEXT("Four tracks"), Data.Bones.Num(), 4);
	TestEqual(TEXT("Two seconds"), (Data.NumSamples - 1) / Data.FrameRate.AsDecimal(), 2.0);
	TestTrue(TEXT("Root stays fixed"), Data.Bones[0].Rotations[0].Equals(Data.Bones[0].Rotations[30]));
	TestFalse(TEXT("Arm changes pose"), Data.Bones[2].Rotations[0].Equals(Data.Bones[2].Rotations[15], 0.01f));
	for (const MotionInbetweening::FBoneSamples& Bone : Data.Bones)
	{
		TestTrue(TEXT("Loop endpoints match"), Bone.Rotations[0].Equals(Bone.Rotations.Last(), 0.0001f));
	}

	auto ExpectRejected = [this, Mesh, &Error](const TCHAR* Label, const MotionInbetweening::FAnimationSamples& BadData)
	{
		TestFalse(Label, MotionInbetweening::ValidateAnimationData(*Mesh->GetSkeleton(), BadData, Error));
		TestFalse(TEXT("Rejection explains the failure"), Error.IsEmpty());
		TestNull(TEXT("Writer rejects invalid data before asset creation"), MotionInbetweening::CreateAndSaveAnimation(*Mesh, BadData, Error));
	};
	auto Bad = Data;
	Bad.Bones[1].BoneName = TEXT("missing_test_bone");
	ExpectRejected(TEXT("Unknown bone"), Bad);
	Bad = Data; Bad.Bones.Add(Data.Bones[0]);
	ExpectRejected(TEXT("Duplicate bone"), Bad);
	Bad = Data; Bad.Bones[1].Positions.Pop();
	ExpectRejected(TEXT("Mismatched key counts"), Bad);
	Bad = Data; Bad.FrameRate = FFrameRate(0, 1);
	ExpectRejected(TEXT("Zero frame rate"), Bad);
	Bad = Data; Bad.FrameRate = FFrameRate(30, 0);
	ExpectRejected(TEXT("Zero rate denominator"), Bad);
	Bad = Data; Bad.NumSamples = 1;
	ExpectRejected(TEXT("Too few samples"), Bad);
	Bad = Data; Bad.Bones.Reset();
	ExpectRejected(TEXT("Empty tracks"), Bad);
	Bad = Data; Bad.Bones[1].Rotations[0] = FQuat4f(0, 0, 0, 0);
	ExpectRejected(TEXT("Invalid quaternion"), Bad);
	Bad = Data; Bad.Bones[1].Positions[0].X = std::numeric_limits<float>::quiet_NaN();
	ExpectRejected(TEXT("Non-finite position"), Bad);
	Bad = Data; Bad.Bones[1].Scales[0].X = 0;
	ExpectRejected(TEXT("Zero scale"), Bad);
	return true;
}

// Run in a fresh UnrealEditor-Cmd process AFTER clicking the generator at least twice.
// It reads the real saved assets, including compressed playback data; it writes no fixtures.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMotionInbetweeningSavedAssetsTest, "MotionInbetweening.SavedAssets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMotionInbetweeningSavedAssetsTest::RunTest(const FString& Parameters)
{
	FString Error;
	USkeletalMesh* Mesh = MotionInbetweening::LoadMannyMesh(Error);
	if (!TestNotNull(TEXT("Manny loads"), Mesh)) return false;
	MotionInbetweening::FAnimationSamples Expected;
	if (!TestTrue(TEXT("Expected test data"), MotionInbetweening::BuildMannyTestData(*Mesh->GetSkeleton(), Expected, Error))) return false;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanPathsSynchronous({ FString(MotionInbetweening::TestAssetFolder) }, true);
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(FName(MotionInbetweening::TestAssetFolder), Assets);
	int32 VerifiedCount = 0;
	for (const FAssetData& Asset : Assets)
	{
		if (Asset.AssetClassPath != UAnimSequence::StaticClass()->GetClassPathName() || !Asset.AssetName.ToString().StartsWith(TEXT("AN_Manny_Test"))) continue;
		UAnimSequence* Sequence = Cast<UAnimSequence>(Asset.GetAsset());
		if (!TestNotNull(TEXT("Saved sequence loads"), Sequence)) continue;
		AddInfo(FString::Printf(TEXT("Verifying saved asset: %s"), *Sequence->GetPathName()));
		TestTrue(TEXT("Same skeleton"), Sequence->GetSkeleton() == Mesh->GetSkeleton());
		TestTrue(TEXT("Manny preview persisted"), Sequence->GetPreviewMesh() == Mesh);
		TestFalse(TEXT("Root motion disabled"), Sequence->bEnableRootMotion);
		const IAnimationDataModel* Model = Sequence->GetDataModel();
		if (!TestNotNull(TEXT("Data model persisted"), Model)) continue;
		TestEqual(TEXT("Intervals persisted"), Model->GetNumberOfFrames(), 60);
		TestEqual(TEXT("Samples persisted"), Model->GetNumberOfKeys(), 61);
		TestTrue(TEXT("30 fps persisted"), Model->GetFrameRate() == Expected.FrameRate);
		TestTrue(TEXT("Duration persisted"), FMath::IsNearlyEqual(Sequence->GetPlayLength(), 2.0, 0.0001));
		TestEqual(TEXT("Four tracks persisted"), Model->GetNumBoneTracks(), 4);
		for (const MotionInbetweening::FBoneSamples& Track : Expected.Bones)
		{
			if (!TestTrue(TEXT("Track exists"), Model->IsValidBoneTrackName(Track.BoneName))) continue;
			TArray<FTransform> Loaded;
			Model->GetBoneTrackTransforms(Track.BoneName, Loaded);
			if (!TestEqual(TEXT("Track keys persisted"), Loaded.Num(), Expected.NumSamples)) continue;
			for (int32 Index = 0; Index < Loaded.Num(); ++Index)
			{
				const FTransform ExpectedPose(FQuat(Track.Rotations[Index]), FVector(Track.Positions[Index]), FVector(Track.Scales[Index]));
				TestTrue(TEXT("Saved local transform matches input"), Loaded[Index].Equals(ExpectedPose, 0.001));
			}
		}
		UE::Anim::IAnimSequenceCompilingManager::FinishCompilation({ Sequence });
		const FSkeletonPoseBoneIndex ArmIndex(Mesh->GetSkeleton()->GetReferenceSkeleton().FindBoneIndex(TEXT("upperarm_l")));
		FTransform Start, Quarter, End;
		Sequence->GetBoneTransform(Start, ArmIndex, FAnimExtractContext(0.0), false);
		Sequence->GetBoneTransform(Quarter, ArmIndex, FAnimExtractContext(0.5), false);
		Sequence->GetBoneTransform(End, ArmIndex, FAnimExtractContext(2.0), false);
		TestFalse(TEXT("Playback evaluation moves the arm"), Start.GetRotation().Equals(Quarter.GetRotation(), 0.01));
		TestTrue(TEXT("Playback loop endpoints match"), Start.Equals(End, 0.01));
		++VerifiedCount;
	}
	TestTrue(TEXT("At least two separately named assets survive reload"), VerifiedCount >= 2);
	AddInfo(FString::Printf(TEXT("Verified %d saved Manny animations."), VerifiedCount));
	return true;
}

#endif
