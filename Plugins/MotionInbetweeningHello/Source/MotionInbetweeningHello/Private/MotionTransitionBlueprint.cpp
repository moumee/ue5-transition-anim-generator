#include "MotionTransitionEditor.h"
#include "MotionTransitionAnimInstance.h"
#include "MotionInbetweeningOutputData.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequenceEvaluator.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "K2Node_VariableGet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MotionInbetweening
{
UAnimBlueprint* BuildTransitionBlueprint(const FTransitionEditorModel& Set,FString& Error)
{
    auto* Mesh=LoadObject<USkeletalMesh>(nullptr,OutputMannyMeshPath);
    if(!Mesh||Set.Motions.IsEmpty()){Error=TEXT("No Manny motion data.");return nullptr;}
    FString PackageName,AssetName;
    FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateUniqueAssetName(Set.OutputFolder()/TEXT("ABP_Transitions"),TEXT(""),PackageName,AssetName);
    if(FPackageName::DoesPackageExist(PackageName)||FindPackage(nullptr,*PackageName)){Error=TEXT("Refusing to overwrite ABP.");return nullptr;}
    auto* Package=CreatePackage(*PackageName); auto* Factory=NewObject<UAnimBlueprintFactory>();
    Factory->ParentClass=UMotionTransitionAnimInstance::StaticClass();Factory->TargetSkeleton=Mesh->GetSkeleton();Factory->PreviewSkeletalMesh=Mesh;
    auto* BP=Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(),Package,*AssetName,RF_Public|RF_Standalone|RF_Transactional,nullptr,GWarn));
    if(!BP){Error=TEXT("ABP factory failed.");return nullptr;}
    auto Fail=[&](const FString& E)->UAnimBlueprint*{Error=E;BP->ClearFlags(RF_Public|RF_Standalone);BP->MarkAsGarbage();Package->SetDirtyFlag(false);return nullptr;};
    UEdGraph* Graph=nullptr;
    for(UEdGraph* Candidate:BP->FunctionGraphs)if(Candidate->GetFName()==UEdGraphSchema_K2::GN_AnimGraph)Graph=Candidate;
    if(!Graph)return Fail(TEXT("No AnimGraph."));
    UAnimGraphNode_Root* Root=nullptr;for(UEdGraphNode* Node:Graph->Nodes)if(auto* R=Cast<UAnimGraphNode_Root>(Node))Root=R;
    if(!Root)return Fail(TEXT("No output pose."));
    auto* Evaluator=NewObject<UAnimGraphNode_SequenceEvaluator>(Graph,NAME_None,RF_Transactional);
    Graph->AddNode(Evaluator,false,false);Evaluator->CreateNewGuid();Evaluator->PostPlacedNewNode();Evaluator->AllocateDefaultPins();
    Evaluator->Node.SetSequence(LoadObject<UAnimSequence>(nullptr,*Set.Motions[0].Prepared));
    Evaluator->Node.SetShouldLoop(false);Evaluator->Node.SetTeleportToExplicitTime(true);
    for(auto& Pin:Evaluator->ShowPinForProperties)if(Pin.PropertyName==TEXT("Sequence")||Pin.PropertyName==TEXT("ExplicitTime"))Pin.bShowPin=true;
    Evaluator->ReconstructNode();Evaluator->NodePosX=-300;Evaluator->NodePosY=0;
    Evaluator->NodeComment=TEXT("Motion Transition: request a motion by name. Runtime scheduler waits for loop end and preserves overshoot. In-place pose playback; no notifies/root motion.");
    Evaluator->bCommentBubbleVisible=true;
    auto Bind=[&](const TCHAR* Property,const TCHAR* PinName,int32 Y)
    {
        auto* Getter=NewObject<UK2Node_VariableGet>(Graph,NAME_None,RF_Transactional);
        Getter->VariableReference.SetSelfMember(Property);Graph->AddNode(Getter,false,false);Getter->CreateNewGuid();Getter->PostPlacedNewNode();Getter->AllocateDefaultPins();Getter->NodePosX=-700;Getter->NodePosY=Y;
        auto* Pin=Evaluator->FindPin(PinName);
        return Pin&&Graph->GetSchema()->TryCreateConnection(Getter->GetValuePin(),Pin);
    };
    if(!Bind(TEXT("PlaybackSequence"),TEXT("Sequence"),0)||!Bind(TEXT("PlaybackTime"),TEXT("ExplicitTime"),120)
        ||!Graph->GetSchema()->TryCreateConnection(Evaluator->FindPin(TEXT("Pose")),Root->FindPin(TEXT("Result"))))
        return Fail(TEXT("Could not bind the runtime playback inputs."));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);FCompilerResultsLog Log;
    FKismetEditorUtilities::CompileBlueprint(BP,EBlueprintCompileOptions::None,&Log);
    if(Log.NumErrors||BP->Status==BS_Error||!BP->GeneratedClass)return Fail(TEXT("Generated ABP compile failed. See Compiler Results."));
    auto Configure=[&](UClass* Class)
    {
        auto* Defaults=Class?Cast<UMotionTransitionAnimInstance>(Class->GetDefaultObject()):nullptr;if(!Defaults)return;
        Defaults->Modify();Defaults->Motions.Reset();Defaults->Transitions.Reset();
        for(const auto& M:Set.Motions){FMotionTransitionState State;State.Name=FName(*M.Name);State.Animation=LoadObject<UAnimSequence>(nullptr,*M.Prepared);Defaults->Motions.Add(State);}
        for(const auto& E:Set.Connections){FMotionTransitionEdge Edge;Edge.From=FName(*Set.Motion(E.From)->Name);Edge.To=FName(*Set.Motion(E.To)->Name);Edge.Animation=LoadObject<UAnimSequence>(nullptr,*E.Playback);Defaults->Transitions.Add(Edge);}
        Defaults->DefaultMotion=FName(*Set.Motion(Set.DefaultMotion)->Name);Defaults->RequestedMotion=Defaults->DefaultMotion;
    };
    Configure(BP->GeneratedClass);Configure(BP->SkeletonGeneratedClass);FBlueprintEditorUtils::MarkBlueprintAsModified(BP);
    // Updating class defaults marks the Blueprint dirty. UE suppresses its root graph
    // while BS_Dirty, so compile the configured defaults before returning a playable ABP.
    FCompilerResultsLog ConfiguredLog;
    FKismetEditorUtilities::CompileBlueprint(BP,EBlueprintCompileOptions::None,&ConfiguredLog);
    if(ConfiguredLog.NumErrors||BP->Status==BS_Error||BP->Status==BS_Dirty)return Fail(TEXT("Configured ABP compile failed."));
    BP->MarkPackageDirty();FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
    if(!UPackage::SavePackage(Package,BP,*FPackageName::LongPackageNameToFilename(PackageName,FPackageName::GetAssetPackageExtension()),Args))return Fail(TEXT("Could not save generated ABP."));
    FAssetRegistryModule::AssetCreated(BP);return BP;
}
}
