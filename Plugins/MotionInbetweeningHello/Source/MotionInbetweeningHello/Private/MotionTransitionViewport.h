#pragma once
#include "SEditorViewport.h"
#include "PreviewScene.h"
class UAnimBlueprint;
class UAnimSequence;
class USkeletalMeshComponent;
class UMotionTransitionAnimInstance;
namespace MotionInbetweening
{
class STransitionViewport : public SEditorViewport
{
public:
    SLATE_BEGIN_ARGS(STransitionViewport){} SLATE_END_ARGS()
    void Construct(const FArguments&);
    void SetBlueprint(UAnimBlueprint* BP);
    void SetClip(UAnimSequence* Clip);
    void Clear();
    void Request(FName Name);
    FString Status() const;
    virtual void Tick(const FGeometry&,double,float) override;
protected:
    virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
private:
    TUniquePtr<FPreviewScene> Scene;
    USkeletalMeshComponent* Component=nullptr;
};
}
