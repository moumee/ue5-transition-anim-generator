#include "PhaseLockedLocomotionAnimInstance.h"

#include "Animation/AnimStateMachineTypes.h"

DEFINE_LOG_CATEGORY_STATIC(LogPhaseLockedLocomotion, Log, All);

UPhaseLockedLocomotionAnimInstance::UPhaseLockedLocomotionAnimInstance()
{
	// The N20 comparison clips are generated from the last 10 of the first 31 samples (30 fps) of the source
	// loop: their frames 0-9 are loop samples 21-30 for Walk (33 samples) and 11-20 for Jog (21 samples). The
	// blueprint starts the clips on frame 7 (Start Position 7/30 s) with a 0.25 s crossfade, so that the blend
	// out of the loop runs across the join between the copied and the generated frames (9 -> 10).
	LoopExitTimes.Add(TEXT("Walk"), 28.0f / 30.0f);
	LoopExitTimes.Add(TEXT("Jog"), 18.0f / 30.0f);
}

void UPhaseLockedLocomotionAnimInstance::NativeInitializeAnimation()
{
	Super::NativeInitializeAnimation();

	MovingProperty = FindFProperty<FBoolProperty>(GetClass(), TEXT("bIsMoving"));
	JoggingProperty = FindFProperty<FBoolProperty>(GetClass(), TEXT("bIsJogging"));
	if (!MovingProperty || !JoggingProperty)
	{
		UE_LOG(LogPhaseLockedLocomotion, Warning, TEXT("%s has no bIsMoving / bIsJogging variables; its own transition rules stay in use."), *GetClass()->GetName());
		return;
	}

	// Transitions that share a rule in the blueprint also share its result node, so every rule gets the same
	// kind of delegate and the delegate looks at the state being left.
	struct FTransition { FName From; FName To; ERequest Request; };
	const FTransition Transitions[] = {
		{ TEXT("Idle"), TEXT("IdleToWalk"), ERequest::Walk }, { TEXT("Idle"), TEXT("IdleToJog"), ERequest::Jog },
		{ TEXT("Walk"), TEXT("WalkToJog"), ERequest::Jog },   { TEXT("Walk"), TEXT("WalkToIdle"), ERequest::Idle },
		{ TEXT("Jog"), TEXT("JogToWalk"), ERequest::Walk },   { TEXT("Jog"), TEXT("JogToIdle"), ERequest::Idle },
	};
	for (const FTransition& Transition : Transitions)
	{
		FName Existing;
		if (!HasNativeTransitionBinding(MachineName, Transition.From, Transition.To, Existing))
		{
			AddNativeTransitionBinding(MachineName, Transition.From, Transition.To,
				FCanTakeTransition::CreateUObject(this, &UPhaseLockedLocomotionAnimInstance::CanStartTransition, Transition.Request));
		}
	}
}

bool UPhaseLockedLocomotionAnimInstance::CanStartTransition(ERequest Request)
{
	const bool bMoving = MovingProperty->GetPropertyValue_InContainer(this);
	const bool bJogging = JoggingProperty->GetPropertyValue_InContainer(this);
	const bool bRequested = Request == ERequest::Idle ? !bMoving : bMoving && bJogging == (Request == ERequest::Jog);
	return bRequested && IsCurrentLoopAtExit();
}

bool UPhaseLockedLocomotionAnimInstance::IsCurrentLoopAtExit()
{
	const int32 Machine = GetStateMachineIndex(MachineName);
	const FBakedAnimationStateMachine* Description = GetStateMachineInstanceDesc(MachineName);
	if (Machine == INDEX_NONE || !Description)
	{
		return true;
	}
	const FName State = GetCurrentStateName(Machine);
	const float* ExitTime = LoopExitTimes.Find(State);
	if (!ExitTime)
	{
		return true;                                  // Idle and other states without a phase
	}
	if (GetInstanceCurrentStateElapsedTime(Machine) < MinTimeInLoop)
	{
		return false;                                 // still blending in (and on the first tick the player reports its old time)
	}
	const int32 StateIndex = Description->FindStateIndex(State);
	const float Length = GetRelevantAnimLength(Machine, StateIndex);
	float SinceExit = GetRelevantAnimTime(Machine, StateIndex) - *ExitTime;
	if (SinceExit < 0.0f && Length > 0.0f)
	{
		SinceExit += Length;                          // the window may run past the end of the loop
	}
	return SinceExit >= 0.0f && SinceExit < ExitWindow;
}
