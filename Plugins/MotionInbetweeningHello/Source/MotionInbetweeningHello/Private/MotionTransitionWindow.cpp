#include "MotionTransitionEditor.h"
#include "MotionTransitionGraph.h"
#include "MotionTransitionViewport.h"
#include "MotionInbetweeningMappingSettings.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimBlueprint.h"
#include "AssetRegistry/AssetData.h"
#include "ContentBrowserModule.h"
#include "Dom/JsonObject.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "IContentBrowserSingleton.h"
#include "IDetailsView.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "PropertyEditorModule.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

namespace MotionInbetweening
{
namespace TransitionWindow
{
TSharedPtr<FTransitionEditorModel> Model;
TWeakPtr<SWindow> Window;
FText Text(const TCHAR* T){return FText::FromString(T);}
}
class STransitionEditor : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(STransitionEditor){} SLATE_ARGUMENT(TSharedPtr<FTransitionEditorModel>,Model) SLATE_END_ARGS()
    void Construct(const FArguments& Args)
    {
        Model=Args._Model;
        FAssetPickerConfig Picker;
        Picker.bAllowDragging=true;
        Picker.Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
        Picker.SelectionMode=ESelectionMode::Multi;Picker.InitialAssetViewType=EAssetViewType::List;
        Picker.OnShouldFilterAsset=FOnShouldFilterAsset::CreateLambda([](const FAssetData& A){return A.PackagePath.ToString().StartsWith(TEXT("/Game/MotionInbetweening/"));});
        auto AssetPicker=FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().CreateAssetPicker(Picker);
        FDetailsViewArgs DetailsArgs;DetailsArgs.bAllowSearch=true;DetailsArgs.bHideSelectionTip=true;DetailsArgs.NameAreaSettings=FDetailsViewArgs::HideNameArea;
        Details=FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor")).CreateDetailView(DetailsArgs);
        Details->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateLambda([](const FPropertyAndParent& P){const auto N=P.Property.GetFName();return N!=TEXT("MaxSamples")&&N!=TEXT("bOpenOutputFolder");}));
        Details->SetObject(GetMutableDefault<UMotionInbetweeningMappingSettings>());
        Details->OnFinishedChangingProperties().AddSP(this,&STransitionEditor::SettingsEdited);
        ChildSlot
        [
            SNew(SVerticalBox)
            +SVerticalBox::Slot().AutoHeight().Padding(10)
            [SNew(SHorizontalBox)
                +SHorizontalBox::Slot().AutoWidth().Padding(3)[Button(TEXT("새 전환 세트"),[this]{Model->NewSet();})]
                +SHorizontalBox::Slot().AutoWidth().Padding(3)[SNew(SComboButton).IsEnabled_Lambda([this]{return !Model->IsBusy();}).OnGetMenuContent(this,&STransitionEditor::SavedSets)
                    .ButtonContent()[SNew(STextBlock).Text(TransitionWindow::Text(TEXT("세트 열기")))]]
                +SHorizontalBox::Slot().FillWidth(1).Padding(8,3)[SNew(SEditableTextBox).Text_Lambda([this]{return FText::FromString(Model->Name);}).IsEnabled_Lambda([this]{return !Model->IsBusy();})
                    .OnTextCommitted_Lambda([this](const FText& T,ETextCommit::Type){if(!T.IsEmpty()){Model->Name=T.ToString();Model->Touch();}})]
                +SHorizontalBox::Slot().AutoWidth().Padding(3)[Button(TEXT("저장"),[this]{if(Model->Save())Model->Message=TEXT("전환 세트를 저장했습니다.");})]
                +SHorizontalBox::Slot().AutoWidth().Padding(3)[Button(TEXT("생성 + ABP"),[this]{Model->Generate();})]
                +SHorizontalBox::Slot().AutoWidth().Padding(3)[SNew(SButton).Text(TransitionWindow::Text(TEXT("취소"))).IsEnabled_Lambda([this]{return Model->IsBusy();}).OnClicked_Lambda([this]{Model->Cancel();return FReply::Handled();})]]
            +SVerticalBox::Slot().AutoHeight().Padding(12,0,12,8)
            [SNew(STextBlock).Text(TransitionWindow::Text(TEXT("Manny 반복 동작 · 30fps · 제자리 재생용 사본 생성 · 원본 보존   |   이동량·Anim Notify는 생성 ABP에 포함하지 않습니다."))).AutoWrapText(true)]
            +SVerticalBox::Slot().FillHeight(1)
            [SNew(SSplitter)
                +SSplitter::Slot().Value(.23f)
                [SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().Padding(10)[Heading(TEXT("1. 동작 선택"))]
                    +SVerticalBox::Slot().AutoHeight().Padding(10,0)[SNew(STextBlock).Text(TransitionWindow::Text(TEXT("Ctrl/Shift로 여러 개 선택 후 그래프에 끌어 놓으세요. 자동 변환됩니다."))).AutoWrapText(true)]
                    +SVerticalBox::Slot().FillHeight(.55f).Padding(8)[SNew(SBox).IsEnabled_Lambda([this]{return !Model->IsBusy();})[AssetPicker]]
                    +SVerticalBox::Slot().AutoHeight()
                    [SNew(SBox).Padding(8).Visibility_Lambda([this]
                        {
                            // Loading a set or editing settings recomputes status, but retains the conversion error.
                            const bool Failed=Model->Motions.ContainsByPredicate([](const FTransitionMotion& M)
                                {return M.Status==TEXT("Failed")||(M.Status==TEXT("Needs preparation")&&!M.Error.IsEmpty());});
                            return !Model->IsBusy()&&Failed?EVisibility::Visible:EVisibility::Collapsed;
                        })
                        [Button(TEXT("변환 실패 재시도"),[this]{Model->Prepare();})]]
                    +SVerticalBox::Slot().FillHeight(.45f).Padding(8)[SNew(SScrollBox)+SScrollBox::Slot()[SAssignNew(MotionList,SVerticalBox)]]]
                +SSplitter::Slot().Value(.53f)
                [SNew(SVerticalBox)
                    +SVerticalBox::Slot().AutoHeight().Padding(10)
                    [SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Heading(TEXT("2. 연결과 프레임"))]
                        +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("모든 방향 연결"),[this]{Model->ConnectAll();})]]
                    +SVerticalBox::Slot().FillHeight(.58f).Padding(8)[SNew(SBorder).Clipping(EWidgetClipping::ClipToBounds)[SAssignNew(Graph,STransitionGraph).Model(Model)]]
                    +SVerticalBox::Slot().AutoHeight().Padding(10,4)
                    [SNew(SHorizontalBox)+SHorizontalBox::Slot().FillWidth(1)[Heading(TEXT("3. 재생 확인"))]
                        +SHorizontalBox::Slot().AutoWidth()[Button(TEXT("ABP 미리보기"),[this]{ShowBlueprint();})]]
                    +SVerticalBox::Slot().FillHeight(.42f).Padding(8)[SAssignNew(Viewport,STransitionViewport)]
                    +SVerticalBox::Slot().AutoHeight().Padding(8,2)[SAssignNew(Requests,SWrapBox).UseAllottedSize(true)]
                    +SVerticalBox::Slot().AutoHeight().Padding(8,2)[SNew(STextBlock).Text_Lambda([this]{return FText::FromString(Viewport->Status());}).AutoWrapText(true)]]
                +SSplitter::Slot().Value(.24f)
                [SNew(SScrollBox)
                    +SScrollBox::Slot().Padding(10)[Heading(TEXT("선택 항목"))]
                    +SScrollBox::Slot().Padding(10)[SAssignNew(Inspector,SVerticalBox)]
                    +SScrollBox::Slot().Padding(10)
                    [SNew(SExpandableArea).InitiallyCollapsed(false)
                        .HeaderContent()[Heading(TEXT("환경 설정"))]
                        .BodyContent()[SNew(SBox).IsEnabled_Lambda([this]{return !Model->IsBusy();})[Details.ToSharedRef()]]]
                    +SScrollBox::Slot().Padding(10)[SNew(STextBlock).Text_Lambda([this]{FString Reason;const int32 Max=Model->MaximumFrames(Reason);return FText::FromString(Reason.IsEmpty()?FString::Printf(TEXT("현재 설정 범위: 5–%d 생성 프레임\n실행 시 모델 파일·해시를 검증합니다."),Max):Reason);}).AutoWrapText(true)]
                    +SScrollBox::Slot().Padding(10)[Button(TEXT("생성 ABP 에셋 열기"),[this]{if(auto* BP=LoadObject<UAnimBlueprint>(nullptr,*Model->Blueprint))GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(BP);})]
                    +SScrollBox::Slot().Padding(10)[Button(TEXT("전환 세트 저장 폴더"),[]{FPlatformProcess::ExploreFolder(*FTransitionEditorModel::SetsDirectory());})]]]
            +SVerticalBox::Slot().AutoHeight().Padding(12)[SNew(STextBlock).Text_Lambda([this]{return FText::FromString(Model->Message+(!Model->Blueprint.IsEmpty()&&Model->BlueprintKey.IsEmpty()?TEXT("  |  ABP 재생성 필요: 현재 미리보기는 이전 결과입니다."):TEXT("")));}).AutoWrapText(true)]
        ];
        Model->OnChanged=FSimpleDelegate::CreateSP(this,&STransitionEditor::Refresh);
        Model->OnBlueprintReady=FSimpleDelegate::CreateSP(this,&STransitionEditor::ShowBlueprint);
        Refresh();if(!Model->Blueprint.IsEmpty())ShowBlueprint();
    }
