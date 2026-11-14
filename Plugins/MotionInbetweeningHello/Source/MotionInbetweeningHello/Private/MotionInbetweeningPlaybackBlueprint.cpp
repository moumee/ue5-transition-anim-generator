#include "MotionInbetweeningPlayback.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "AnimationStateMachineGraph.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_StateMachine.h"
#include "AnimStateEntryNode.h"
#include "AnimStateNode.h"
#include "AnimStateTransitionNode.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/SkeletalMesh.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace MotionInbetweening
{
namespace PlaybackBlueprint
{
template<typename T> T* AddNode(UEdGraph& Graph, int32 X, int32 Y)
{
	T* Node = NewObject<T>(&Graph, NAME_None, RF_Transactional);
	Graph.AddNode(Node, false, false);
	Node->CreateNewGuid();
	Node->PostPlacedNewNode();
	Node->AllocateDefaultPins();
	Node->NodePosX = X; Node->NodePosY = Y;
	return Node;
}

UAnimStateNode* AddState(UAnimationStateMachineGraph& Graph, UAnimSequence& Clip, const TCHAR* Name, int32 X)
{
	auto* State = AddNode<UAnimStateNode>(Graph, X, 0);
	FBlueprintEditorUtils::RenameGraph(State->BoundGraph, Name);
	State->bAlwaysResetOnEntry = true;
	auto* Player = AddNode<UAnimGraphNode_SequencePlayer>(*State->BoundGraph, -300, 0);
	Player->Node.SetSequence(&Clip);
	Player->Node.SetLoopAnimation(false);
	Player->Node.SetStartPosition(0.0f);
	UEdGraphPin* Pose = Player->FindPin(TEXT("Pose"));
	UEdGraphPin* Result = State->GetPoseSinkPinInsideState();
	if (!Pose || !Result || !State->BoundGraph->GetSchema()->TryCreateConnection(Pose, Result)) return nullptr;
	return State;
}

void Connect(UAnimationStateMachineGraph& Graph, UAnimStateNode& From, UAnimStateNode& To)
{
	auto* Transition = AddNode<UAnimStateTransitionNode>(Graph, (From.NodePosX + To.NodePosX) / 2, 80);
	Transition->CrossfadeDuration = 0.0f;
	Transition->bAutomaticRuleBasedOnSequencePlayerInState = true;
	Transition->AutomaticRuleTriggerTime = 0.0f;
	From.GetOutputPin()->MakeLinkTo(Transition->GetInputPin());
	Transition->GetOutputPin()->MakeLinkTo(To.GetInputPin());
}
}

UAnimBlueprint* CreatePlaybackPreviewBlueprint(USkeletalMesh& Mesh, UAnimSequence& Source,
	UAnimSequence& Bridge, UAnimSequence& Destination, FString& Error)
{
	Error.Reset();
	if (!Mesh.GetSkeleton() || Source.GetSkeleton() != Mesh.GetSkeleton()
		|| Bridge.GetSkeleton() != Mesh.GetSkeleton() || Destination.GetSkeleton() != Mesh.GetSkeleton())
	{
		Error = TEXT("Preview ABP clips must share the preview mesh skeleton."); return nullptr;
	}
	const FString Base = FPackageName::GetLongPackagePath(Source.GetOutermost()->GetName()) / TEXT("ABP_PlaybackPreview");
	if (!Base.StartsWith(TEXT("/Game/MotionInbetweening/")))
	{
		Error = TEXT("Preview ABP output must be under /Game/MotionInbetweening/."); return nullptr;
	}
	FString PackageName, AssetName;
	FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateUniqueAssetName(Base, TEXT(""), PackageName, AssetName);
	if (FPackageName::DoesPackageExist(PackageName) || FindPackage(nullptr, *PackageName))
	{
		Error = TEXT("Refusing to overwrite an existing preview ABP."); return nullptr;
	}
	UPackage* Package = CreatePackage(*PackageName);
	auto* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->ParentClass = UAnimInstance::StaticClass();
	Factory->TargetSkeleton = Mesh.GetSkeleton();
	Factory->PreviewSkeletalMesh = &Mesh;
	auto* BP = Cast<UAnimBlueprint>(Factory->FactoryCreateNew(UAnimBlueprint::StaticClass(), Package,
		*AssetName, RF_Public | RF_Standalone | RF_Transactional, nullptr, GWarn));
	if (!BP) { Error = TEXT("AnimBlueprint factory failed."); Package->SetDirtyFlag(false); return nullptr; }
	auto Fail = [&](const FString& Message) -> UAnimBlueprint*
	{
		Error = Message; BP->ClearFlags(RF_Public | RF_Standalone); BP->MarkAsGarbage();
		Package->SetDirtyFlag(false); return nullptr;
	};
	UEdGraph* AnimGraph = nullptr;
	for (UEdGraph* Graph : BP->FunctionGraphs)
		if (Graph->GetFName() == UEdGraphSchema_K2::GN_AnimGraph) AnimGraph = Graph;
	if (!AnimGraph) return Fail(TEXT("Factory did not create an AnimGraph."));
	UAnimGraphNode_Root* Root = nullptr;
	for (UEdGraphNode* Node : AnimGraph->Nodes)
		if (auto* Candidate = Cast<UAnimGraphNode_Root>(Node)) Root = Candidate;
	if (!Root) return Fail(TEXT("AnimGraph has no output pose node."));
	auto* Machine = PlaybackBlueprint::AddNode<UAnimGraphNode_StateMachine>(*AnimGraph, -400, 0);
	FBlueprintEditorUtils::RenameGraph(Machine->EditorStateMachineGraph, TEXT("TransitionPreview"));
	if (!AnimGraph->GetSchema()->TryCreateConnection(Machine->FindPin(TEXT("Pose")), Root->FindPin(TEXT("Result"))))
		return Fail(TEXT("Could not connect the state machine output."));
	auto& Graph = *Machine->EditorStateMachineGraph;
	Graph.EntryNode->NodePosX = -250;
	Graph.EntryNode->NodePosY = 0;
	auto* A = PlaybackBlueprint::AddState(Graph, Source, TEXT("Source"), 0);
	auto* T = PlaybackBlueprint::AddState(Graph, Bridge, TEXT("Bridge"), 250);
	auto* B = PlaybackBlueprint::AddState(Graph, Destination, TEXT("Destination"), 500);
	if (!A || !T || !B) return Fail(TEXT("Could not connect a state sequence player."));
	Graph.EntryNode->GetOutputPin()->MakeLinkTo(A->GetInputPin());
	PlaybackBlueprint::Connect(Graph, *A, *T);
	PlaybackBlueprint::Connect(Graph, *T, *B);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(BP);
	FCompilerResultsLog Log;
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::None, &Log);
	if (Log.NumErrors || BP->Status == BS_Error || !BP->GeneratedClass)
		return Fail(FString::Printf(TEXT("Preview ABP compile failed with %d errors. See the Blueprint compiler log."), Log.NumErrors));
	BP->MarkPackageDirty();
	FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
	const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!UPackage::SavePackage(Package, BP, *Filename, Args)) return Fail(TEXT("Preview ABP could not be saved."));
	FAssetRegistryModule::AssetCreated(BP);
	return BP;
}
}
