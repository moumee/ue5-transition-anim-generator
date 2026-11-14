#if WITH_DEV_AUTOMATION_TESTS
#include "MotionTransitionEditor.h"
#include "MotionTransitionAnimInstance.h"
#include "MotionInbetweeningOutputData.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimClassInterface.h"
#include "AnimNodes/AnimNode_SequenceEvaluator.h"
#include "UObject/UnrealType.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"

namespace TransitionEditorTests
{
using namespace MotionInbetweening;
FString Records()
{FString Path=FPlatformMisc::GetEnvironmentVariable(TEXT("MIB_PLAYBACK_RECORD_DIR"));return Path.IsEmpty()?FPaths::ProjectSavedDir()/TEXT("MotionInbetweening/IntegratedTests"):Path;}
void CheckEndpoints(FAutomationTestBase& Test,const FTransitionEditorModel& Set)
{
    for(const auto& E:Set.Connections)
    {
        auto* A=LoadObject<UAnimSequence>(nullptr,*Set.Motion(E.From)->Prepared);
        auto* B=LoadObject<UAnimSequence>(nullptr,*Set.Motion(E.To)->Prepared);
        auto* T=LoadObject<UAnimSequence>(nullptr,*E.Playback);
        if(!Test.TestNotNull(TEXT("Prepared source"),A)||!Test.TestNotNull(TEXT("Prepared destination"),B)||!Test.TestNotNull(TEXT("Generated playback"),T))return;
        Test.TestEqual(TEXT("Transition contains anchor + N + target"),T->GetDataModel()->GetNumberOfKeys(),E.Frames+2);
        double MaxPosition=0,MaxAngle=0;
        auto* Mesh=LoadObject<USkeletalMesh>(nullptr,OutputMannyMeshPath);
        for(int32 Bone=0;Bone<Mesh->GetRefSkeleton().GetNum();++Bone)
        {
            const FName Name=Mesh->GetRefSkeleton().GetBoneName(Bone);
            for(int32 Side=0;Side<2;++Side)
            {
                const auto P=T->GetDataModel()->GetBoneTrackTransform(Name,FFrameNumber(Side?E.Frames+1:0));
                const auto Expected=(Side?B:A)->GetDataModel()->GetBoneTrackTransform(Name,FFrameNumber(Side?0:A->GetDataModel()->GetNumberOfKeys()-1));
                MaxPosition=FMath::Max(MaxPosition,FVector::Distance(P.GetTranslation(),Expected.GetTranslation()));
                MaxAngle=FMath::Max(MaxAngle,FMath::RadiansToDegrees(P.GetRotation().AngularDistance(Expected.GetRotation())));
            }
        }
        Test.TestTrue(TEXT("Every boundary bone matches within 0.001 cm"),MaxPosition<.001);
        Test.TestTrue(TEXT("Every boundary bone matches within 0.05 degrees"),MaxAngle<.05);
        Test.AddInfo(FString::Printf(TEXT("Boundary %s: %.8f cm, %.8f degrees"),*E.Id,MaxPosition,MaxAngle));
    }
}
void CheckRuntime(FAutomationTestBase& Test,UAnimBlueprint* BP)
{
    if(!Test.TestNotNull(TEXT("Generated ABP"),BP))return;
    auto* Defaults=Cast<UMotionTransitionAnimInstance>(BP->GeneratedClass->GetDefaultObject());
    if(!Test.TestNotNull(TEXT("Runtime parent"),Defaults))return;
    Test.TestEqual(TEXT("Three saved motions"),Defaults->Motions.Num(),3);Test.TestEqual(TEXT("Six directed transitions"),Defaults->Transitions.Num(),6);
    if(Defaults->Motions.Num()!=3||Defaults->Transitions.Num()!=6)return;
    const FName Start=Defaults->DefaultMotion;
    const auto& Edge=*Defaults->Transitions.FindByPredicate([&](const auto& E){return E.From==Start;});
    auto* TestOwner=NewObject<USkeletalMeshComponent>();
    auto* I=NewObject<UMotionTransitionAnimInstance>(TestOwner);I->Motions=Defaults->Motions;I->Transitions=Defaults->Transitions;I->DefaultMotion=Start;I->ResetPlayback();
    const float Loop=I->PlaybackSequence->GetPlayLength();
    I->NativeUpdateAnimation(.1f);I->RequestMotion(Edge.To);I->NativeUpdateAnimation(.1f);
    Test.TestFalse(TEXT("Request waits for loop boundary"),I->bInTransition);
    I->RequestMotion(Start);I->NativeUpdateAnimation(Loop);
    Test.TestFalse(TEXT("Requesting current state cancels pending transition"),I->bInTransition);
    I->ResetPlayback();I->RequestMotion(Edge.To);I->NativeUpdateAnimation(Loop+.12f);
    Test.TestTrue(TEXT("Large tick reaches bridge"),I->bInTransition);
    Test.TestTrue(TEXT("Bridge receives overshoot"),FMath::IsNearlyEqual(I->PlaybackTime,.12f,1e-4f));
    FName Third=NAME_None;for(const auto& M:I->Motions)if(M.Name!=Start&&M.Name!=Edge.To)Third=M.Name;
    I->RequestMotion(Third);I->NativeUpdateAnimation(Edge.Animation->GetPlayLength()-.12f+.07f);
    Test.TestEqual(TEXT("Mid-transition request does not replace current destination"),I->ActiveMotion,Edge.To);
    Test.TestTrue(TEXT("Destination receives overshoot"),FMath::IsNearlyEqual(I->PlaybackTime,.07f,1e-4f));
    Test.TestEqual(TEXT("Latest request remains queued"),I->RequestedMotion,Third);
    I->RequestMotion(TEXT("NotConnected"));I->NativeUpdateAnimation(1.f);Test.TestEqual(TEXT("Missing connection keeps motion"),I->ActiveMotion,Edge.To);
    for(float Dt:{1.f/30,1.f/60,.12f})
    {
        FPreviewScene Scene(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false));
        auto* Mesh=LoadObject<USkeletalMesh>(nullptr,OutputMannyMeshPath);auto* Component=NewObject<USkeletalMeshComponent>();
        Component->SetSkeletalMesh(Mesh);Component->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
        Scene.AddComponent(Component,FTransform::Identity);Component->SetAnimInstanceClass(BP->GeneratedClass);Component->InitAnim(true);
        auto* Runtime=Cast<UMotionTransitionAnimInstance>(Component->GetAnimInstance());
        if(!Test.TestNotNull(TEXT("Generated evaluator instantiates"),Runtime))return;
        Runtime->RequestMotion(Edge.To);bool SawBridge=false,SawTarget=false;double MaxPosition=0,MaxAngle=0;int32 DebugCount=0;
        for(int32 Tick=0;Tick<FMath::CeilToInt((Loop+Edge.Animation->GetPlayLength()+1)/Dt);++Tick)
        {
            Component->TickAnimation(Dt,false);Component->RefreshBoneTransforms();SawBridge|=Runtime->bInTransition;SawTarget|=Runtime->ActiveMotion==Edge.To;
            for(const auto& Pose:Component->GetBoneSpaceTransforms())if(Pose.ContainsNaN()){Test.AddError(TEXT("Non-finite generated ABP pose"));return;}
            Test.TestTrue(TEXT("No accumulated root translation"),Component->GetBoneSpaceTransforms()[0].GetTranslation().Size()<.001);
            if(auto* Playing=Cast<UAnimSequence>(Runtime->PlaybackSequence))for(const TCHAR* Bone:{TEXT("pelvis"),TEXT("calf_l"),TEXT("upperarm_l")})
            {
                const int32 Index=Mesh->GetRefSkeleton().FindBoneIndex(Bone);
                const auto Expected=Playing->GetDataModel()->EvaluateBoneTrackTransform(Bone,FFrameTime::FromDecimal(Runtime->PlaybackTime*30.0),EAnimInterpolationType::Linear);
                const auto& Actual=Component->GetBoneSpaceTransforms()[Index];
                MaxPosition=FMath::Max(MaxPosition,FVector::Distance(Actual.GetTranslation(),Expected.GetTranslation()));
                MaxAngle=FMath::Max(MaxAngle,FMath::RadiansToDegrees(Actual.GetRotation().AngularDistance(Expected.GetRotation())));
                if(DebugCount<3&&FMath::RadiansToDegrees(Actual.GetRotation().AngularDistance(Expected.GetRotation()))>1)
                {
                    ++DebugCount;Test.AddInfo(FString::Printf(TEXT("POSE_DEBUG bone=%s tick=%d t=%.5f seq=%s expected=%s actual=%s"),Bone,Tick,Runtime->PlaybackTime,*Playing->GetName(),*Expected.ToString(),*Actual.ToString()));
                    for(auto* P:IAnimClassInterface::GetFromClass(BP->GeneratedClass)->GetAnimNodeProperties())
                        if(P->Struct==FAnimNode_SequenceEvaluator::StaticStruct())
                        {auto* N=P->ContainerPtrToValuePtr<FAnimNode_SequenceEvaluator>(Runtime);Test.AddInfo(FString::Printf(TEXT("EVALUATOR_DEBUG t=%.5f seq=%s"),N->GetExplicitTime(),N->GetSequence()?*N->GetSequence()->GetName():TEXT("null")));}
                }
            }
        }
        Test.TestTrue(TEXT("Actual evaluator played bridge"),SawBridge);Test.TestTrue(TEXT("Actual evaluator reached requested motion"),SawTarget);
        Test.TestTrue(TEXT("Evaluator follows selected sequence/time within 0.5cm"),MaxPosition<.5);
        Test.TestTrue(TEXT("Evaluator follows selected sequence/time within 1 degree"),MaxAngle<1);
        Test.AddInfo(FString::Printf(TEXT("Runtime evaluated at dt=%.6f, max pose difference %.6f cm / %.6f deg"),Dt,MaxPosition,MaxAngle));
    }
}
class FRun : public IAutomationLatentCommand
{
    FAutomationTestBase* Test;TSharedPtr<FTransitionEditorModel> Set;int32 Phase=0;double Started=FPlatformTime::Seconds();
    TArray<FString> OldPlaybacks;FString OldBlueprint;
public:
    explicit FRun(FAutomationTestBase* InTest):Test(InTest)
    {
        Set=MakeShared<FTransitionEditorModel>();Set->Name=TEXT("Idle · Walk · Run — 검증 예제");
        TArray<UAnimSequence*> Assets;
        for(const TCHAR* Name:{TEXT("MM_Idle"),TEXT("MM_Walk_Fwd"),TEXT("MM_Run_Fwd")})
            Assets.Add(LoadObject<UAnimSequence>(nullptr,*(FString(TEXT("/Game/ControlRig/Characters/Mannequins/Animations/Manny/"))+Name+TEXT(".")+Name)));
        for(auto* A:Assets)Test->TestNotNull(TEXT("Fixture motion"),A);
        Set->AddMotions(Assets);
    }
    virtual bool Update()override
    {
        if(FPlatformTime::Seconds()-Started>900){Set->Cancel();Test->AddError(TEXT("Integrated workflow timeout"));return true;}
        if(Phase==2&&IsMannyInferenceRunning())
        {
            Set->Cancel();Test->TestFalse(TEXT("Cancellation stops active inference"),IsMannyInferenceRunning());
            Test->TestFalse(TEXT("Cancellation leaves no active queue"),Set->IsBusy());
            if(!Set->Generate()){Test->AddError(Set->Message);return true;}Phase=3;return false;
        }
        if(Set->IsBusy())return false;
        if(Phase==0)
        {
            if(!Test->TestEqual(TEXT("Three selected motions"),Set->Motions.Num(),3))return true;
            for(const auto& M:Set->Motions)if(!Test->TestTrue(*(FString(TEXT("Prepared "))+M.Name), M.Status==TEXT("Ready"))){Test->AddError(M.Error);return true;}
            Set->ConnectAll();Test->TestEqual(TEXT("All directions without self edges"),Set->Connections.Num(),6);
            for(int32 I=0;I<Set->Connections.Num();++I)Set->SetFrames(Set->Connections[I].Id,5+5*I);
            if(!Set->Generate()){Test->AddError(Set->Message);return true;}Phase=1;return false;
        }
        if(Phase==1)
        {
            for(const auto& E:Set->Connections)if(E.Status!=TEXT("Ready")){Test->AddError(E.Error);return true;}
            CheckEndpoints(*Test,*Set);CheckRuntime(*Test,LoadObject<UAnimBlueprint>(nullptr,*Set->Blueprint));
            for(const auto& E:Set->Connections)OldPlaybacks.Add(E.Playback);OldBlueprint=Set->Blueprint;
            Set->SetFrames(Set->Connections[0].Id,7);
            if(!Set->Generate()){Test->AddError(Set->Message);return true;}Phase=2;return false;
        }
        for(const auto& E:Set->Connections)if(E.Status!=TEXT("Ready")){Test->AddError(E.Error);return true;}
        Test->TestNotEqual(TEXT("Changed connection regenerated"),Set->Connections[0].Playback,OldPlaybacks[0]);
        for(int32 I=1;I<OldPlaybacks.Num();++I)Test->TestEqual(TEXT("Unchanged connection preserved"),Set->Connections[I].Playback,OldPlaybacks[I]);
        Test->TestNotEqual(TEXT("ABP regenerated into new version"),Set->Blueprint,OldBlueprint);CheckEndpoints(*Test,*Set);
        auto Reload=MakeShared<FTransitionEditorModel>();Test->TestTrue(TEXT("Set reloads"),Reload->Load(Set->SetPath()));
        Test->TestEqual(TEXT("Saved graph edges persist"),Reload->Connections.Num(),6);
        Test->TestEqual(TEXT("Saved ABP reference persists"),Reload->Blueprint,Set->Blueprint);
        IFileManager::Get().MakeDirectory(*Records(),true);FFileHelper::SaveStringToFile(Set->SetPath(),*(Records()/TEXT("integrated-set.txt")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        Test->AddInfo(TEXT("INTEGRATED_SET=")+Set->SetPath());return true;
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIntegratedCreate,"MotionInbetweeningUX.Integrated.Create",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIntegratedCreate::RunTest(const FString&){ADD_LATENT_AUTOMATION_COMMAND(TransitionEditorTests::FRun(this));return true;}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIntegratedReload,"MotionInbetweeningUX.Integrated.Reload",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIntegratedReload::RunTest(const FString&)
{
    FString Path; if(!TestTrue(TEXT("Saved workflow handoff"),FFileHelper::LoadFileToString(Path,*(TransitionEditorTests::Records()/TEXT("integrated-set.txt")))))return false;
    auto Set=MakeShared<MotionInbetweening::FTransitionEditorModel>();if(!TestTrue(TEXT("New-process set reload"),Set->Load(Path)))return false;
    TransitionEditorTests::CheckEndpoints(*this,*Set);TransitionEditorTests::CheckRuntime(*this,LoadObject<UAnimBlueprint>(nullptr,*Set->Blueprint));return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FIntegratedBlueprint,"MotionInbetweeningUX.Integrated.Blueprint",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FIntegratedBlueprint::RunTest(const FString&)
{
    FString Path,Error;if(!TestTrue(TEXT("Saved workflow handoff"),FFileHelper::LoadFileToString(Path,*(TransitionEditorTests::Records()/TEXT("integrated-set.txt")))))return false;
    auto Set=MakeShared<MotionInbetweening::FTransitionEditorModel>();if(!TestTrue(TEXT("Load ready graph"),Set->Load(Path)))return false;
    auto* BP=MotionInbetweening::BuildTransitionBlueprint(*Set,Error);if(!BP){AddError(Error);return false;}
    TransitionEditorTests::CheckRuntime(*this,BP);return true;
}
#endif
