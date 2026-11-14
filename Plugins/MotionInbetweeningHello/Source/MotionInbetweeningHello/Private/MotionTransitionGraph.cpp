#include "MotionTransitionGraph.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "InputCoreTypes.h"
#include "Animation/AnimSequence.h"
#include "DragAndDrop/AssetDragDropOp.h"

namespace MotionInbetweening
{
namespace
{
bool IsNewMotionAsset(const FAssetData& Asset,const FTransitionEditorModel& Model)
{
    return Asset.IsInstanceOf<UAnimSequence>()&&!Model.Motions.ContainsByPredicate(
        [&](const FTransitionMotion& Motion){return Motion.Source==Asset.GetSoftObjectPath().ToString();});
}
}
TArray<FVector2D> STransitionGraph::Curve(const FTransitionConnection& E)const
{
    const auto* A=Model->Motion(E.From);const auto* B=Model->Motion(E.To);if(!A||!B)return {};
    const FVector2D Start=A->Position+FVector2D(180,34),End=B->Position+FVector2D(0,34);
    const double Bend=FMath::Max(70.0,FMath::Abs(End.X-Start.X)*0.4);
    // Give opposite directions distinct lanes; otherwise both frame labels overlap.
    const double Lane=End.X<Start.X?-110.0:65.0;
    const FVector2D C1=Start+FVector2D(Bend,Lane),C2=End+FVector2D(-Bend,Lane);
    TArray<FVector2D> Points;
    for(int32 I=0;I<=32;++I){const double T=I/32.0,U=1-T;Points.Add(Screen(U*U*U*Start+3*U*U*T*C1+3*U*T*T*C2+T*T*T*End));}
    return Points;
}
FVector2D STransitionGraph::Label(const FTransitionConnection& E)const
{
    TArray<FVector2D> Occupied;
    for(const auto& Edge:Model->Connections)
    {
        const auto P=Curve(Edge);if(!P.Num())continue;const FVector2D Base=P[16];FVector2D At=Base;
        auto Free=[&](FVector2D Candidate)
        {
            if(Candidate.Y<65)return false;
            for(const auto& M:Model->Motions)
            {const auto Min=Screen(M.Position)-FVector2D(38,24),Max=Screen(M.Position+FVector2D(180,68))+FVector2D(38,24);if(Candidate.X>=Min.X&&Candidate.X<=Max.X&&Candidate.Y>=Min.Y&&Candidate.Y<=Max.Y)return false;}
            for(const auto& Other:Occupied)if(FMath::Abs(Candidate.X-Other.X)<68&&FMath::Abs(Candidate.Y-Other.Y)<34)return false;
            return true;
        };
        if(!Free(At))for(int32 Radius=1;Radius<=20;++Radius)
        {
            bool Found=false;
            for(const FVector2D Offset:{FVector2D(0,36),FVector2D(0,-36),FVector2D(70,0),FVector2D(-70,0),FVector2D(70,36),FVector2D(-70,36)})
                if(Free(Base+Offset*Radius)){At=Base+Offset*Radius;Found=true;break;}
            if(Found)break;
        }
        Occupied.Add(At);if(Edge.Id==E.Id)return At;
    }
    return FVector2D::ZeroVector;
}
int32 STransitionGraph::OnPaint(const FPaintArgs&,const FGeometry& G,const FSlateRect&,FSlateWindowElementList& Out,int32 Layer,const FWidgetStyle&,bool)const
{
    const auto* Brush=FAppStyle::GetBrush("WhiteBrush");auto Font=FAppStyle::GetFontStyle("NormalFont");
    auto Box=[&](FVector2D P,FVector2D Size,FLinearColor Color,int32 L){FSlateDrawElement::MakeBox(Out,L,G.ToPaintGeometry(Size,FSlateLayoutTransform(P)),Brush,ESlateDrawEffect::None,Color);};
    auto Text=[&](FVector2D P,const FString& Value,FLinearColor Color,int32 L,float Scale=1.f){auto ScaledFont=Font;ScaledFont.Size=FMath::Max(6,FMath::RoundToInt(Font.Size*Scale));FSlateDrawElement::MakeText(Out,L,G.ToPaintGeometry(FVector2D(200,20)*Scale,FSlateLayoutTransform(P)),Value,ScaledFont,ESlateDrawEffect::None,Color);};
    Box(FVector2D::ZeroVector,G.GetLocalSize(),FLinearColor(.025,.03,.045),Layer);
    for(double X=FMath::Fmod(Pan.X,40*Zoom);X<G.GetLocalSize().X;X+=40*Zoom)
        FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),{FVector2D(X,0),FVector2D(X,G.GetLocalSize().Y)},ESlateDrawEffect::None,FLinearColor(.07,.08,.10),false);
    for(double Y=FMath::Fmod(Pan.Y,40*Zoom);Y<G.GetLocalSize().Y;Y+=40*Zoom)
        FSlateDrawElement::MakeLines(Out,Layer+1,G.ToPaintGeometry(),{FVector2D(0,Y),FVector2D(G.GetLocalSize().X,Y)},ESlateDrawEffect::None,FLinearColor(.07,.08,.10),false);
    for(const auto& E:Model->Connections)
    {
        const auto Points=Curve(E);if(!Points.Num())continue;
        const FLinearColor Color=E.Id==Model->SelectedConnection?FLinearColor(.95,.7,.15):(E.Status==TEXT("Ready")?FLinearColor(.2,.75,.5):(E.Status==TEXT("Failed")?FLinearColor(.9,.25,.25):FLinearColor(.35,.55,.85)));
        FSlateDrawElement::MakeLines(Out,Layer+2,G.ToPaintGeometry(),Points,ESlateDrawEffect::None,Color,true,2);
        const FVector2D End=Points.Last(),Dir=(End-Points[Points.Num()-3]).GetSafeNormal(),Normal(-Dir.Y,Dir.X);
        FSlateDrawElement::MakeLines(Out,Layer+3,G.ToPaintGeometry(),{End-Dir*12+Normal*5,End,End-Dir*12-Normal*5},ESlateDrawEffect::None,Color,true,2);
        const FVector2D At=Label(E);
        if(FVector2D::Distance(At,Points[16])>8)FSlateDrawElement::MakeLines(Out,Layer+3,G.ToPaintGeometry(),{Points[16],At},ESlateDrawEffect::None,Color.CopyWithNewOpacity(.5f),true,1);
        Box(At-FVector2D(28,12),FVector2D(56,24),FLinearColor(.06,.08,.12),Layer+3);
        Text(At-FVector2D(22,9),FString::Printf(TEXT("%d fr"),E.Frames),Color,Layer+4);
    }
    for(const auto& M:Model->Motions)
    {
        const FVector2D At=Screen(M.Position),Size=FVector2D(180,68)*Zoom;
        const FLinearColor Border=M.Id==Model->SelectedMotion||M.Id==Connecting?FLinearColor(.95,.7,.15):FLinearColor(.18,.34,.55);
        Box(At-FVector2D(2,2),Size+FVector2D(4,4),Border,Layer+5);Box(At,Size,FLinearColor(.07,.10,.16),Layer+6);
        Text(At+FVector2D(10,8)*Zoom,M.Name.Left(22)+(M.Id==Model->DefaultMotion?TEXT(" *"):TEXT("")),FLinearColor::White,Layer+7,Zoom);
        Text(At+FVector2D(10,33)*Zoom,M.Status,M.Status==TEXT("Ready")?FLinearColor(.25,.9,.6):FLinearColor(.9,.7,.35),Layer+7,Zoom);
        Box(Screen(M.Position+FVector2D(0,34))-FVector2D(6,6),FVector2D(12,12),FLinearColor(.3,.6,1),Layer+8);
        Box(Screen(M.Position+FVector2D(180,34))-FVector2D(6,6),FVector2D(12,12),FLinearColor(.3,.6,1),Layer+8);
    }
    Text(FVector2D(12,8),Connecting.IsEmpty()?TEXT("오른쪽 점 → 목적 동작 클릭  |  숫자: 프레임 수정"):TEXT("목적 동작을 클릭하세요. Esc: 연결 취소"),FLinearColor(.7,.77,.85),Layer+9);
    Text(FVector2D(12,28),TEXT("노드 드래그: 배치  |  휠: 확대/축소  |  오른쪽 드래그: 이동"),FLinearColor(.7,.77,.85),Layer+9);
    if(Model->Motions.IsEmpty()&&!bAssetDragOver)
        Text(FVector2D(24,90),TEXT("목록의 동작을 여기에 끌어 놓으세요. 여러 개도 한 번에 추가됩니다."),FLinearColor(.7,.77,.85),Layer+9);
    if(bAssetDragOver)
    {
        const FLinearColor Color=bCanDropAssets?FLinearColor(.25,.9,.6):FLinearColor(1,.5,.3);
        const FVector2D Size=G.GetLocalSize();
        FSlateDrawElement::MakeLines(Out,Layer+10,G.ToPaintGeometry(),{FVector2D(2,2),FVector2D(Size.X-2,2),Size-FVector2D(2,2),FVector2D(2,Size.Y-2),FVector2D(2,2)},ESlateDrawEffect::None,Color,false,3);
        Box(FVector2D(8,52),FVector2D(Size.X-16,28),FLinearColor(.04,.07,.10),Layer+10);
        Text(FVector2D(16,57),DropHint,Color,Layer+11);
    }
    return Layer+11;
}
FReply STransitionGraph::OnDragOver(const FGeometry&,const FDragDropEvent& Event)
{
    const auto Operation=Event.GetOperationAs<FAssetDragDropOp>();
    if(!Operation.IsValid())return FReply::Unhandled();
    int32 NewCount=0;bool bHasAnimation=false;
    for(const auto& Asset:Operation->GetAssets())
    {
        bHasAnimation|=Asset.IsInstanceOf<UAnimSequence>();
        if(IsNewMotionAsset(Asset,*Model))++NewCount;
    }
    bAssetDragOver=true;bCanDropAssets=!Model->IsBusy()&&NewCount>0;
    DropHint=Model->IsBusy()?TEXT("변환이 끝난 뒤 추가할 수 있습니다."):
        (NewCount>0?FString::Printf(TEXT("놓으면 동작 %d개 추가 · 자동 변환"),NewCount):
        (bHasAnimation?TEXT("이미 추가된 동작입니다."):TEXT("Animation Sequence를 끌어 놓으세요.")));
    Operation->SetToolTip(FText::FromString(DropHint),FAppStyle::GetBrush(bCanDropAssets?"Graph.ConnectorFeedback.OK":"Graph.ConnectorFeedback.Error"));
    Operation->SetCursorOverride(bCanDropAssets?TOptional<EMouseCursor::Type>():TOptional<EMouseCursor::Type>(EMouseCursor::SlashedCircle));
    Invalidate(EInvalidateWidgetReason::Paint);
    return FReply::Handled();
}
void STransitionGraph::OnDragLeave(const FDragDropEvent& Event)
{
    bAssetDragOver=false;bCanDropAssets=false;DropHint.Reset();
    if(const auto Operation=Event.GetOperationAs<FAssetDragDropOp>())
    {Operation->ResetToDefaultToolTip();Operation->SetCursorOverride(TOptional<EMouseCursor::Type>());}
    Invalidate(EInvalidateWidgetReason::Paint);
}
FReply STransitionGraph::OnDrop(const FGeometry& G,const FDragDropEvent& Event)
{
    const auto Operation=Event.GetOperationAs<FAssetDragDropOp>();
    OnDragLeave(Event);
    if(!Operation.IsValid())return FReply::Unhandled();
    if(Model->IsBusy()){Model->Message=TEXT("변환이 끝난 뒤 추가할 수 있습니다.");Changed();return FReply::Handled();}
    TArray<UAnimSequence*> Clips;
    bool bHasAnimation=false;
    for(const auto& Asset:Operation->GetAssets())
    {
        bHasAnimation|=Asset.IsInstanceOf<UAnimSequence>();
        if(IsNewMotionAsset(Asset,*Model))if(auto* Clip=Cast<UAnimSequence>(Asset.GetAsset()))Clips.AddUnique(Clip);
    }
    if(Clips.IsEmpty())
    {
        Model->Message=bHasAnimation?TEXT("추가할 새 동작이 없습니다. 이미 추가된 항목 또는 불러올 수 없는 항목입니다."):TEXT("Animation Sequence를 끌어 놓으세요.");
        Changed();return FReply::Handled();
    }
    const int32 First=Model->Motions.Num();
    Model->AddMotions(Clips); // Reuse the button's validation, preparation queue and persistence.
    const int32 Count=Model->Motions.Num()-First;
    if(Count>0)
    {
        const FVector2D Size=G.GetLocalSize();
        const int32 Columns=FMath::Min(FMath::CeilToInt(FMath::Sqrt(static_cast<float>(Count))),FMath::Max(1,FMath::FloorToInt((Size.X-24)/(210*Zoom))));
        const int32 Rows=(Count+Columns-1)/Columns;
        const FVector2D GroupSize=FVector2D(Columns*210-30,Rows*100-32)*Zoom;
        FVector2D Anchor=G.AbsoluteToLocal(Event.GetScreenSpacePosition())-FVector2D(90,34)*Zoom;
        Anchor.X=FMath::Clamp(Anchor.X,12.0,FMath::Max(12.0,Size.X-GroupSize.X-12));
        Anchor.Y=FMath::Clamp(Anchor.Y,84.0,FMath::Max(84.0,Size.Y-GroupSize.Y-12));
        // Persist graph coordinates so dropping also works after panning or zooming.
        for(int32 I=0;I<Count;++I)Model->Motions[First+I].Position=(Anchor-Pan)/Zoom+FVector2D((I%Columns)*210,(I/Columns)*100);
        Model->SelectedMotion=Model->Motions[First].Id;Model->SelectedConnection.Reset();
        Connecting.Reset();Dragging.Reset();bPanning=false;
        Model->Touch();
    }
    return FReply::Handled().SetUserFocus(SharedThis(this));
}
FReply STransitionGraph::OnMouseButtonDown(const FGeometry& G,const FPointerEvent& Event)
{
    Last=G.AbsoluteToLocal(Event.GetScreenSpacePosition());
    if(Event.GetEffectingButton()==EKeys::RightMouseButton||Event.GetEffectingButton()==EKeys::MiddleMouseButton){bPanning=true;return FReply::Handled().CaptureMouse(SharedThis(this));}
    if(Event.GetEffectingButton()!=EKeys::LeftMouseButton)return FReply::Unhandled();
    for(const auto& M:Model->Motions)
    {
        if(!Model->IsBusy()&&FVector2D::Distance(Last,Screen(M.Position+FVector2D(180,34)))<14){Connecting=M.Id;Changed();return FReply::Handled().SetUserFocus(SharedThis(this));}
        const auto P=Screen(M.Position);const auto Size=FVector2D(180,68)*Zoom;
        if(Last.X>=P.X-10&&Last.X<=P.X+Size.X&&Last.Y>=P.Y&&Last.Y<=P.Y+Size.Y)
        {
            if(!Connecting.IsEmpty()&&!Model->IsBusy()){const FString From=Connecting;Connecting.Reset();Model->Connect(From,M.Id);}
            else {Model->SelectedMotion=M.Id;Model->SelectedConnection.Reset();if(!Model->IsBusy())Dragging=M.Id;Changed();}
            return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this));
        }
    }
    for(const auto& E:Model->Connections)if(FVector2D::Distance(Last,Label(E))<32){Model->SelectedConnection=E.Id;Model->SelectedMotion.Reset();Changed();return FReply::Handled();}
    Connecting.Reset();Model->SelectedMotion.Reset();Model->SelectedConnection.Reset();Changed();return FReply::Handled();
}
FReply STransitionGraph::OnMouseMove(const FGeometry& G,const FPointerEvent& E)
{
    const auto P=G.AbsoluteToLocal(E.GetScreenSpacePosition());const auto Delta=P-Last;
    if(bPanning)Pan+=Delta;
    else if(!Dragging.IsEmpty()&&!Model->IsBusy())if(auto* M=Model->Motion(Dragging))M->Position+=Delta/Zoom;
    Last=P;Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();
}
FReply STransitionGraph::OnMouseButtonUp(const FGeometry&,const FPointerEvent&)
{if(!Dragging.IsEmpty())Model->Save();Dragging.Reset();bPanning=false;return FReply::Handled().ReleaseMouseCapture();}
FReply STransitionGraph::OnMouseWheel(const FGeometry& G,const FPointerEvent& E)
{const auto P=G.AbsoluteToLocal(E.GetScreenSpacePosition());const auto Before=(P-Pan)/Zoom;Zoom=FMath::Clamp(Zoom+E.GetWheelDelta()*.1f,.5f,1.5f);Pan=P-Before*Zoom;Invalidate(EInvalidateWidgetReason::Paint);return FReply::Handled();}
FReply STransitionGraph::OnKeyDown(const FGeometry&,const FKeyEvent& E)
{if(E.GetKey()==EKeys::Escape){Connecting.Reset();return FReply::Handled();}if(E.GetKey()==EKeys::Delete){Model->RemoveSelected();return FReply::Handled();}return FReply::Unhandled();}
}
