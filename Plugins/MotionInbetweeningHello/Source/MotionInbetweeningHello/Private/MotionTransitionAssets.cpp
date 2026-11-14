#include "MotionTransitionEditor.h"
#include "MotionInbetweeningAnimationData.h"
#include "MotionInbetweeningAnimSequence.h"
#include "MotionInbetweeningOutputData.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"

namespace MotionInbetweening
{
FString TransitionFileHash(const FString& Path)
{
    return FPaths::FileExists(Path) ? LexToString(FMD5Hash::HashFile(*Path)) : FString();
}
FString TransitionSourceStamp(UAnimSequence* Asset)
{
    if (!Asset || Asset->GetOutermost()->IsDirty()) return FString();
    return TransitionFileHash(FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension()));
}
namespace TransitionAssets
{
FAnimationSamples Read(UAnimSequence& Clip, const FReferenceSkeleton& Ref, double Start, int32 Count)
{
    FAnimationSamples Data; Data.NumSamples = Count;
    const auto* Model = Clip.GetDataModel();
    for (int32 B = 0; B < Ref.GetNum(); ++B)
    {
        FBoneSamples Track; Track.BoneName = Ref.GetBoneName(B);
        for (int32 F = 0; F < Count; ++F)
        {
            const FTransform Pose = Model->IsValidBoneTrackName(Track.BoneName)
                ? Model->EvaluateBoneTrackTransform(Track.BoneName, FFrameTime::FromDecimal((Start + F / 30.0) * Model->GetFrameRate().AsDecimal()), EAnimInterpolationType::Linear)
                : Ref.GetRefBonePose()[B];
            Track.Positions.Add(FVector3f(Pose.GetTranslation()));
            Track.Rotations.Add(FQuat4f(Pose.GetRotation().GetNormalized()));
            Track.Scales.Add(FVector3f(Pose.GetScale3D()));
        }
        Data.Bones.Add(MoveTemp(Track));
    }
    return Data;
}
FTransform Pose(const FBoneSamples& Track, int32 F)
{
    return FTransform(FQuat(Track.Rotations[F]), FVector(Track.Positions[F]), FVector(Track.Scales[F]));
}
void Set(FBoneSamples& Track, int32 F, const FTransform& T)
{
    Track.Positions[F] = FVector3f(T.GetTranslation()); Track.Rotations[F] = FQuat4f(T.GetRotation().GetNormalized()); Track.Scales[F] = FVector3f(T.GetScale3D());
}
// A deliberate in-place output policy, applied equally to source loops and model results.
// Preserve height/articulation; center pelvis XY, remove root yaw/translation, retain all bones.
void InPlace(FAnimationSamples& Data, const FReferenceSkeleton& Ref)
{
    const int32 Pelvis = Ref.FindBoneIndex(TEXT("pelvis"));
    for (int32 F = 0; F < Data.NumSamples; ++F)
    {
        TArray<FTransform> World;
        for (int32 B = 0; B < Ref.GetNum(); ++B)
        {
            FTransform T = Pose(Data.Bones[B], F);
            if (Ref.GetParentIndex(B) >= 0) T = T * World[Ref.GetParentIndex(B)];
            World.Add(T);
        }
        const FQuat Rotation = FRotator(0, -World[0].Rotator().Yaw, 0).Quaternion();
        const FVector Center = Rotation.RotateVector(World[Pelvis].GetTranslation());
        for (int32 B = 1; B < World.Num(); ++B)
        {
            FVector P = Rotation.RotateVector(World[B].GetTranslation()); P.X -= Center.X; P.Y -= Center.Y;
            World[B].SetTranslation(P); World[B].SetRotation((Rotation * World[B].GetRotation()).GetNormalized());
        }
        World[0] = Ref.GetRefBonePose()[0];
        for (int32 B = 0; B < Ref.GetNum(); ++B)
            Set(Data.Bones[B], F, Ref.GetParentIndex(B) >= 0 ? World[B].GetRelativeTransform(World[Ref.GetParentIndex(B)]) : World[B]);
    }
}
void Append(FAnimationSamples& Out, const FAnimationSamples& From)
{
    for (int32 B = 0; B < Out.Bones.Num(); ++B)
        for (int32 F = 1; F < From.NumSamples; ++F)
        {
            Out.Bones[B].Positions.Add(From.Bones[B].Positions[F]);
            Out.Bones[B].Rotations.Add(From.Bones[B].Rotations[F]);
            Out.Bones[B].Scales.Add(From.Bones[B].Scales[F]);
        }
    Out.NumSamples += From.NumSamples - 1;
}
}
UAnimSequence* PrepareTransitionMotion(UAnimSequence& Original, const FString& Folder, int32& Samples, FString& Error)
{
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, OutputMannyMeshPath);
    if (!Mesh || Original.GetSkeleton() != Mesh->GetSkeleton() || !Original.GetDataModel() || Original.IsValidAdditive())
    { Error = TEXT("Choose a non-additive Animation Sequence on the Manny skeleton."); return nullptr; }
    Samples = FMath::FloorToInt(Original.GetPlayLength() * 30.0 + 1e-5) + 1;
    if (Samples < 11 || Samples > 300)
    { Error = TEXT("Loop length must contain 11..300 samples at 30 fps (0.34..9.97 seconds)."); return nullptr; }
    auto Data = TransitionAssets::Read(Original, Mesh->GetRefSkeleton(), 0, Samples);
    TransitionAssets::InPlace(Data, Mesh->GetRefSkeleton());
    // Close the loop over its final three intervals. Original assets are never changed.
    for (auto& Track : Data.Bones)
        for (int32 F = Samples - 4; F < Samples; ++F)
        {
            const float T = (F - (Samples - 4)) / 3.0f;
            FTransform Blended; Blended.Blend(TransitionAssets::Pose(Track, F), TransitionAssets::Pose(Track, 0), T * T * (3 - 2 * T));
            TransitionAssets::Set(Track, F, Blended);
        }
    return CreateAndSaveAnimation(*Mesh, Data, Error, Folder / TEXT("AN_Loop"));
}
bool BuildTransitionConnection(UAnimSequence& Source, UAnimSequence& Destination, UAnimSequence& Full,
    int32 Frames, const FString& Folder, FString& Playback, FString& Preview, FString& Error)
{
    auto* Mesh = LoadObject<USkeletalMesh>(nullptr, OutputMannyMeshPath);
    if (!Mesh || Frames < 5 || Frames > 60 || !Full.GetDataModel() || Full.GetDataModel()->GetNumberOfKeys() != Frames + 11
        || Full.GetDataModel()->GetFrameRate() != FFrameRate(30, 1) || Source.GetSkeleton() != Mesh->GetSkeleton()
        || Destination.GetSkeleton() != Mesh->GetSkeleton() || Full.GetSkeleton() != Mesh->GetSkeleton())
    { Error = TEXT("Generated clip does not match this connection's Manny skeleton, N or 30 fps contract."); return false; }
    const auto& Ref = Mesh->GetRefSkeleton();
    auto A = TransitionAssets::Read(Source, Ref, Source.GetPlayLength() - 9.0 / 30.0, 10);
    auto B = TransitionAssets::Read(Destination, Ref, 0, FMath::Min(16, Destination.GetDataModel()->GetNumberOfKeys()));
    auto Bridge = TransitionAssets::Read(Full, Ref, 9.0 / 30.0, Frames + 2);
    TransitionAssets::InPlace(Bridge, Ref);
    for (int32 Bone = 0; Bone < Bridge.Bones.Num(); ++Bone)
    {
        auto& Track = Bridge.Bones[Bone];
        const FTransform TargetA = TransitionAssets::Pose(A.Bones[Bone], 9), TargetB = TransitionAssets::Pose(B.Bones[Bone], 0);
        const FTransform OldA = TransitionAssets::Pose(Track, 0), OldB = TransitionAssets::Pose(Track, Bridge.NumSamples - 1);
        const FQuat CorrectionA = TargetA.GetRotation() * OldA.GetRotation().Inverse();
        const FQuat CorrectionB = TargetB.GetRotation() * OldB.GetRotation().Inverse();
        for (int32 F = 0; F < Bridge.NumSamples; ++F)
        {
            const double T = F / double(Bridge.NumSamples - 1), Smooth = T * T * (3 - 2 * T);
            FTransform P = TransitionAssets::Pose(Track, F);
            P.AddToTranslation(FMath::Lerp(TargetA.GetTranslation() - OldA.GetTranslation(), TargetB.GetTranslation() - OldB.GetTranslation(), Smooth));
            P.SetRotation((FQuat::Slerp(CorrectionA, CorrectionB, Smooth) * P.GetRotation()).GetNormalized());
            P.SetScale3D(FMath::Lerp(TargetA.GetScale3D(), TargetB.GetScale3D(), Smooth));
            TransitionAssets::Set(Track, F, P);
        }
        TransitionAssets::Set(Track, 0, TargetA); TransitionAssets::Set(Track, Bridge.NumSamples - 1, TargetB);
    }
    auto* Clip = CreateAndSaveAnimation(*Mesh, Bridge, Error, Folder / TEXT("AN_Transition"));
    if (!Clip) return false;
    Playback = Clip->GetPathName();
    TransitionAssets::Append(A, Bridge); TransitionAssets::Append(A, B);
    auto* Combined = CreateAndSaveAnimation(*Mesh, A, Error, Folder / TEXT("AN_Preview"));
    if (!Combined) return false;
    Preview = Combined->GetPathName(); return true;
}
}
