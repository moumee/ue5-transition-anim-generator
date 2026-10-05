#if WITH_DEV_AUTOMATION_TESTS

#include "MotionInbetweeningMapping.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

class FWaitForMannyMapping : public IAutomationLatentCommand
{
public:
	FWaitForMannyMapping(FAutomationTestBase* InTest, FString InDirectory, bool bInNormalized)
		: Test(InTest), Directory(MoveTemp(InDirectory)), bNormalized(bInNormalized), Deadline(FPlatformTime::Seconds() + 125) {}

	virtual bool Update() override
	{
		if (MotionInbetweening::IsMannyInputMappingRunning())
		{
			if (FPlatformTime::Seconds() < Deadline) return false;
			MotionInbetweening::CancelMannyInputMapping();
			Test->AddError(TEXT("Input-mapping test timed out."));
			return true;
		}
		const MotionInbetweening::FMappingResult& Result = MotionInbetweening::GetLastMappingResult();
		Test->TestTrue(*FString::Printf(TEXT("Mapping succeeds: %s"), *Result.Error), Result.bSuccess);
		Test->TestEqual(TEXT("Same job directory"), Result.OutputDirectory, Directory);
		Test->TestEqual(TEXT("Walk sample count"), Result.SampleCount, 31);
		Test->TestEqual(TEXT("Normalization matches configured statistics"), Result.bNormalized, bNormalized);
		FString Text;
		TSharedPtr<FJsonObject> Json;
		if (Test->TestTrue(TEXT("Model input JSON can be read"), FFileHelper::LoadFileToString(Text, *(Directory / TEXT("model_input.json")))
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid()))
		{
			Test->TestEqual(TEXT("30 fps"), Json->GetNumberField(TEXT("sample_rate_hz")), 30.0);
			Test->TestEqual(TEXT("10-frame context"), Json->GetNumberField(TEXT("context_len")), 10.0);
			Test->TestTrue(TEXT("Heading alignment applied"), Json->GetBoolField(TEXT("heading_alignment_applied")));
			const TArray<TSharedPtr<FJsonValue>>& Rows = Json->GetArrayField(TEXT("vectors_tx135"));
			Test->TestEqual(TEXT("31 input rows"), Rows.Num(), 31);
			for (const auto& Row : Rows)
			{
				Test->TestEqual(TEXT("135 channels per row"), Row->AsArray().Num(), 135);
				for (const auto& Value : Row->AsArray()) Test->TestTrue(TEXT("Finite model input"), FMath::IsFinite(Value->AsNumber()));
			}
		}
		Test->AddInfo(TEXT("Manny mapping output: ") + Directory);
		return true;
	}
private:
	FAutomationTestBase* Test;
	FString Directory;
	bool bNormalized;
	double Deadline;
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMotionInbetweeningMappingTest, "MotionInbetweening.MannyInputMapping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMotionInbetweeningMappingTest::RunTest(const FString& Parameters)
{
	UAnimSequence* Walk = LoadObject<UAnimSequence>(nullptr,
		TEXT("/Game/ControlRig/Characters/Mannequins/Animations/Manny/MM_Walk_Fwd.MM_Walk_Fwd"));
	if (!TestNotNull(TEXT("Real Manny Walk loads"), Walk)) return false;
	MotionInbetweening::FMappingOptions Options = MotionInbetweening::GetMappingOptions();
	Options.MaxSamples = 31;
	Options.bOpenOutputFolder = false;
	if (!TestTrue(TEXT("Configure a Python 3.11+ executable with NumPy in Motion In-betweening settings"), FPaths::FileExists(Options.PythonExecutable))) return false;
	FString Directory, Error;
	auto Bad = Options;
	Bad.PythonExecutable.Reset();
	TestFalse(TEXT("Missing Python is rejected before export"), MotionInbetweening::StartMannyInputMapping(Walk, Bad, Directory, Error));
	TestTrue(TEXT("No output directory for rejected configuration"), Directory.IsEmpty());
	Bad = Options; Bad.MaxSamples = 1;
	TestFalse(TEXT("Insufficient context is rejected"), MotionInbetweening::StartMannyInputMapping(Walk, Bad, Directory, Error));
	TestFalse(TEXT("Missing selected animation is rejected"), MotionInbetweening::StartMannyInputMapping(nullptr, Options, Directory, Error));
	if (!TestTrue(*FString::Printf(TEXT("Start actual input export: %s"), *Error), MotionInbetweening::StartMannyInputMapping(Walk, Options, Directory, Error))) return false;
	FString SecondDirectory;
	TestFalse(TEXT("Duplicate launch is rejected while active"), MotionInbetweening::StartMannyInputMapping(Walk, Options, SecondDirectory, Error));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitForMannyMapping(this, Directory, !Options.StatisticsFile.IsEmpty()));
	return true;
}

#endif
