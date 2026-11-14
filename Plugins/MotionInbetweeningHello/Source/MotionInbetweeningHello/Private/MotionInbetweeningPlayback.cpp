#include "MotionInbetweeningPlayback.h"

#include "MotionInbetweeningAnimSequence.h"
#include "MotionInbetweeningOutputData.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace MotionInbetweening
{
namespace PlaybackCheck
{
FTransform Sample(const UAnimSequence& Sequence, const FReferenceSkeleton& Ref, int32 Bone, double Seconds)
{
	const auto* Model = Sequence.GetDataModel();
	const FName Name = Ref.GetBoneName(Bone);
	if (!Model->IsValidBoneTrackName(Name)) return Ref.GetRefBonePose()[Bone];
	return Model->EvaluateBoneTrackTransform(Name,
		FFrameTime::FromDecimal(Seconds * Model->GetFrameRate().AsDecimal()), EAnimInterpolationType::Linear);
}

FAnimationSamples Extract(const UAnimSequence& Sequence, const FReferenceSkeleton& Ref, double Start, int32 Count)
{
	FAnimationSamples Data;
	Data.NumSamples = Count;
	for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
	{
		FBoneSamples Track;
		Track.BoneName = Ref.GetBoneName(Bone);
		for (int32 Frame = 0; Frame < Count; ++Frame)
		{
			const FTransform T = Sample(Sequence, Ref, Bone, Start + Frame / 30.0);
			Track.Positions.Add(FVector3f(T.GetTranslation()));
			Track.Rotations.Add(FQuat4f(T.GetRotation().GetNormalized()));
			Track.Scales.Add(FVector3f(T.GetScale3D()));
		}
		Data.Bones.Add(MoveTemp(Track));
	}
	return Data;
}

TArray<FTransform> ComponentPose(const FAnimationSamples& Data, const FReferenceSkeleton& Ref, int32 Frame)
{
	TArray<FTransform> Pose;
	for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
	{
		const auto& Track = Data.Bones[Bone];
		FTransform T(FQuat(Track.Rotations[Frame]), FVector(Track.Positions[Frame]), FVector(Track.Scales[Frame]));
		const int32 Parent = Ref.GetParentIndex(Bone);
		if (Parent != INDEX_NONE) T = T * Pose[Parent];
		Pose.Add(T);
	}
	return Pose;
}

TSharedRef<FJsonObject> Compare(const FAnimationSamples& A, int32 AFrame, const FAnimationSamples& B,
	int32 BFrame, const FReferenceSkeleton& Ref)
{
	const auto APose = ComponentPose(A, Ref, AFrame);
	const auto BPose = ComponentPose(B, Ref, BFrame);
	TArray<TSharedPtr<FJsonValue>> Bones;
	double MaxPosition = 0, MaxRotation = 0;
	FString PositionBone, RotationBone;
	for (int32 Bone = 0; Bone < Ref.GetNum(); ++Bone)
	{
		const double Position = FVector::Distance(APose[Bone].GetTranslation(), BPose[Bone].GetTranslation());
		const double Rotation = FMath::RadiansToDegrees(APose[Bone].GetRotation().AngularDistance(BPose[Bone].GetRotation()));
		const FString Name = Ref.GetBoneName(Bone).ToString();
		if (Position > MaxPosition) { MaxPosition = Position; PositionBone = Name; }
		if (Rotation > MaxRotation) { MaxRotation = Rotation; RotationBone = Name; }
		auto Item = MakeShared<FJsonObject>();
		Item->SetStringField(TEXT("bone"), Name);
		Item->SetNumberField(TEXT("component_position_cm"), Position);
		Item->SetNumberField(TEXT("component_rotation_degrees"), Rotation);
		Item->SetNumberField(TEXT("local_position_cm"), FVector3f::Distance(A.Bones[Bone].Positions[AFrame], B.Bones[Bone].Positions[BFrame]));
		Item->SetNumberField(TEXT("local_rotation_degrees"), FMath::RadiansToDegrees(
			FQuat(A.Bones[Bone].Rotations[AFrame]).AngularDistance(FQuat(B.Bones[Bone].Rotations[BFrame]))));
		Bones.Add(MakeShared<FJsonValueObject>(Item));
	}
	auto Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("max_component_position_cm"), MaxPosition);
	Result->SetNumberField(TEXT("max_component_rotation_degrees"), MaxRotation);
	Result->SetStringField(TEXT("max_position_bone"), PositionBone);
	Result->SetStringField(TEXT("max_rotation_bone"), RotationBone);
	Result->SetArrayField(TEXT("bones"), Bones);
	return Result;
}

void AppendAfterBoundary(FAnimationSamples& To, const FAnimationSamples& From)
{
	for (int32 Bone = 0; Bone < To.Bones.Num(); ++Bone)
	{
		for (int32 Frame = 1; Frame < From.NumSamples; ++Frame)
		{
			To.Bones[Bone].Positions.Add(From.Bones[Bone].Positions[Frame]);
			To.Bones[Bone].Rotations.Add(From.Bones[Bone].Rotations[Frame]);
			To.Bones[Bone].Scales.Add(From.Bones[Bone].Scales[Frame]);
		}
	}
	To.NumSamples += From.NumSamples - 1;
}
}

