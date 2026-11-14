#include "MotionTransitionAnimInstance.h"
#include "Animation/AnimSequence.h"

UMotionTransitionAnimInstance::UMotionTransitionAnimInstance() { bUseMultiThreadedAnimationUpdate = false; }
const FMotionTransitionState* UMotionTransitionAnimInstance::FindMotion(FName Name) const
{
    return Motions.FindByPredicate([Name](const auto& M) { return M.Name == Name && M.Animation && M.Animation->GetPlayLength() > 0; });
}
const FMotionTransitionEdge* UMotionTransitionAnimInstance::FindTransition(FName From, FName To) const
{
    return Transitions.FindByPredicate([this, From, To](const auto& E) { return E.From == From && E.To == To && E.Animation && E.Animation->GetPlayLength() > 0 && FindMotion(To); });
}
void UMotionTransitionAnimInstance::ResetPlayback()
{
    bInTransition = false; PlaybackTime = 0; DestinationMotion = NAME_None;
    const auto* Initial = FindMotion(DefaultMotion);
    if (!Initial && Motions.Num()) Initial = FindMotion(Motions[0].Name);
    ActiveMotion = Initial ? Initial->Name : NAME_None;
    RequestedMotion = ActiveMotion;
    PlaybackSequence = Initial ? Initial->Animation.Get() : nullptr;
    PlaybackStatus = Initial ? TEXT("Looping") : TEXT("No valid motion");
}
void UMotionTransitionAnimInstance::NativeInitializeAnimation() { Super::NativeInitializeAnimation(); ResetPlayback(); }
void UMotionTransitionAnimInstance::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);
    if (!PlaybackSequence) ResetPlayback();
    if (!PlaybackSequence || !FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0) return;
    double Remaining = DeltaSeconds;
    for (int32 Step = 0; Step < 256 && PlaybackSequence; ++Step)
    {
        const double Length = PlaybackSequence->GetPlayLength();
        if (Length <= 0) break;
        const auto* Edge = !bInTransition && RequestedMotion != ActiveMotion ? FindTransition(ActiveMotion, RequestedMotion) : nullptr;
        if (!bInTransition && !Edge)
        {
            PlaybackTime = FMath::Fmod(PlaybackTime + Remaining, Length);
            PlaybackStatus = RequestedMotion.IsNone() || RequestedMotion == ActiveMotion ? TEXT("Looping") : TEXT("No direct connection; keeping current motion");
            return;
        }
        PlaybackStatus = bInTransition ? TEXT("Playing transition; latest request waits") : TEXT("Waiting for loop end");
        const double UntilBoundary = FMath::Max(0.0, Length - PlaybackTime);
        if (Remaining + 1e-7 < UntilBoundary) { PlaybackTime += Remaining; return; }
        Remaining = FMath::Max(0.0, Remaining - UntilBoundary);
        PlaybackTime = 0;
        if (bInTransition)
        {
            const auto* Target = FindMotion(DestinationMotion);
            if (!Target) { ResetPlayback(); return; }
            ActiveMotion = Target->Name; PlaybackSequence = Target->Animation;
            DestinationMotion = NAME_None; bInTransition = false;
        }
        else
        {
            DestinationMotion = Edge->To; PlaybackSequence = Edge->Animation; bInTransition = true;
        }
        if (Remaining <= 1e-7) return;
    }
    PlaybackStatus = TEXT("Playback step limit reached");
}