private:
    TSharedPtr<FTransitionEditorModel> Model;
    TSharedPtr<SVerticalBox> Inspector,MotionList;
    TSharedPtr<SWrapBox> Requests;
    TSharedPtr<STransitionGraph> Graph;
    TSharedPtr<STransitionViewport> Viewport;
    TSharedPtr<IDetailsView> Details;
    FString DisplayedSet;
    TSharedRef<SWidget> Heading(const TCHAR* T){return SNew(STextBlock).Text(TransitionWindow::Text(T)).Font(FCoreStyle::GetDefaultFontStyle("Bold",12));}
    TSharedRef<SWidget> Button(const TCHAR* Label,TFunction<void()> Action)
    {return SNew(SButton).Text(TransitionWindow::Text(Label)).IsEnabled_Lambda([this]{return !Model->IsBusy();}).OnClicked_Lambda([Action]{Action();return FReply::Handled();});}
    void SettingsEdited(const FPropertyChangedEvent&)
    {GetMutableDefault<UMotionInbetweeningMappingSettings>()->SaveConfig();Model->SettingsChanged();}
    void OpenSet()
    {
        auto* Desktop=FDesktopPlatformModule::Get();TArray<FString> Files;
        if(Desktop&&Desktop->OpenFileDialog(FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),TEXT("전환 세트 열기"),FTransitionEditorModel::SetsDirectory(),TEXT(""),TEXT("Transition set (*.json)|*.json"),0,Files)&&Files.Num())
        {Model->Load(Files[0]);Refresh();ShowBlueprint();}
    }
    TSharedRef<SWidget> SavedSets()
    {
        auto List=SNew(SVerticalBox);TArray<FString> Files;
        IFileManager::Get().FindFiles(Files,*(FTransitionEditorModel::SetsDirectory()/TEXT("*.json")),true,false);Files.Sort();
        for(const FString& File:Files)
        {
            const FString Path=FTransitionEditorModel::SetsDirectory()/File;FString Contents,Label;TSharedPtr<FJsonObject> Json;
            if(!FFileHelper::LoadFileToString(Contents,*Path)||!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Contents),Json)||!Json||!Json->TryGetStringField(TEXT("name"),Label))continue;
            List->AddSlot().AutoHeight().Padding(3)[SNew(SButton).Text(FText::FromString(Label+TEXT("  ·  ")+File.Left(8))).OnClicked_Lambda([this,Path]{FSlateApplication::Get().DismissAllMenus();if(Model->Load(Path)){Refresh();ShowBlueprint();}return FReply::Handled();})];
        }
        List->AddSlot().AutoHeight().Padding(3)[Button(TEXT("JSON 파일에서 열기…"),[this]{FSlateApplication::Get().DismissAllMenus();OpenSet();})];
        return SNew(SBox).WidthOverride(400).MaxDesiredHeight(380)[SNew(SScrollBox)+SScrollBox::Slot()[List]];
    }
    void ShowBlueprint()
    {if(!Model->Blueprint.IsEmpty())if(auto* BP=LoadObject<UAnimBlueprint>(nullptr,*Model->Blueprint))Viewport->SetBlueprint(BP);}
    void Refresh()
    {
        if(!Inspector||!MotionList||!Requests)return;
        if(DisplayedSet!=Model->Id){Viewport->Clear();DisplayedSet=Model->Id;}
        MotionList->ClearChildren();Requests->ClearChildren();Inspector->ClearChildren();
        for(const auto& M:Model->Motions)
        {
            const FString Key=M.Id,MotionName=M.Name;
            MotionList->AddSlot().AutoHeight().Padding(0,3)
            [SNew(SButton).Text(FText::FromString((M.Id==Model->DefaultMotion?TEXT("* "):TEXT(""))+M.Name+TEXT(" · ")+M.Status))
                .OnClicked_Lambda([this,Key]{Model->SelectedMotion=Key;Model->SelectedConnection.Reset();Refresh();return FReply::Handled();})];
            Requests->AddSlot().Padding(3)[SNew(SButton).Text(FText::FromString(MotionName)).OnClicked_Lambda([this,MotionName]{Viewport->Request(FName(*MotionName));return FReply::Handled();})];
        }
        if(auto* E=Model->Connection(Model->SelectedConnection))
        {
            const FString Key=E->Id;
            Inspector->AddSlot().AutoHeight().Padding(0,4)[SNew(STextBlock).Text(FText::FromString(Model->Motion(E->From)->Name+TEXT(" → ")+Model->Motion(E->To)->Name)).AutoWrapText(true)];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[Heading(TEXT("생성 프레임 수"))];
            FString Reason;const int32 Max=Model->MaximumFrames(Reason);
            Inspector->AddSlot().AutoHeight().Padding(0,4)[SNew(SSpinBox<int32>).MinValue(5).MaxValue(Max).Value(E->Frames).IsEnabled_Lambda([this]{return !Model->IsBusy();})
                .OnValueCommitted_Lambda([this,Key](int32 N,ETextCommit::Type){Model->SetFrames(Key,N);})];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[SNew(STextBlock).Text(FText::FromString(FString::Printf(TEXT("출발→도착 %.3f초 · %s"),(E->Frames+1)/30.0,*E->Status))).AutoWrapText(true)];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[SNew(STextBlock).Text(FText::FromString(E->Error)).AutoWrapText(true)];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[Button(TEXT("이 연결 재생"),[this,Key]{if(auto* C=Model->Connection(Key))if(auto* Clip=LoadObject<UAnimSequence>(nullptr,*C->Preview))Viewport->SetClip(Clip);})];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[Button(TEXT("실행 로그 폴더"),[this,Key]{if(auto* C=Model->Connection(Key))if(!C->Job.IsEmpty())FPlatformProcess::ExploreFolder(*C->Job);})];
        }
        else if(auto* M=Model->Motion(Model->SelectedMotion))
        {
            const FString Key=M->Id;
            Inspector->AddSlot().AutoHeight().Padding(0,4)[SNew(SEditableTextBox).Text(FText::FromString(M->Name)).IsEnabled_Lambda([this]{return !Model->IsBusy();})
                .OnTextCommitted_Lambda([this,Key](const FText& T,ETextCommit::Type){Model->RenameMotion(Key,T.ToString());})];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[SNew(STextBlock).Text(FText::FromString(M->Status+TEXT("\n")+M->Error)).AutoWrapText(true)];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[SNew(STextBlock).Text(FText::FromString(M->Source)).AutoWrapText(true)];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[Button(TEXT("기본 동작으로 지정"),[this,Key]{Model->SetDefault(Key);})];
            Inspector->AddSlot().AutoHeight().Padding(0,4)[Button(TEXT("변환된 동작 재생"),[this,Key]{if(auto* N=Model->Motion(Key))if(auto* Clip=LoadObject<UAnimSequence>(nullptr,*N->Prepared))Viewport->SetClip(Clip);})];
        }
        else Inspector->AddSlot().AutoHeight()[SNew(STextBlock).Text(TransitionWindow::Text(TEXT("노드의 오른쪽 점을 누르고 목적 동작을 클릭해 연결하세요. 선 위의 프레임 숫자를 누르면 길이를 수정할 수 있습니다."))).AutoWrapText(true)];
        if(!Model->SelectedMotion.IsEmpty()||!Model->SelectedConnection.IsEmpty())
            Inspector->AddSlot().AutoHeight().Padding(0,10)[Button(TEXT("선택 항목을 세트에서 제외"),[this]{Model->RemoveSelected();})];
        Graph->Invalidate(EInvalidateWidgetReason::Paint);
    }
};
void OpenTransitionEditor()
{
    if(auto Existing=TransitionWindow::Window.Pin()){Existing->BringToFront();return;}
    if(!TransitionWindow::Model)
    {
        TransitionWindow::Model=MakeShared<FTransitionEditorModel>();FString Last;
        if(FFileHelper::LoadFileToString(Last,*(FTransitionEditorModel::SetsDirectory()/TEXT("last.txt"))))TransitionWindow::Model->Load(Last);
    }
    auto Window=SNew(SWindow).Title(FText::FromString(TEXT("Motion Transition Editor"))).ClientSize(FVector2D(1560,900)).SupportsMaximize(true).SupportsMinimize(true)
        [SNew(STransitionEditor).Model(TransitionWindow::Model)];
    TransitionWindow::Window=Window;FSlateApplication::Get().AddWindow(Window);
}
void ShutdownTransitionEditor()
{
    if(TransitionWindow::Model)TransitionWindow::Model->Cancel();
    if(auto Window=TransitionWindow::Window.Pin())Window->RequestDestroyWindow();
    TransitionWindow::Window.Reset();TransitionWindow::Model.Reset();
}
}
