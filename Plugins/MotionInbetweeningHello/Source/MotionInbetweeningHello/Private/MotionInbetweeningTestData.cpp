#include "MotionInbetweeningAnimationData.h"

#include "Animation/Skeleton.h"

namespace MotionInbetweening
{
bool BuildMannyTestData(const USkeleton& Skeleton, FAnimationSamples& OutData, FString& OutError)
{
	OutData = FAnimationSamples();
	OutError.Reset();
	OutData.NumSamples = 61; // 60 intervals / 30 fps = 2 seconds, endpoints included.
	const FReferenceSkeleton& Reference = Skeleton.GetReferenceSkeleton();
	const FName BoneNames[] = { TEXT("root"), TEXT("spine_03"), TEXT("upperarm_l"), TEXT("lowerarm_l") };

	for (const FName BoneName : BoneNames)
	{
		const int32 BoneIndex = Reference.FindBoneIndex(BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Manny test data requires bone '%s' in %s."), *BoneName.ToString(), *Skeleton.GetPathName());
			OutData = FAnimationSamples();
			return false;
		}

		const FTransform& RefPose = Reference.GetRefBonePose()[BoneIndex];
		FBoneSamples& Track = OutData.Bones.AddDefaulted_GetRef();
		Track.BoneName = BoneName;
		for (int32 Sample = 0; Sample < OutData.NumSamples; ++Sample)
		{
			const double Phase = Sample == OutData.NumSamples - 1 ? 0.0 : 2.0 * PI * Sample / (OutData.NumSamples - 1);
			double Degrees = 0.0;
			FVector Axis = FVector::YAxisVector;
			if (BoneName == TEXT("spine_03"))
			{
				Axis = FVector::XAxisVector;
				Degrees = 8.0 * FMath::Sin(Phase);
			}
			else if (BoneName == TEXT("upperarm_l"))
			{
				Degrees = 35.0 * FMath::Sin(Phase);
			}
			else if (BoneName == TEXT("lowerarm_l"))
			{
				Degrees = 55.0 * (1.0 - FMath::Cos(Phase)) * 0.5;
			}
			// Keep the reference translation/scale. Apply a small bone-local rotation
			// after the actual skeleton's reference orientation, not identity rotations.
			const FQuat Rotation = (RefPose.GetRotation() * FQuat(Axis, FMath::DegreesToRadians(Degrees))).GetNormalized();
			Track.Positions.Add(FVector3f(RefPose.GetTranslation()));
			Track.Rotations.Add(FQuat4f(Rotation));
			Track.Scales.Add(FVector3f(RefPose.GetScale3D()));
		}
	}
	return ValidateAnimationData(Skeleton, OutData, OutError);
}

bool ValidateAnimationData(const USkeleton& Skeleton, const FAnimationSamples& Data, FString& OutError)
{
	OutError.Reset();
	if (Data.FrameRate.Numerator <= 0 || Data.FrameRate.Denominator <= 0 || Data.NumSamples < 2 || Data.Bones.IsEmpty())
	{
		OutError = TEXT("Animation requires a positive frame rate, at least two samples and at least one bone track.");
		return false;
	}

	TSet<FName> SeenBones;
	for (const FBoneSamples& Track : Data.Bones)
	{
		if (Skeleton.GetReferenceSkeleton().FindBoneIndex(Track.BoneName) == INDEX_NONE || SeenBones.Contains(Track.BoneName))
		{
			OutError = FString::Printf(TEXT("Unknown or duplicate bone '%s'."), *Track.BoneName.ToString());
			return false;
		}
		SeenBones.Add(Track.BoneName);
		if (Track.Positions.Num() != Data.NumSamples || Track.Rotations.Num() != Data.NumSamples || Track.Scales.Num() != Data.NumSamples)
		{
			OutError = FString::Printf(TEXT("Bone '%s' requires %d position/rotation/scale keys; got %d/%d/%d."),
				*Track.BoneName.ToString(), Data.NumSamples, Track.Positions.Num(), Track.Rotations.Num(), Track.Scales.Num());
			return false;
		}
		for (int32 Sample = 0; Sample < Data.NumSamples; ++Sample)
		{
			const FVector3f& Scale = Track.Scales[Sample];
			if (Track.Positions[Sample].ContainsNaN() || Track.Rotations[Sample].ContainsNaN() || Scale.ContainsNaN()
				|| !Track.Rotations[Sample].IsNormalized() || Scale.GetMin() <= UE_SMALL_NUMBER)
			{
				OutError = FString::Printf(TEXT("Invalid transform for bone '%s', sample %d: require finite values, a normalized quaternion and positive scale."),
					*Track.BoneName.ToString(), Sample);
				return false;
			}
		}
	}
	return true;
}
}
