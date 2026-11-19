#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "PhaseLockedLocomotionAnimInstance.generated.h"

/**
 * Parent class for locomotion state machines that play generated transition clips between loops.
 *
 * A generated clip begins with a copy of its source loop from one fixed sample and ends on the first sample of
 * the destination loop, so the feet only line up when the loop is left at that sample. This class replaces the
 * blueprint rules that start a transition clip with native ones: the same bIsMoving / bIsJogging request, and,
 * while a loop listed in LoopExitTimes is playing, that loop having just reached its exit time.
 *
 * The blueprint keeps its states, its bIsMoving / bIsJogging variables and the transitions named in the .cpp file.
 */
UCLASS()
class TRANSITION_GENERATOR_API UPhaseLockedLocomotionAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	UPhaseLockedLocomotionAnimInstance();
	virtual void NativeInitializeAnimation() override;

protected:
	UPROPERTY(EditDefaultsOnly, Category = "Phase Lock")
	FName MachineName = TEXT("LocomotionSM");

	/** Loop state name -> loop time in seconds at which its transition clips may start. */
	UPROPERTY(EditDefaultsOnly, Category = "Phase Lock")
	TMap<FName, float> LoopExitTimes;

	/** How long after the exit time a loop may still be left. The transition's crossfade absorbs the lateness. */
	UPROPERTY(EditDefaultsOnly, Category = "Phase Lock", meta = (ClampMin = "0.0"))
	float ExitWindow = 0.07f;

	/** A loop is not left again before it has played this long, so that the blend into it has finished. */
	UPROPERTY(EditDefaultsOnly, Category = "Phase Lock", meta = (ClampMin = "0.0"))
	float MinTimeInLoop = 0.3f;

private:
	enum class ERequest : uint8 { Idle, Walk, Jog };

	bool CanStartTransition(ERequest Request);
	bool IsCurrentLoopAtExit();

	const FBoolProperty* MovingProperty = nullptr;
	const FBoolProperty* JoggingProperty = nullptr;
};
