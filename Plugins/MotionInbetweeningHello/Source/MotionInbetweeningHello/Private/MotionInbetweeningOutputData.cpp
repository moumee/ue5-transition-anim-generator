#include "MotionInbetweeningOutputData.h"

#include "MotionInbetweeningAnimationData.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace MotionInbetweening
{
namespace
{
TArray<TSharedPtr<FJsonValue>> Numbers(std::initializer_list<double> Values)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	for (double Value : Values) Result.Add(MakeShared<FJsonValueNumber>(Value));
	return Result;
}

bool ReadNumbers(const FJsonObject& Object, const TCHAR* Key, int32 Count, float* OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values;
	if (!Object.TryGetArrayField(Key, Values) || Values->Num() != Count) return false;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		double Number;
		if (!(*Values)[Index]->TryGetNumber(Number) || !FMath::IsFinite(Number)) return false;
		OutValues[Index] = static_cast<float>(Number);
		if (!FMath::IsFinite(OutValues[Index])) return false;
	}
	return true;
}
}

bool ExportMannyReferenceSkeleton(const USkeletalMesh& Mesh, const FString& Filename, FString& OutError)
{
	if (!Mesh.GetSkeleton()) { OutError = TEXT("Manny mesh has no skeleton."); return false; }
	const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
	const auto& Pose = Reference.GetRawRefBonePose();
	TArray<TSharedPtr<FJsonValue>> Bones;
	for (int32 Index = 0; Index < Reference.GetRawBoneNum(); ++Index)
	{
		const FTransform& Transform = Pose[Index];
		const FVector Position = Transform.GetTranslation(), Scale = Transform.GetScale3D();
		const FQuat Rotation = Transform.GetRotation();
		TSharedRef<FJsonObject> Local = MakeShared<FJsonObject>();
		Local->SetArrayField(TEXT("translation_cm"), Numbers({ Position.X, Position.Y, Position.Z }));
		Local->SetArrayField(TEXT("rotation_quaternion"), Numbers({ Rotation.X, Rotation.Y, Rotation.Z, Rotation.W }));
		Local->SetArrayField(TEXT("scale"), Numbers({ Scale.X, Scale.Y, Scale.Z }));
		TSharedRef<FJsonObject> Bone = MakeShared<FJsonObject>();
		Bone->SetNumberField(TEXT("index"), Index);
		Bone->SetStringField(TEXT("name"), Reference.GetBoneName(Index).ToString());
		const int32 Parent = Reference.GetParentIndex(Index);
		if (Parent == INDEX_NONE) Bone->SetField(TEXT("parent"), MakeShared<FJsonValueNull>());
		else Bone->SetStringField(TEXT("parent"), Reference.GetBoneName(Parent).ToString());
		Bone->SetObjectField(TEXT("reference_local_transform"), Local);
		Bones.Add(MakeShared<FJsonValueObject>(Bone));
	}
	TSharedRef<FJsonObject> Document = MakeShared<FJsonObject>();
	Document->SetStringField(TEXT("skeletal_mesh"), Mesh.GetPathName());
	Document->SetStringField(TEXT("skeleton"), Mesh.GetSkeleton()->GetPathName());
	Document->SetNumberField(TEXT("bone_count"), Bones.Num());
	Document->SetArrayField(TEXT("bones"), Bones);
	FString Text;
	if (!FJsonSerializer::Serialize(Document, TJsonWriterFactory<>::Create(&Text))
		|| !FFileHelper::SaveStringToFile(Text, *Filename, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("Could not write the Manny reference skeleton."); return false;
	}
	return true;
}

bool LoadMannyLocalPose(const FString& Filename, const USkeletalMesh& Mesh, FAnimationSamples& OutData, FString& OutError)
{
	OutData = FAnimationSamples();
	OutError = TEXT("Invalid Manny pose JSON. Inspect manny_pose.json in the output job folder.");
	FString Text;
	TSharedPtr<FJsonObject> Document;
	if (!Mesh.GetSkeleton() || !FFileHelper::LoadFileToString(Text, *Filename)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Document) || !Document) return false;
	FString MeshPath, SkeletonPath;
	const TSharedPtr<FJsonObject>* Validation;
	bool bPassed = false;
	if (!Document->TryGetObjectField(TEXT("validation"), Validation)
		|| !(*Validation)->TryGetBoolField(TEXT("passed"), bPassed) || !bPassed
		|| !Document->TryGetStringField(TEXT("skeletal_mesh"), MeshPath) || MeshPath != Mesh.GetPathName()
		|| !Document->TryGetStringField(TEXT("skeleton"), SkeletonPath) || SkeletonPath != Mesh.GetSkeleton()->GetPathName()) return false;
	double Rate, Count;
	const TArray<TSharedPtr<FJsonValue>>* Frames;
	if (!Document->TryGetNumberField(TEXT("sample_rate_hz"), Rate) || !FMath::IsFinite(Rate) || Rate <= 0 || Rate > 240
		|| !Document->TryGetArrayField(TEXT("full_local_pose_frames"), Frames) || Frames->Num() < 2 || Frames->Num() > 10000
		|| !Document->TryGetNumberField(TEXT("frame_count"), Count) || Count != Frames->Num()) return false;
	FAnimationSamples Data;
	Data.NumSamples = Frames->Num();
	const int32 Numerator = FMath::RoundToInt(Rate * 1000000.0);
	if (Numerator <= 0) return false;
	int32 A = Numerator, B = 1000000;
	while (B != 0) { const int32 Remainder = A % B; A = B; B = Remainder; }
	Data.FrameRate = FFrameRate(Numerator / A, 1000000 / A);
	const FReferenceSkeleton& Reference = Mesh.GetRefSkeleton();
	Data.Bones.SetNum(Reference.GetRawBoneNum());
	for (int32 Index = 0; Index < Data.Bones.Num(); ++Index)
	{
		Data.Bones[Index].BoneName = Reference.GetBoneName(Index);
	}
	for (int32 FrameIndex = 0; FrameIndex < Frames->Num(); ++FrameIndex)
	{
		if ((*Frames)[FrameIndex]->Type != EJson::Object) return false;
		const TSharedPtr<FJsonObject> Frame = (*Frames)[FrameIndex]->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Bones;
		double Time;
		if (!Frame->TryGetArrayField(TEXT("bones"), Bones) || Bones->Num() != Data.Bones.Num()
			|| !Frame->TryGetNumberField(TEXT("time_seconds"), Time) || !FMath::IsFinite(Time)
			|| FMath::Abs(Time - FrameIndex / Rate) > 0.00001) return false;
		for (int32 BoneIndex = 0; BoneIndex < Bones->Num(); ++BoneIndex)
		{
			if ((*Bones)[BoneIndex]->Type != EJson::Object) return false;
			const TSharedPtr<FJsonObject> Bone = (*Bones)[BoneIndex]->AsObject();
			FBoneSamples& Track = Data.Bones[BoneIndex];
			FString Name;
			float Position[3], Rotation[4], Scale[3];
			if (!Bone->TryGetStringField(TEXT("name"), Name) || FName(Name) != Track.BoneName
				|| !ReadNumbers(*Bone, TEXT("local_translation_cm"), 3, Position)
				|| !ReadNumbers(*Bone, TEXT("local_rotation_xyzw"), 4, Rotation)
				|| !ReadNumbers(*Bone, TEXT("local_scale"), 3, Scale)) return false;
			Track.Positions.Add(FVector3f(Position[0], Position[1], Position[2]));
			Track.Rotations.Add(FQuat4f(Rotation[0], Rotation[1], Rotation[2], Rotation[3]));
			Track.Scales.Add(FVector3f(Scale[0], Scale[1], Scale[2]));
		}
	}
	if (!ValidateAnimationData(*Mesh.GetSkeleton(), Data, OutError)) return false;
	OutData = MoveTemp(Data);
	OutError.Reset();
	return true;
}
}
