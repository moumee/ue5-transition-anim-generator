#include "MotionTransitionEditor.h"
#include "MotionInbetweeningOutputMapping.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Interfaces/IPluginManager.h"

namespace MotionInbetweening
{
namespace TransitionModel
{
FString NewId() { return FGuid::NewGuid().ToString(EGuidFormats::Digits); }
FString Digest(const FString& Value) { return FMD5::HashAnsiString(*Value); }
bool Read(const FString& Path, TSharedPtr<FJsonObject>& Object)
{
    FString Text; return FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object;
}
FString Str(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
{ FString Value; Object->TryGetStringField(Key, Value); return Value; }
bool AssetExists(const FString& ObjectPath)
{ return !ObjectPath.IsEmpty() && LoadObject<UAnimSequence>(nullptr, *ObjectPath) != nullptr; }
FString PreparationKey(const FString& Stamp, const FMappingOptions& Options)
{
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("MotionInbetweeningHello"));
    FString Key = TEXT("inplace-v1-loop3-30hz|") + Stamp + Options.PythonExecutable + TransitionFileHash(Options.StatisticsFile)
        + FString::Printf(TEXT("|%g|%g"), Options.MaxRootStepCm, Options.MaxJointStepDegrees);
    for (const TCHAR* File : {TEXT("export_manny_input.py"), TEXT("prepare_manny_input.py")})
        Key += TransitionFileHash(Plugin->GetBaseDir() / TEXT("Scripts") / File);
    TArray<FString> Sources; IFileManager::Get().FindFilesRecursive(Sources, *(FPaths::ProjectDir() / TEXT("Tools/MannyLafanMapping")), TEXT("*.py"), true, false);
    Sources.Sort(); for (const FString& File : Sources) Key += TransitionFileHash(File);
    return Digest(Key);
}
}
FTransitionEditorModel::FTransitionEditorModel() { Id = TransitionModel::NewId(); }
FTransitionEditorModel::~FTransitionEditorModel() { Cancel(); }
FTransitionMotion* FTransitionEditorModel::Motion(const FString& Key) { return Motions.FindByPredicate([&](const auto& M) { return M.Id == Key; }); }
const FTransitionMotion* FTransitionEditorModel::Motion(const FString& Key) const { return Motions.FindByPredicate([&](const auto& M) { return M.Id == Key; }); }
FTransitionConnection* FTransitionEditorModel::Connection(const FString& Key) { return Connections.FindByPredicate([&](const auto& E) { return E.Id == Key; }); }
FString FTransitionEditorModel::SetsDirectory() { return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/TransitionSets")); }
FString FTransitionEditorModel::SetPath() const { return SetsDirectory() / (Id + TEXT(".json")); }
FString FTransitionEditorModel::OutputFolder() const { return TEXT("/Game/MotionInbetweening/Sets/Set_") + Id; }
void FTransitionEditorModel::Touch() { ++Revision; Save(); OnChanged.ExecuteIfBound(); }
bool FTransitionEditorModel::Save()
{
    auto Root = MakeShared<FJsonObject>(); Root->SetNumberField(TEXT("version"), 1);
    Root->SetStringField(TEXT("id"), Id); Root->SetStringField(TEXT("name"), Name);
    Root->SetStringField(TEXT("default"), DefaultMotion); Root->SetStringField(TEXT("blueprint"), Blueprint); Root->SetStringField(TEXT("blueprint_key"), BlueprintKey);
    TArray<TSharedPtr<FJsonValue>> Nodes, Edges;
    for (const auto& M : Motions)
    {
        auto O = MakeShared<FJsonObject>();
#define FIELD(Key, Value) O->SetStringField(TEXT(Key), Value)
        FIELD("id",M.Id); FIELD("name",M.Name); FIELD("source",M.Source); FIELD("source_stamp",M.SourceStamp);
        FIELD("prepared",M.Prepared); FIELD("input",M.Input); FIELD("input_hash",M.InputHash); FIELD("preparation_key",M.PreparationKey);
        FIELD("status",M.Status); FIELD("error",M.Error);
        O->SetNumberField(TEXT("x"),M.Position.X); O->SetNumberField(TEXT("y"),M.Position.Y); O->SetNumberField(TEXT("samples"),M.Samples);
        Nodes.Add(MakeShared<FJsonValueObject>(O));
    }
    for (const auto& E : Connections)
    {
        auto O = MakeShared<FJsonObject>();
        FIELD("id",E.Id); FIELD("from",E.From); FIELD("to",E.To); FIELD("full_result",E.FullResult); FIELD("playback",E.Playback);
        FIELD("preview",E.Preview); FIELD("generation_key",E.GenerationKey); FIELD("status",E.Status); FIELD("error",E.Error); FIELD("job",E.Job);
        O->SetNumberField(TEXT("frames"),E.Frames); Edges.Add(MakeShared<FJsonValueObject>(O));
    }
#undef FIELD
    Root->SetArrayField(TEXT("motions"),Nodes); Root->SetArrayField(TEXT("connections"),Edges);
    FString Text;
    IFileManager::Get().MakeDirectory(*SetsDirectory(),true);
    if (!FJsonSerializer::Serialize(Root,TJsonWriterFactory<>::Create(&Text))
        || !FFileHelper::SaveStringToFile(Text,*(SetPath()+TEXT(".tmp")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
        || !IFileManager::Get().Move(*SetPath(),*(SetPath()+TEXT(".tmp")),true,true))
    {Message = TEXT("Could not save the transition set. Check disk space and permissions.");return false;}
    FFileHelper::SaveStringToFile(SetPath(),*(SetsDirectory()/TEXT("last.txt")),FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    return true;
}
bool FTransitionEditorModel::Load(const FString& Path)
{
    if (bBusy) return false;
    TSharedPtr<FJsonObject> Root; double Version=0;
    if (!TransitionModel::Read(Path,Root) || !Root->TryGetNumberField(TEXT("version"),Version) || Version!=1)
    { Message=TEXT("Unsupported or invalid transition set."); return false; }
    const TArray<TSharedPtr<FJsonValue>> *Nodes=nullptr,*Edges=nullptr;
    FGuid ParsedId; const FString NewId=TransitionModel::Str(Root,TEXT("id"));
    if (!FGuid::ParseExact(NewId,EGuidFormats::Digits,ParsedId) || !Root->TryGetArrayField(TEXT("motions"),Nodes) || !Root->TryGetArrayField(TEXT("connections"),Edges))
    { Message=TEXT("Transition set is missing its graph."); return false; }
    TArray<FTransitionMotion> NewMotions; TArray<FTransitionConnection> NewEdges; TSet<FString> Ids; TSet<FName> Names;
    for (const auto& Value:*Nodes)
    {
        const TSharedPtr<FJsonObject>* Object=nullptr; if (!Value->TryGetObject(Object)) return false; const auto O=*Object;
        FTransitionMotion M;
#define READ(Key, Field) Field=TransitionModel::Str(O,TEXT(Key))
        READ("id",M.Id); READ("name",M.Name); READ("source",M.Source); READ("source_stamp",M.SourceStamp);
        READ("prepared",M.Prepared); READ("input",M.Input); READ("input_hash",M.InputHash); READ("preparation_key",M.PreparationKey);
        READ("status",M.Status); READ("error",M.Error);
        double X=0,Y=0,Samples=0; O->TryGetNumberField(TEXT("x"),X); O->TryGetNumberField(TEXT("y"),Y); O->TryGetNumberField(TEXT("samples"),Samples);
        if (!FGuid::ParseExact(M.Id,EGuidFormats::Digits,ParsedId) || Ids.Contains(M.Id) || M.Name.IsEmpty() || Names.Contains(FName(*M.Name))
            || !FMath::IsFinite(X) || !FMath::IsFinite(Y) || Samples<0 || Samples>300) { Message=TEXT("Invalid or duplicate motion in set."); return false; }
        M.Position=FVector2D(X,Y); M.Samples=Samples; Ids.Add(M.Id); Names.Add(FName(*M.Name)); NewMotions.Add(M);
    }
    TSet<FString> Pairs,EdgeIds;
    for (const auto& Value:*Edges)
    {
        const TSharedPtr<FJsonObject>* Object=nullptr; if (!Value->TryGetObject(Object)) return false; const auto O=*Object;
        FTransitionConnection E;
        READ("id",E.Id); READ("from",E.From); READ("to",E.To); READ("full_result",E.FullResult); READ("playback",E.Playback);
        READ("preview",E.Preview); READ("generation_key",E.GenerationKey); READ("status",E.Status); READ("error",E.Error); READ("job",E.Job);
        double Frames=0; O->TryGetNumberField(TEXT("frames"),Frames);
        if (!FGuid::ParseExact(E.Id,EGuidFormats::Digits,ParsedId) || EdgeIds.Contains(E.Id) || !Ids.Contains(E.From) || !Ids.Contains(E.To)
            || E.From==E.To || Pairs.Contains(E.From+E.To) || Frames<5 || Frames>60 || FMath::FloorToDouble(Frames)!=Frames)
        { Message=TEXT("Invalid connection in set."); return false; }
        E.Frames=Frames; EdgeIds.Add(E.Id); Pairs.Add(E.From+E.To); NewEdges.Add(E);
    }
#undef READ
    Id=NewId; Name=TransitionModel::Str(Root,TEXT("name")); DefaultMotion=TransitionModel::Str(Root,TEXT("default"));
    Motions=MoveTemp(NewMotions); Connections=MoveTemp(NewEdges); Blueprint=TransitionModel::Str(Root,TEXT("blueprint")); BlueprintKey=TransitionModel::Str(Root,TEXT("blueprint_key"));
    if (!Motion(DefaultMotion)) DefaultMotion=Motions.Num()?Motions[0].Id:FString();
    for (auto& M:Motions) M.Status=IsPrepared(M)?TEXT("Ready"):TEXT("Needs preparation");
    for (auto& E:Connections) E.Status=IsCurrent(E)?TEXT("Ready"):TEXT("Needs generation");
    if(Motions.ContainsByPredicate([](const auto& M){return M.Status!=TEXT("Ready");})||Connections.ContainsByPredicate([](const auto& E){return E.Status!=TEXT("Ready");}))BlueprintKey.Reset();
    SelectedMotion.Reset(); SelectedConnection.Reset(); Message=TEXT("Set loaded. Original assets are preserved."); Touch(); return true;
}
void FTransitionEditorModel::NewSet()
{
    if (bBusy||!Save()) return; Id=TransitionModel::NewId(); Name=TEXT("Locomotion"); DefaultMotion.Reset(); Blueprint.Reset(); BlueprintKey.Reset();
    Motions.Reset(); Connections.Reset(); SelectedMotion.Reset(); SelectedConnection.Reset(); Message=TEXT("Select animations and add them to prepare automatically."); Touch();
}
bool FTransitionEditorModel::AddMotions(const TArray<UAnimSequence*>& Assets)
{
    if (bBusy) return false;
    for (auto* Asset:Assets)
    {
        if (!Asset || Motions.ContainsByPredicate([&](const auto& M){ return M.Source==Asset->GetPathName(); })) continue;
        FTransitionMotion M; M.Id=TransitionModel::NewId(); M.Source=Asset->GetPathName(); M.Name=Asset->GetName();
        FString Base=M.Name; int32 Suffix=2;
        while (Motions.ContainsByPredicate([&](const auto& Other){ return FName(*Other.Name)==FName(*M.Name); })) M.Name=Base+FString::FromInt(Suffix++);
        const int32 I=Motions.Num(); M.Position=FVector2D(I%3==2?210:50+(I%3)*330,100+(I/3)*340+(I%3==2?170:0)); M.Status=TEXT("Queued"); Motions.Add(M);
        if (DefaultMotion.IsEmpty()) DefaultMotion=M.Id;
    }
    Touch(); return Prepare();
}
bool FTransitionEditorModel::Connect(const FString& From,const FString& To)
{
    if (bBusy || From==To || !Motion(From) || !Motion(To) || Connections.ContainsByPredicate([&](const auto& E){return E.From==From&&E.To==To;})) return false;
    FTransitionConnection E; E.Id=TransitionModel::NewId(); E.From=From; E.To=To; E.Frames=FMath::Clamp(GetInferenceOptions().TransitionFrames,5,60); E.Status=TEXT("Needs generation");
    Connections.Add(E); SelectedConnection=E.Id; SelectedMotion.Reset(); BlueprintKey.Reset(); Touch(); return true;
}
void FTransitionEditorModel::ConnectAll() { if (!bBusy) for (const auto& A:Motions) for (const auto& B:Motions) Connect(A.Id,B.Id); }
void FTransitionEditorModel::RemoveSelected()
{
    if (bBusy) return;
    if (!SelectedConnection.IsEmpty()) Connections.RemoveAll([&](const auto& E){return E.Id==SelectedConnection;});
    else if (!SelectedMotion.IsEmpty())
    {
        Connections.RemoveAll([&](const auto& E){return E.From==SelectedMotion||E.To==SelectedMotion;});
        Motions.RemoveAll([&](const auto& M){return M.Id==SelectedMotion;});
        if (DefaultMotion==SelectedMotion) DefaultMotion=Motions.Num()?Motions[0].Id:FString();
    }
    SelectedMotion.Reset(); SelectedConnection.Reset(); BlueprintKey.Reset(); Touch();
}
void FTransitionEditorModel::SetFrames(const FString& EdgeId,int32 Frames)
{
    if (bBusy) return; if (auto* E=Connection(EdgeId)) { if(E->Frames==Frames)return; E->Frames=Frames; E->Status=TEXT("Needs generation"); E->Error.Reset(); BlueprintKey.Reset(); Touch(); }
}
bool FTransitionEditorModel::RenameMotion(const FString& MotionId,const FString& NewName)
{
    const FString Trim=NewName.TrimStartAndEnd(); if (bBusy || Trim.IsEmpty() || Trim.Len()>64) return false;
    for (const auto& M:Motions) if (M.Id!=MotionId&&FName(*M.Name)==FName(*Trim)) {Message=TEXT("Motion names must be unique."); return false;}
    if (auto* M=Motion(MotionId)) {if(M->Name!=Trim){M->Name=Trim; BlueprintKey.Reset(); Touch();} return true;} return false;
}
void FTransitionEditorModel::SetDefault(const FString& MotionId) {if (!bBusy&&Motion(MotionId)&&DefaultMotion!=MotionId){DefaultMotion=MotionId; BlueprintKey.Reset(); Touch();}}
FString FTransitionEditorModel::SettingsKey() const
{
    auto O=GetInferenceOptions(); FString Key=TEXT("transition-model-v1|")+O.MappingPythonExecutable+O.InferencePythonExecutable+O.ModelConfigFile
        +TransitionFileHash(O.ModelConfigFile)+TransitionFileHash(O.InferenceScript)+TransitionFileHash(O.StatisticsFile);
    // Invalidate ordinary checkpoint replacements without hashing large weights on UI actions.
    // The inference backend verifies its required checkpoint hashes before execution.
    TSharedPtr<FJsonObject> Config;
    if(TransitionModel::Read(O.ModelConfigFile,Config))
    {
        TArray<TSharedPtr<FJsonObject>> Models{Config};const TSharedPtr<FJsonObject>* Long=nullptr;
        if(Config->TryGetObjectField(TEXT("long_transition_model"),Long))Models.Add(*Long);
        for(const auto& C:Models)for(const TCHAR* Field:{TEXT("context_checkpoint"),TEXT("detail_checkpoint"),TEXT("context_statistics"),TEXT("detail_statistics")})
        {
            FString File=TransitionModel::Str(C,Field);if(File.IsEmpty())continue;
            if(FPaths::IsRelative(File))File=FPaths::ConvertRelativePathToFull(FPaths::GetPath(O.ModelConfigFile),File);
            Key+=File+FString::Printf(TEXT("|%lld|%lld"),IFileManager::Get().FileSize(*File),IFileManager::Get().GetTimeStamp(*File).GetTicks());
            if(FString(Field).EndsWith(TEXT("statistics")))Key+=TransitionFileHash(File);
        }
    }
    TArray<FString> Sources;
    IFileManager::Get().FindFilesRecursive(Sources,*(FPaths::ProjectDir()/TEXT("Tools/MotionInference")),TEXT("*.py"),true,false);
    Sources.Sort(); for (const FString& File:Sources) Key+=TransitionFileHash(File);
    const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("MotionInbetweeningHello"));
    for (const TCHAR* F:{TEXT("run_manny_inference.py"),TEXT("prepare_manny_output.py")}) Key+=TransitionFileHash(Plugin->GetBaseDir()/TEXT("Scripts")/F);
    return TransitionModel::Digest(Key);
}
FString FTransitionEditorModel::EdgeKey(const FTransitionConnection& E) const
{
    const auto* A=Motion(E.From); const auto* B=Motion(E.To); if (!A||!B) return FString();
    return TransitionModel::Digest(A->PreparationKey+A->InputHash+B->PreparationKey+B->InputHash+FString::FromInt(E.Frames)+SettingsKey());
}
bool FTransitionEditorModel::IsPrepared(const FTransitionMotion& M) const
{
    const FString Stamp=TransitionSourceStamp(LoadObject<UAnimSequence>(nullptr,*M.Source));
    return !Stamp.IsEmpty()&&Stamp==M.SourceStamp&&M.PreparationKey==TransitionModel::PreparationKey(Stamp,GetMappingOptions())
        &&!M.InputHash.IsEmpty()&&TransitionFileHash(M.Input)==M.InputHash&&TransitionModel::AssetExists(M.Prepared);
}
bool FTransitionEditorModel::IsCurrent(const FTransitionConnection& E) const
{
    const auto* A=Motion(E.From); const auto* B=Motion(E.To);
    return A&&B&&A->Status==TEXT("Ready")&&B->Status==TEXT("Ready")&&!E.GenerationKey.IsEmpty()&&E.GenerationKey==EdgeKey(E)
        &&TransitionModel::AssetExists(E.Playback)&&TransitionModel::AssetExists(E.Preview);
}
int32 FTransitionEditorModel::MaximumFrames(FString& Reason) const
{
    Reason.Reset(); const FString Path=GetInferenceOptions().ModelConfigFile; TSharedPtr<FJsonObject> Config;
    if (!TransitionModel::Read(Path,Config)) {Reason=TEXT("Choose a valid model config in Environment settings."); return 45;}
    const TSharedPtr<FJsonObject>* Long=nullptr;
    if (Config->TryGetObjectField(TEXT("long_transition_model"),Long))
    {
        double Min=0; if (!(*Long)->TryGetNumberField(TEXT("min_transition"),Min)||Min<6||Min>60||FMath::FloorToDouble(Min)!=Min) {Reason=TEXT("Invalid long model min_transition."); return 45;}
        TArray<FString> Keys{TEXT("context_checkpoint")}; if(TransitionModel::Str(Config,TEXT("pipeline"))!=TEXT("context"))Keys.Add(TEXT("detail_checkpoint"));
        for (const FString& Key:Keys)
        {
            FString File=TransitionModel::Str(*Long,*Key); if (FPaths::IsRelative(File)) File=FPaths::ConvertRelativePathToFull(FPaths::GetPath(Path),File);
            if (!FPaths::FileExists(File)) {Reason=TEXT("Long model checkpoint is missing; fix the selected config."); return 45;}
        }
        return 60;
    }
    if (Config->HasField(TEXT("long_transition_model"))&&Config->TryGetField(TEXT("long_transition_model"))->Type!=EJson::Null)
        Reason=TEXT("long_transition_model must be an object or null.");
    return 45;
}
void FTransitionEditorModel::SettingsChanged()
{
    if (bBusy) return; for (auto& M:Motions) M.Status=IsPrepared(M)?TEXT("Ready"):TEXT("Needs preparation");
    for (auto& E:Connections) E.Status=IsCurrent(E)?TEXT("Ready"):TEXT("Needs generation"); BlueprintKey.Reset(); Touch();
}
bool FTransitionEditorModel::Prepare() { return Start(false); }
bool FTransitionEditorModel::Generate() { return Start(true); }
bool FTransitionEditorModel::Start(bool GenerateEdges)
{
    if (bBusy||IsMannyInputMappingRunning()||IsMannyInferenceRunning()||IsMannyOutputMappingRunning()) {Message=TEXT("Another conversion is running. Wait or cancel it."); return false;}
    if (Motions.IsEmpty()) {Message=TEXT("Add at least one animation."); return false;}
    FString Reason; const int32 Max=MaximumFrames(Reason);
    if (GenerateEdges&&!Reason.IsEmpty()) {Message=Reason; return false;}
    if (GenerateEdges&&Connections.IsEmpty()) {Message=TEXT("Connect motions before generating."); return false;}
    MappingOptions=GetMappingOptions(); MappingOptions.bOpenOutputFolder=false;
    InferenceOptions=GetInferenceOptions(); InferenceOptions.bOpenAsset=false;
    PendingMotions.Reset(); PendingEdges.Reset();
    for (auto& M:Motions)
    {
        if (!IsPrepared(M)) {M.Status=TEXT("Queued"); M.Error.Reset(); PendingMotions.Add(M.Id);}
        else M.Status=TEXT("Ready");
    }
    for (auto& E:Connections)
    {
        if (E.Frames<5||E.Frames>Max) {E.Status=TEXT("Failed"); E.Error=FString::Printf(TEXT("This config supports 5..%d generated frames."),Max);}
        else if (!IsCurrent(E)) {E.Status=TEXT("Needs generation"); if(GenerateEdges)PendingEdges.Add(E.Id);}
        else E.Status=TEXT("Ready");
    }
    if(PendingMotions.Num()||PendingEdges.Num())BlueprintKey.Reset();
    bBusy=true; bGenerate=GenerateEdges; Work=EWork::Idle; Message=TEXT("Preparing selected motions...");
    Ticker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this,&FTransitionEditorModel::Tick),0.1f); Touch(); return true;
}
bool FTransitionEditorModel::Tick(float)
{
    if (!bBusy) return false;
    if (Work==EWork::Mapping)
    {
        if (IsMannyInputMappingRunning()) return true;
        auto* M=Motion(ActiveId); const auto R=GetLastMappingResult();
        if (M)
        {
            if (R.bSuccess&&R.OutputDirectory==ActiveDirectory&&TransitionSourceStamp(LoadObject<UAnimSequence>(nullptr,*M->Source))==M->SourceStamp)
            {M->Input=R.OutputDirectory/TEXT("model_input.json"); M->InputHash=TransitionFileHash(M->Input); M->PreparationKey=ActiveKey; M->Status=TEXT("Ready"); M->Error.Reset();}
            else {M->Status=TEXT("Failed"); M->Error=R.Error.IsEmpty()?TEXT("Source changed or conversion receipt did not match."):R.Error;}
        }
        Work=EWork::Idle; Touch();
    }
    if (Work==EWork::Inference)
    {
        if (IsMannyInferenceRunning()) return true;
        auto* E=Connection(ActiveId); const auto R=GetLastInferenceResult();
        if (E)
        {
            E->Job=R.OutputDirectory;
            auto* A=Motion(E->From); auto* B=Motion(E->To); FString Error;
            auto* Full=R.bSuccess?LoadObject<UAnimSequence>(nullptr,*R.AssetPath):nullptr;
            auto* Source=A?LoadObject<UAnimSequence>(nullptr,*A->Prepared):nullptr;
            auto* Destination=B?LoadObject<UAnimSequence>(nullptr,*B->Prepared):nullptr;
            FString Playback,Preview;
            if (Full&&Source&&Destination&&R.OutputDirectory==ActiveDirectory&&ActiveKey==EdgeKey(*E)&&IsPrepared(*A)&&IsPrepared(*B)
                &&BuildTransitionConnection(*Source,*Destination,*Full,E->Frames,OutputFolder()/(TEXT("Edge_")+E->Id),Playback,Preview,Error))
            {E->FullResult=R.AssetPath; E->Playback=Playback; E->Preview=Preview; E->GenerationKey=ActiveKey; E->Status=TEXT("Ready"); E->Error.Reset();}
            else {E->Status=TEXT("Failed"); E->Error=!R.Error.IsEmpty()?R.Error:(!Error.IsEmpty()?Error:TEXT("Input changed or result did not match this connection."));}
        }
        Work=EWork::Idle; Touch();
    }
    while (PendingMotions.Num())
    {
        ActiveId=PendingMotions[0]; PendingMotions.RemoveAt(0); auto* M=Motion(ActiveId); if (!M)continue;
        auto* Original=LoadObject<UAnimSequence>(nullptr,*M->Source); M->SourceStamp=TransitionSourceStamp(Original);
        if (M->SourceStamp.IsEmpty()) {M->Status=TEXT("Failed"); M->Error=TEXT("Save the original Animation Sequence before preparing it."); Touch(); continue;}
        auto* Prepared=PrepareTransitionMotion(*Original,OutputFolder()/(TEXT("Motion_")+M->Id),M->Samples,M->Error);
        if (!Prepared) {M->Status=TEXT("Failed"); Touch(); continue;}
        M->Prepared=Prepared->GetPathName(); MappingOptions.MaxSamples=M->Samples;
        ActiveKey=TransitionModel::PreparationKey(M->SourceStamp,MappingOptions);
        if (!StartMannyInputMapping(Prepared,MappingOptions,ActiveDirectory,M->Error)) {M->Status=TEXT("Failed"); Touch(); continue;}
        M->Status=TEXT("Converting"); Work=EWork::Mapping; Message=TEXT("Converting ")+M->Name; Touch(); return true;
    }
    while (PendingEdges.Num())
    {
        ActiveId=PendingEdges[0]; PendingEdges.RemoveAt(0); auto* E=Connection(ActiveId); if(!E)continue;
        auto* A=Motion(E->From); auto* B=Motion(E->To);
        if (!A||!B||A->Status!=TEXT("Ready")||B->Status!=TEXT("Ready")) {E->Status=TEXT("Failed"); E->Error=TEXT("Prepare both endpoint motions first."); Touch(); continue;}
        InferenceOptions.TransitionFrames=E->Frames; ActiveKey=EdgeKey(*E);
        if (!StartMannyInference(A->Input,B->Input,InferenceOptions,ActiveDirectory,E->Error)) {E->Status=TEXT("Failed"); Touch(); continue;}
        E->Status=TEXT("Generating"); E->Job=ActiveDirectory; Work=EWork::Inference;
        Message=TEXT("Generating ")+A->Name+TEXT(" -> ")+B->Name; Touch(); return true;
    }
    Finish(); return false;
}
void FTransitionEditorModel::Finish()
{
    bBusy=false; Work=EWork::Idle; Ticker.Reset();
    int32 Ready=0; for(const auto& E:Connections)if(E.Status==TEXT("Ready"))++Ready;
    if(bGenerate)
    {
        FString Error;
        if(BuildBlueprint(Error)) {Message=FString::Printf(TEXT("Completed: %d transitions and ABP saved. Use the motion buttons to preview."),Ready); OnBlueprintReady.ExecuteIfBound();}
        else Message=FString::Printf(TEXT("%d/%d transitions ready. ABP: %s"),Ready,Connections.Num(),*Error);
    }
    else {int32 Prepared=0;for(const auto& M:Motions)if(M.Status==TEXT("Ready"))++Prepared;Message=FString::Printf(TEXT("%d/%d motions prepared. Connect nodes, set frames, then Generate + ABP. Failed motions show the reason in the inspector."),Prepared,Motions.Num());}
    Touch();
}
void FTransitionEditorModel::Cancel()
{
    if(!bBusy)return;
    if(Work==EWork::Mapping&&GetLastMappingResult().OutputDirectory==ActiveDirectory)CancelMannyInputMapping();
    if(Work==EWork::Inference&&GetLastInferenceResult().OutputDirectory==ActiveDirectory)CancelMannyInference();
    if(Ticker.IsValid())FTSTicker::GetCoreTicker().RemoveTicker(Ticker); Ticker.Reset();
    for(auto& M:Motions)if(M.Status==TEXT("Queued")||M.Status==TEXT("Converting"))M.Status=TEXT("Cancelled");
    for(auto& E:Connections)if(E.Status==TEXT("Generating"))E.Status=TEXT("Cancelled");
    bBusy=false; Work=EWork::Idle; PendingMotions.Reset(); PendingEdges.Reset(); Message=TEXT("Cancelled. Completed results kept; Generate retries unfinished connections."); Touch();
}
bool FTransitionEditorModel::BuildBlueprint(FString& Error)
{
    if (!Motion(DefaultMotion)) {Error=TEXT("Choose a default motion.");return false;}
    TSet<FString> Reached; Reached.Add(DefaultMotion);
    for(int32 I=0;I<Motions.Num();++I)for(const auto& E:Connections)if(Reached.Contains(E.From))Reached.Add(E.To);
    if(Reached.Num()!=Motions.Num()){Error=TEXT("Some motions cannot be reached from the default. Connect or remove them.");return false;}
    for(const auto& M:Motions)if(!IsPrepared(M)){Error=TEXT("A motion changed; prepare it again.");return false;}
    FString Key=DefaultMotion;
    for(const auto& M:Motions)Key+=M.Id+M.Name+M.Prepared+M.PreparationKey;
    for(const auto& E:Connections){if(!IsCurrent(E)){Error=TEXT("Generate every connection successfully before building the ABP.");return false;}Key+=E.GenerationKey+E.Playback;}
    Key=TransitionModel::Digest(Key);
    if(Key==BlueprintKey&&!Blueprint.IsEmpty()&&LoadObject<UAnimBlueprint>(nullptr,*Blueprint))return true;
    auto* BP=BuildTransitionBlueprint(*this,Error); if(!BP)return false;
    Blueprint=BP->GetPathName(); BlueprintKey=Key; Save();return true;
}
}