bool BuildPlaybackCheckData(const FPlaybackCheckRequest& R, USkeletalMesh& Mesh, FPlaybackCheckData& Out, FString& Error)
{
	Out = FPlaybackCheckData(); Error.Reset();
	auto Fail = [&Error](const TCHAR* Message) { Error = Message; return false; };
	if (!R.Source || !R.Destination || !R.FullResult || !Mesh.GetSkeleton())
		return Fail(TEXT("Choose a source, destination and generated Animation Sequence."));
	for (UAnimSequence* Clip : { R.Source, R.Destination, R.FullResult })
	{
		if (Clip->GetSkeleton() != Mesh.GetSkeleton() || !Clip->GetDataModel() || Clip->IsValidAdditive())
			return Fail(TEXT("All clips must be non-additive sequences on the Manny skeleton."));
	}
	const auto* Model = R.FullResult->GetDataModel();
	if (R.TransitionFrames < 5 || R.TransitionFrames > 60 || Model->GetFrameRate() != FFrameRate(30, 1)
		|| Model->GetNumberOfKeys() != R.TransitionFrames + 11)
		return Fail(TEXT("Generated result must have 10 context + N generated + 1 target keys at 30 fps (N=5..60)."));
	if (!FMath::IsFinite(R.DepartureSeconds) || !FMath::IsFinite(R.ArrivalSeconds)
		|| R.DepartureSeconds < 9.0 / 30.0 || R.DepartureSeconds > R.Source->GetPlayLength() + 1e-6
		|| R.ArrivalSeconds < 0 || R.DestinationPreviewIntervals < 1 || R.DestinationPreviewIntervals > 300
		|| R.ArrivalSeconds + R.DestinationPreviewIntervals / 30.0 > R.Destination->GetPlayLength() + 1e-6)
		return Fail(TEXT("Source needs 10 context samples before departure; destination needs the requested continuation after arrival."));
	const auto& Ref = Mesh.GetRefSkeleton();
	Out.SourceContext = PlaybackCheck::Extract(*R.Source, Ref, R.DepartureSeconds - 9.0 / 30.0, 10);
	Out.Bridge = PlaybackCheck::Extract(*R.FullResult, Ref, 9.0 / 30.0, R.TransitionFrames + 2);
	Out.DestinationContext = PlaybackCheck::Extract(*R.Destination, Ref, R.ArrivalSeconds, R.DestinationPreviewIntervals + 1);
	for (const auto* Data : { &Out.SourceContext, &Out.Bridge, &Out.DestinationContext })
		if (!ValidateAnimationData(*Mesh.GetSkeleton(), *Data, Error)) return false;
	Out.Combined = Out.SourceContext;
	PlaybackCheck::AppendAfterBoundary(Out.Combined, Out.Bridge);
	PlaybackCheck::AppendAfterBoundary(Out.Combined, Out.DestinationContext);
	if (!ValidateAnimationData(*Mesh.GetSkeleton(), Out.Combined, Error)) return false;
	Out.Diagnostics = MakeShared<FJsonObject>();
	auto& D = *Out.Diagnostics;
	D.SetStringField(TEXT("schema"), TEXT("motion_inbetweening.playback_check.v1"));
	D.SetStringField(TEXT("source"), R.Source->GetPathName());
	D.SetStringField(TEXT("destination"), R.Destination->GetPathName());
	D.SetStringField(TEXT("full_result"), R.FullResult->GetPathName());
	D.SetNumberField(TEXT("generated_samples"), R.TransitionFrames);
	D.SetNumberField(TEXT("departure_seconds"), R.DepartureSeconds);
	D.SetNumberField(TEXT("arrival_seconds"), R.ArrivalSeconds);
	D.SetNumberField(TEXT("bridge_samples"), Out.Bridge.NumSamples);
	D.SetNumberField(TEXT("bridge_seconds"), (Out.Bridge.NumSamples - 1) / 30.0);
	D.SetNumberField(TEXT("combined_samples"), Out.Combined.NumSamples);
	D.SetStringField(TEXT("scope"), TEXT("Diagnostic preview only; source times are supplied, provenance is not inferred. No seam correction or quality approval."));
	D.SetObjectField(TEXT("departure_boundary"), PlaybackCheck::Compare(Out.SourceContext, 9, Out.Bridge, 0, Ref));
	D.SetObjectField(TEXT("arrival_boundary"), PlaybackCheck::Compare(Out.Bridge, Out.Bridge.NumSamples - 1, Out.DestinationContext, 0, Ref));
	D.SetObjectField(TEXT("source_loop"), PlaybackCheck::Compare(
		PlaybackCheck::Extract(*R.Source, Ref, 0, 1), 0, PlaybackCheck::Extract(*R.Source, Ref, R.Source->GetPlayLength(), 1), 0, Ref));
	D.SetObjectField(TEXT("destination_loop"), PlaybackCheck::Compare(
		PlaybackCheck::Extract(*R.Destination, Ref, 0, 1), 0, PlaybackCheck::Extract(*R.Destination, Ref, R.Destination->GetPlayLength(), 1), 0, Ref));
	return true;
}

