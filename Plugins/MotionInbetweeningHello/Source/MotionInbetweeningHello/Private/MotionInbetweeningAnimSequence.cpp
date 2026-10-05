#include "MotionInbetweeningAnimSequence.h"

#include "MotionInbetweeningAnimationData.h"
#include "MotionInbetweeningHelloModule.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimSequence.h"
#include "Animation/IAnimationSequenceCompiler.h"
#include "Animation/Skeleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MotionInbetweening
{
USkeletalMesh* LoadMannyMesh(FString& OutError)
{
	OutError.Reset();
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, MannyMeshPath);
	if (!Mesh || !Mesh->GetSkeleton())
	{
		OutError = FString::Printf(TEXT("Could not load Manny mesh and its skeleton: %s"), MannyMeshPath);
		return nullptr;
	}
	UE_LOG(LogMotionInbetweeningHello, Log, TEXT("Manny mesh: %s; skeleton: %s; reference bones: %d."),
		*Mesh->GetPathName(), *Mesh->GetSkeleton()->GetPathName(), Mesh->GetSkeleton()->GetReferenceSkeleton().GetNum());
	return Mesh;
}

UAnimSequence* CreateAndSaveAnimation(USkeletalMesh& PreviewMesh, const FAnimationSamples& Data, FString& OutError, const FString& AssetBasePath)
{
	USkeleton* Skeleton = PreviewMesh.GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("The preview mesh has no skeleton.");
		return nullptr;
	}
	if (!ValidateAnimationData(*Skeleton, Data, OutError))
	{
		return nullptr; // Validate before allocating or writing any package.
	}

	FString PackageName;
	FString AssetName;
	FAssetToolsModule& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	if (!AssetBasePath.StartsWith(TEXT("/Game/MotionInbetweening/")) || !FPackageName::IsValidLongPackageName(AssetBasePath))
	{
		OutError = TEXT("Use a valid asset path under /Game/MotionInbetweening/.");
		return nullptr;
	}
	AssetTools.Get().CreateUniqueAssetName(AssetBasePath, TEXT(""), PackageName, AssetName);
	// Also guard the file and loaded package independently of the Asset Registry.
	if (FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName))
	{
		OutError = FString::Printf(TEXT("Refusing to overwrite existing package: %s"), *PackageName);
		return nullptr;
	}
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
	{
		OutError = FString::Printf(TEXT("Could not create output directory: %s"), *FPaths::GetPath(Filename));
		return nullptr;
	}

	UPackage* Package = CreatePackage(*PackageName);
	UAnimSequence* Sequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	Sequence->SetSkeleton(Skeleton);
	Sequence->SetPreviewMesh(&PreviewMesh);
	Sequence->bEnableRootMotion = false;
	IAnimationDataController& Controller = Sequence->GetController();
	Controller.InitializeModel();
	bool bTracksWritten = true;
	{
		const IAnimationDataController::FScopedBracket Bracket(Controller, NSLOCTEXT("MotionInbetweeningHello", "PopulateAnimation", "Populate Manny animation"), false);
		Controller.SetFrameRate(Data.FrameRate, false);
		Controller.SetNumberOfFrames(FFrameNumber(Data.NumSamples - 1), false);
		for (const FBoneSamples& Track : Data.Bones)
		{
			if (!Controller.AddBoneCurve(Track.BoneName, false)
				|| !Controller.SetBoneTrackKeys(Track.BoneName, Track.Positions, Track.Rotations, Track.Scales, false))
			{
				OutError = FString::Printf(TEXT("Animation controller rejected bone '%s'."), *Track.BoneName.ToString());
				bTracksWritten = false;
				break;
			}
		}
		if (bTracksWritten)
		{
			Controller.NotifyPopulated();
		}
	}
	if (bTracksWritten)
	{
		// Wait for derived animation data before saving or opening the preview.
		UE::Anim::IAnimSequenceCompilingManager::FinishCompilation({ Sequence });
		Sequence->MarkPackageDirty();
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (UPackage::SavePackage(Package, Sequence, *Filename, SaveArgs))
		{
			FAssetRegistryModule::AssetCreated(Sequence);
			UE_LOG(LogMotionInbetweeningHello, Display,
				TEXT("Saved %s | %d tracks, %d samples, %d intervals, %d/%d fps, %.3f seconds | %s"),
				*Sequence->GetPathName(), Data.Bones.Num(), Data.NumSamples, Data.NumSamples - 1,
				Data.FrameRate.Numerator, Data.FrameRate.Denominator, Sequence->GetPlayLength(), *FPaths::ConvertRelativePathToFull(Filename));
			return Sequence;
		}
		OutError = FString::Printf(TEXT("SavePackage failed: %s. Check directory permissions, disk space and Output Log."), *Filename);
	}
	// Failed creations should not appear as successful, unsaved assets in the browser.
	Sequence->ClearFlags(RF_Public | RF_Standalone);
	Sequence->MarkAsGarbage();
	Package->SetDirtyFlag(false);
	return nullptr;
}
}
