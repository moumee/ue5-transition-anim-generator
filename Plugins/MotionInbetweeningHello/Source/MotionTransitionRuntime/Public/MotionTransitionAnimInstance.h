#pragma once
#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "MotionTransitionAnimInstance.generated.h"

class UAnimSequence;
class UAnimSequenceBase;

USTRUCT(BlueprintType)
struct MOTIONTRANSITIONRUNTIME_API FMotionTransitionState
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") FName Name;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") TObjectPtr<UAnimSequence> Animation;
};

USTRUCT(BlueprintType)
struct MOTIONTRANSITIONRUNTIME_API FMotionTransitionEdge
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") FName From;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") FName To;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") TObjectPtr<UAnimSequence> Animation;
};

/** Runtime-only fixed-anchor playback. Generated clips are in-place and contain no notifies. */
UCLASS(Blueprintable, BlueprintType)
class MOTIONTRANSITIONRUNTIME_API UMotionTransitionAnimInstance : public UAnimInstance
{
    GENERATED_BODY()
public:
    UMotionTransitionAnimInstance();
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") TArray<FMotionTransitionState> Motions;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") TArray<FMotionTransitionEdge> Transitions;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Motion Transition") FName DefaultMotion;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Motion Transition") FName RequestedMotion;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Motion Transition") FName ActiveMotion;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Motion Transition") FName DestinationMotion;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Motion Transition") bool bInTransition = false;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Motion Transition") TObjectPtr<UAnimSequenceBase> PlaybackSequence;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Motion Transition") float PlaybackTime = 0;
    UPROPERTY(Transient, BlueprintReadOnly, Category="Motion Transition") FString PlaybackStatus;
    UFUNCTION(BlueprintCallable, Category="Motion Transition") void RequestMotion(FName Motion) { RequestedMotion = Motion; }
    UFUNCTION(BlueprintCallable, Category="Motion Transition") void ResetPlayback();
    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;
private:
    const FMotionTransitionState* FindMotion(FName Name) const;
    const FMotionTransitionEdge* FindTransition(FName From, FName To) const;
};
