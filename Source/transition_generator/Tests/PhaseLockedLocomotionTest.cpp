#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Animation/AnimInstance.h"
#include "Animation/AnimStateMachineTypes.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"

// Drives /Game/Comparison/BP_Transition through Idle -> Walk -> Jog -> Walk -> Idle -> Jog -> Idle at 60 fps and
// checks where each loop is left and how far planted feet slide around the state changes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPhaseLockedLocomotionTest, "TransitionGenerator.PhaseLockedLocomotion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPhaseLockedLocomotionTest::RunTest(const FString& Parameters)
{
	UClass* CharacterClass = LoadClass<ACharacter>(nullptr, TEXT("/Game/Comparison/BP_Transition.BP_Transition_C"));
	if (!TestNotNull(TEXT("BP_Transition loads"), CharacterClass)) return false;

	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(World);
	World->InitializeActorsForPlay(FURL());
	World->BeginPlay();
	World->GetWorldSettings()->NotifyBeginPlay();       // no game mode here, so start play by hand
	ON_SCOPE_EXIT
	{
		for (AActor* Actor : World->PersistentLevel->Actors)
		{
			if (Actor) Actor->RouteEndPlay(EEndPlayReason::Quit);
		}
		World->SetBegunPlay(false);
		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
	};

	ACharacter* Character = World->SpawnActor<ACharacter>(CharacterClass, FTransform::Identity);
	if (!TestNotNull(TEXT("Character spawns"), Character)) return false;
	// There is no floor and no controller: fly level and let the root motion move the actor.
	Character->GetCharacterMovement()->bRunPhysicsWithNoController = true;
	Character->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	USkeletalMeshComponent* Mesh = Character->GetMesh();
	Mesh->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	Mesh->bEnableUpdateRateOptimizations = false;
	const FBoolProperty* Moving = FindFProperty<FBoolProperty>(CharacterClass, TEXT("bIsMoving"));
	const FBoolProperty* Jogging = FindFProperty<FBoolProperty>(CharacterClass, TEXT("bIsJogging"));
	UAnimInstance* Anim = Mesh->GetAnimInstance();
	if (!TestTrue(TEXT("Character has the expected variables and an anim instance"), Moving && Jogging && Anim)) return false;

	const FName MachineName(TEXT("LocomotionSM"));
	const int32 Machine = Anim->GetStateMachineIndex(MachineName);
	const FBakedAnimationStateMachine* Description = Anim->GetStateMachineInstanceDesc(MachineName);
	if (!TestTrue(TEXT("LocomotionSM exists"), Machine != INDEX_NONE && Description)) return false;
	const int32 Feet[] = { Mesh->GetBoneIndex(TEXT("foot_l")), Mesh->GetBoneIndex(TEXT("foot_r")) };
	if (!TestTrue(TEXT("Foot bones exist"), Feet[0] != INDEX_NONE && Feet[1] != INDEX_NONE)) return false;

	struct FInput { float Time; bool bMoving; bool bJogging; };
	const FInput Script[] = { { 0.5f, true, false }, { 3.0f, true, true }, { 5.5f, true, false }, { 8.0f, false, false },
		{ 10.0f, true, true }, { 12.5f, false, false } };
	const float DeltaTime = 1.0f / 60.0f;
	const int32 TickCount = 15 * 60;
	const float JoinSeconds = 0.35f;                  // the part after a state change where a phase mismatch shows

	// Set PHASE_LOCK_TRACE to a file path to get every bone's component-space position per tick for offline analysis.
	const FString TracePath = FPlatformMisc::GetEnvironmentVariable(TEXT("PHASE_LOCK_TRACE"));
	TArray<FString> Trace;
	if (!TracePath.IsEmpty())
	{
		FString Header = TEXT("time state since_change actor_x actor_y actor_z");
		for (int32 Bone = 0; Bone < Mesh->GetNumBones(); ++Bone) Header += TEXT(" ") + Mesh->GetBoneName(Bone).ToString();
		Trace.Add(Header);
	}

	struct FSample { bool bTransitionClip; float SinceChange; FVector Foot[2]; };
	TArray<FSample> Samples;
	TArray<FString> States;
	FName State = Anim->GetCurrentStateName(Machine);
	States.Add(State.ToString());
	float StateTime = 0.0f, SinceChange = JoinSeconds;
	for (int32 Tick = 0; Tick < TickCount; ++Tick)
	{
		const float Now = Tick * DeltaTime;
		for (const FInput& Input : Script)
		{
			if (Now >= Input.Time)
			{
				Moving->SetPropertyValue_InContainer(Character, Input.bMoving);
				Jogging->SetPropertyValue_InContainer(Character, Input.bJogging);
			}
		}
		++GFrameCounter;
		World->Tick(LEVELTICK_All, DeltaTime);
		Mesh->HandleExistingParallelEvaluationTask(true, true);

		const FName NewState = Anim->GetCurrentStateName(Machine);
		SinceChange += DeltaTime;
		if (NewState != State)
		{
			// StateTime is what the rule saw: the time of the old state's player after the previous tick.
			UE_LOG(LogTemp, Display, TEXT("PhaseLock %6.3f s  %-10s -> %-10s (left at %.3f s)"), Now, *State.ToString(), *NewState.ToString(), StateTime);
			// Loops are left on the sample the clips start from (their frame 7: Walk 28, Jog 18), give or take the tick.
			const auto Near = [StateTime](float Exit, float Length) { const float Late = FMath::Fmod(StateTime - Exit + Length, Length); return Late < 0.08f || Late > Length - 0.001f; };
			if (State == TEXT("Walk")) TestTrue(TEXT("Walk is left at loop sample 28"), Near(28.0f / 30.0f, 32.0f / 30.0f));
			if (State == TEXT("Jog")) TestTrue(TEXT("Jog is left at loop sample 18"), Near(18.0f / 30.0f, 0.7f));
			// The blueprint blends into the loop over the last 0.2 s of the 1 s clip.
			if (State.ToString().Contains(TEXT("To"))) TestTrue(TEXT("Transition clips play to their end"), StateTime >= 0.8f - 2.0f * DeltaTime);
			States.Add(NewState.ToString());
			State = NewState;
			SinceChange = 0.0f;
		}
		StateTime = Anim->GetRelevantAnimTime(Machine, Description->FindStateIndex(State));

		if (!TracePath.IsEmpty())
		{
			const FVector Actor = Character->GetActorLocation();
			FString Line = FString::Printf(TEXT("%.4f %s %.4f %.3f %.3f %.3f"), Now, *State.ToString(), SinceChange, Actor.X, Actor.Y, Actor.Z);
			for (const FTransform& Bone : Mesh->GetComponentSpaceTransforms())
			{
				Line += FString::Printf(TEXT(" %.3f %.3f %.3f"), Bone.GetTranslation().X, Bone.GetTranslation().Y, Bone.GetTranslation().Z);
			}
			Trace.Add(MoveTemp(Line));
		}

		FSample& Sample = Samples.AddDefaulted_GetRef();
		Sample.bTransitionClip = State.ToString().Contains(TEXT("To"));
		Sample.SinceChange = SinceChange;
		for (int32 Foot = 0; Foot < 2; ++Foot)
		{
			// World position, but with the height measured from the actor so that the ground level is constant.
			Sample.Foot[Foot] = Mesh->GetBoneTransform(Feet[Foot]).GetTranslation();
			Sample.Foot[Foot].Z -= Character->GetActorLocation().Z;
		}
	}

	if (!TracePath.IsEmpty()) FFileHelper::SaveStringArrayToFile(Trace, *TracePath);

	TestEqual(TEXT("State order"), FString::Join(States, TEXT(" ")),
		FString(TEXT("Idle IdleToWalk Walk WalkToJog Jog JogToWalk Walk WalkToIdle Idle IdleToJog Jog JogToIdle Idle")));

	// Foot slide: horizontal travel per tick of a foot that stays within 3 cm of its lowest height.
	enum { Loop, Clip, Join, Count };
	double Sum[Count] = {}, Worst[Count] = {};
	int32 Number[Count] = {};
	for (int32 Foot = 0; Foot < 2; ++Foot)
	{
		double Ground = TNumericLimits<double>::Max();
		for (const FSample& Sample : Samples) Ground = FMath::Min(Ground, Sample.Foot[Foot].Z);
		for (int32 Index = 1; Index < Samples.Num(); ++Index)
		{
			const FSample& Before = Samples[Index - 1];
			const FSample& After = Samples[Index];
			if (Before.Foot[Foot].Z > Ground + 3.0 || After.Foot[Foot].Z > Ground + 3.0) continue;
			const double Slide = FVector::Dist2D(Before.Foot[Foot], After.Foot[Foot]);
			const int32 Kind = After.SinceChange < JoinSeconds ? Join : After.bTransitionClip ? Clip : Loop;
			Sum[Kind] += Slide; Worst[Kind] = FMath::Max(Worst[Kind], Slide); ++Number[Kind];
		}
	}
	const TCHAR* Names[Count] = { TEXT("loops"), TEXT("transition clips"), TEXT("first 0.35 s after a state change") };
	double Mean[Count] = {};
	for (int32 Kind = 0; Kind < Count; ++Kind)
	{
		Mean[Kind] = Number[Kind] ? Sum[Kind] / Number[Kind] : 0.0;
		UE_LOG(LogTemp, Display, TEXT("PhaseLock planted-foot slide, %s: mean %.2f cm/tick, max %.2f cm/tick (%d samples)"), Names[Kind], Mean[Kind], Worst[Kind], Number[Kind]);
	}
	UE_LOG(LogTemp, Display, TEXT("PhaseLock actor travelled %.0f cm"), Character->GetActorLocation().Size2D());
	TestTrue(TEXT("Root motion moves the actor"), Character->GetActorLocation().Size2D() > 1000.0);
	// Phase-matched joins blend a loop with a copy of itself, so feet slide no more there than inside the clips.
	TestTrue(TEXT("Feet stay planted across state changes"), Mean[Join] <= FMath::Max(Mean[Clip], Mean[Loop]) + 0.25);
	return true;
}

#endif