bool CreatePlaybackCheck(const FPlaybackCheckRequest& R, FPlaybackCheckResult& Out, FString& Error)
{
	Out = FPlaybackCheckResult(); Error.Reset();
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, OutputMannyMeshPath);
	if (!Mesh) { Error = TEXT("Manny Simple preview mesh could not be loaded."); return false; }
	FPlaybackCheckData Data;
	if (!BuildPlaybackCheckData(R, *Mesh, Data, Error)) return false;
	const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	const FString Folder = TEXT("/Game/MotionInbetweening/PlaybackChecks/Check_") + Id;
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/PlaybackChecks") / Id);
	if (!IFileManager::Get().MakeDirectory(*Directory, true)) { Error = TEXT("Could not create the playback report directory."); return false; }
	Out.ReportFile = Directory / TEXT("report.json");
	auto Finish = [&](bool Success)
	{
		Data.Diagnostics->SetBoolField(TEXT("success"), Success);
		Data.Diagnostics->SetStringField(TEXT("error"), Error);
		FString Text;
		const bool Saved = FJsonSerializer::Serialize(Data.Diagnostics.ToSharedRef(), TJsonWriterFactory<>::Create(&Text))
			&& FFileHelper::SaveStringToFile(Text, *Out.ReportFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		if (!Saved) { Error += TEXT(" Could not save the diagnostic report."); return false; }
		return Success;
	};
	UAnimSequence* Source = CreateAndSaveAnimation(*Mesh, Data.SourceContext, Error, Folder / TEXT("AN_SourceContext"));
	if (!Source) return Finish(false);
	Data.Diagnostics->SetStringField(TEXT("source_context_asset"), Source->GetPathName());
	UAnimSequence* Bridge = CreateAndSaveAnimation(*Mesh, Data.Bridge, Error, Folder / TEXT("AN_Transition"));
	if (!Bridge) return Finish(false);
	Data.Diagnostics->SetStringField(TEXT("bridge_asset"), Bridge->GetPathName());
	UAnimSequence* Destination = CreateAndSaveAnimation(*Mesh, Data.DestinationContext, Error, Folder / TEXT("AN_DestinationContext"));
	if (!Destination) return Finish(false);
	Data.Diagnostics->SetStringField(TEXT("destination_context_asset"), Destination->GetPathName());
	Out.Combined = CreateAndSaveAnimation(*Mesh, Data.Combined, Error, Folder / TEXT("AN_ContinuousPreview"));
	if (!Out.Combined) return Finish(false);
	Data.Diagnostics->SetStringField(TEXT("combined_asset"), Out.Combined->GetPathName());
	Out.Blueprint = CreatePlaybackPreviewBlueprint(*Mesh, *Source, *Bridge, *Destination, Error);
	if (!Out.Blueprint) return Finish(false);
	Data.Diagnostics->SetStringField(TEXT("blueprint_asset"), Out.Blueprint->GetPathName());
	const auto Departure = Data.Diagnostics->GetObjectField(TEXT("departure_boundary"));
	const auto Arrival = Data.Diagnostics->GetObjectField(TEXT("arrival_boundary"));
	Out.Summary = FString::Printf(TEXT("Preview created. Boundary differences: departure %.3f cm / %.3f deg; arrival %.3f cm / %.3f deg. These are measurements, not a quality pass."),
		Departure->GetNumberField(TEXT("max_component_position_cm")), Departure->GetNumberField(TEXT("max_component_rotation_degrees")),
		Arrival->GetNumberField(TEXT("max_component_position_cm")), Arrival->GetNumberField(TEXT("max_component_rotation_degrees")));
	return Finish(true);
}
}
