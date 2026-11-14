#pragma once
#include "Widgets/SLeafWidget.h"
#include "MotionTransitionEditor.h"
namespace MotionInbetweening
{
class STransitionGraph : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(STransitionGraph){} SLATE_ARGUMENT(TSharedPtr<FTransitionEditorModel>,Model) SLATE_END_ARGS()
    void Construct(const FArguments& Args){Model=Args._Model;SetCanTick(false);}
    virtual FVector2D ComputeDesiredSize(float) const override{return FVector2D(800,400);}
    virtual int32 OnPaint(const FPaintArgs&,const FGeometry&,const FSlateRect&,FSlateWindowElementList&,int32,const FWidgetStyle&,bool)const override;
    virtual FReply OnMouseButtonDown(const FGeometry&,const FPointerEvent&) override;
    virtual FReply OnMouseButtonUp(const FGeometry&,const FPointerEvent&) override;
    virtual FReply OnMouseMove(const FGeometry&,const FPointerEvent&) override;
    virtual FReply OnMouseWheel(const FGeometry&,const FPointerEvent&) override;
    virtual FReply OnDragOver(const FGeometry&,const FDragDropEvent&) override;
    virtual void OnDragLeave(const FDragDropEvent&) override;
    virtual FReply OnDrop(const FGeometry&,const FDragDropEvent&) override;
    virtual bool SupportsKeyboardFocus() const override{return true;}
    virtual FReply OnKeyDown(const FGeometry&,const FKeyEvent&) override;
private:
    TSharedPtr<FTransitionEditorModel> Model;
    FVector2D Pan=FVector2D::ZeroVector,Last=FVector2D::ZeroVector;
    float Zoom=1;
    FString Dragging,Connecting;
    bool bPanning=false;
    bool bAssetDragOver=false,bCanDropAssets=false;
    FString DropHint;
    FVector2D Screen(FVector2D P) const{return P*Zoom+Pan;}
    FVector2D Label(const FTransitionConnection&)const;
    TArray<FVector2D> Curve(const FTransitionConnection&)const;
    void Changed(){++Model->Revision;Model->OnChanged.ExecuteIfBound();}
};
}
