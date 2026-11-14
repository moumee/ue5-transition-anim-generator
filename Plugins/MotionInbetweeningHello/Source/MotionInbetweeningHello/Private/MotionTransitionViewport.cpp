#include "MotionTransitionViewport.h"
#include "MotionTransitionAnimInstance.h"
#include "MotionInbetweeningOutputData.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
namespace MotionInbetweening
{
void STransitionViewport::Construct(const FArguments&)
{
    Scene=MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false));
    Component=NewObject<USkeletalMeshComponent>();Component->SetSkeletalMesh(LoadObject<USkeletalMesh>(nullptr,OutputMannyMeshPath));
    Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);Component->VisibilityBasedAnimTickOption=EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
    Scene->AddComponent(Component,FTransform::Identity);
    Component->SetVisibility(false);
    SEditorViewport::Construct(SEditorViewport::FArguments());
}
TSharedRef<FEditorViewportClient> STransitionViewport::MakeEditorViewportClient()
{
    auto ViewClient=MakeShared<FEditorViewportClient>(nullptr,Scene.Get(),SharedThis(this));
    ViewClient->SetViewLocation(FVector(240,240,140));ViewClient->SetViewRotation(FRotator(-10,-135,0));ViewClient->SetViewMode(VMI_Lit);
    ViewClient->SetRealtime(true);ViewClient->EngineShowFlags.SetGrid(true);ViewClient->EngineShowFlags.SetSelectionOutline(false);
    return ViewClient;
}
void STransitionViewport::Tick(const FGeometry& G,double T,float Dt)
{
    SEditorViewport::Tick(G,T,Dt);
    if(Component&&IsVisible()){Component->TickAnimation(FMath::Min(Dt,0.25f),false);Component->RefreshBoneTransforms();Component->MarkRenderDynamicDataDirty();}
}
void STransitionViewport::SetBlueprint(UAnimBlueprint* BP)
{
    if(!BP||!BP->GeneratedClass)return;
    Component->SetVisibility(true);Component->SetAnimationMode(EAnimationMode::AnimationBlueprint);Component->SetAnimInstanceClass(BP->GeneratedClass);Component->InitAnim(true);Invalidate();
}
void STransitionViewport::SetClip(UAnimSequence* Clip)
{
    if(!Clip)return;Component->SetVisibility(true);Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);Component->PlayAnimation(Clip,true);Invalidate();
}
void STransitionViewport::Clear()
{Component->SetAnimationMode(EAnimationMode::AnimationSingleNode);Component->SetAnimation(nullptr);Component->SetVisibility(false);Invalidate();}
void STransitionViewport::Request(FName Name)
{if(auto* Instance=Cast<UMotionTransitionAnimInstance>(Component->GetAnimInstance()))Instance->RequestMotion(Name);}
FString STransitionViewport::Status() const
{
    if(!Component->IsVisible())return TEXT("동작이나 연결을 선택해 재생하거나, 생성 + ABP를 실행하세요.");
    if(auto* I=Cast<UMotionTransitionAnimInstance>(Component->GetAnimInstance()))return FString::Printf(TEXT("%s%s  |  %.2fs  |  %s"),*I->ActiveMotion.ToString(),I->bInTransition?*(TEXT(" -> ")+I->DestinationMotion.ToString()):TEXT(""),I->PlaybackTime,*I->PlaybackStatus);
    return TEXT("Clip preview. Select ABP preview to test motion requests.");
}
}
