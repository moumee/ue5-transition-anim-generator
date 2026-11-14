#pragma once
#include "CoreMinimal.h"
#include "MotionInbetweeningMapping.h"
#include "MotionInbetweeningInference.h"
#include "Containers/Ticker.h"

class UAnimSequence;
class UAnimBlueprint;
namespace MotionInbetweening
{
struct FTransitionMotion
{
    FString Id, Name, Source, SourceStamp, Prepared, Input, InputHash, PreparationKey, Status, Error;
    FVector2D Position = FVector2D::ZeroVector;
    int32 Samples = 0;
};
struct FTransitionConnection
{
    FString Id, From, To, FullResult, Playback, Preview, GenerationKey, Status, Error, Job;
    int32 Frames = 15;
};
class FTransitionEditorModel : public TSharedFromThis<FTransitionEditorModel>
{
public:
    FString Id, Name = TEXT("Locomotion"), DefaultMotion, Blueprint, BlueprintKey, Message;
    TArray<FTransitionMotion> Motions;
    TArray<FTransitionConnection> Connections;
    FString SelectedMotion, SelectedConnection;
    uint32 Revision = 0;
    FSimpleDelegate OnChanged;
    FSimpleDelegate OnBlueprintReady;
    FTransitionEditorModel();
    ~FTransitionEditorModel();
    FTransitionMotion* Motion(const FString& Key);
    const FTransitionMotion* Motion(const FString& Key) const;
    FTransitionConnection* Connection(const FString& Key);
    bool AddMotions(const TArray<UAnimSequence*>& Assets);
    bool Connect(const FString& From, const FString& To);
    void ConnectAll();
    void RemoveSelected();
    void SetFrames(const FString& EdgeId, int32 Frames);
    bool RenameMotion(const FString& MotionId, const FString& NewName);
    void SetDefault(const FString& MotionId);
    void Touch();
    bool Save();
    bool Load(const FString& Path);
    void NewSet();
    FString SetPath() const;
    static FString SetsDirectory();
    FString OutputFolder() const;
    bool IsBusy() const { return bBusy; }
    bool Prepare();
    bool Generate();
    void Cancel();
    bool Tick(float Delta);
    bool IsPrepared(const FTransitionMotion& M) const;
    bool IsCurrent(const FTransitionConnection& E) const;
    bool BuildBlueprint(FString& Error);
    int32 MaximumFrames(FString& Reason) const;
    FString SettingsKey() const;
    FString EdgeKey(const FTransitionConnection& E) const;
    void SettingsChanged();
private:
    bool bBusy = false, bGenerate = false;
    enum class EWork { Idle, Mapping, Inference } Work = EWork::Idle;
    TArray<FString> PendingMotions, PendingEdges;
    FString ActiveId, ActiveDirectory, ActiveKey;
    FMappingOptions MappingOptions;
    FInferenceOptions InferenceOptions;
    FTSTicker::FDelegateHandle Ticker;
    bool Start(bool GenerateEdges);
    void Finish();
};

FString TransitionFileHash(const FString& Path);
FString TransitionSourceStamp(UAnimSequence* Asset);
UAnimSequence* PrepareTransitionMotion(UAnimSequence& Original, const FString& Folder, int32& Samples, FString& Error);
bool BuildTransitionConnection(UAnimSequence& Source, UAnimSequence& Destination, UAnimSequence& Full,
    int32 Frames, const FString& Folder, FString& Playback, FString& Preview, FString& Error);
UAnimBlueprint* BuildTransitionBlueprint(const FTransitionEditorModel& Set, FString& Error);
void OpenTransitionEditor();
void ShutdownTransitionEditor();
}
