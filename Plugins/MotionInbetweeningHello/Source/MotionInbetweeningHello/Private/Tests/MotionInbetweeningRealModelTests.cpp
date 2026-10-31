#if WITH_DEV_AUTOMATION_TESTS

#include "MotionInbetweeningInference.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
class FCheckRealModelOutput : public IAutomationLatentCommand
{
public:
    explicit FCheckRealModelOutput(FAutomationTestBase* InTest)
        : Test(InTest), Deadline(FPlatformTime::Seconds() + 180) {}

    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            MotionInbetweening::CancelMannyInference();
            Test->AddError(TEXT("Real model integration exceeded 180 seconds."));
            return true;
        }
        if (MotionInbetweening::IsMannyInferenceRunning()) return false;
        const auto Result = MotionInbetweening::GetLastInferenceResult();
        Test->AddInfo(TEXT("Real model job: ") + Result.OutputDirectory);
        if (!Test->TestTrue(*Result.Error, Result.bSuccess)) return true;
        Test->TestEqual(TEXT("Complete pipeline stage"), Result.Stage, FString(TEXT("completed")));
        Test->TestEqual(TEXT("Context + transition + target samples"), Result.SampleCount, 31);
        FString Text;
        TSharedPtr<FJsonObject> Receipt;
        const bool bRead = FFileHelper::LoadFileToString(Text, *(Result.OutputDirectory / TEXT("pipeline_result.json")))
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Receipt) && Receipt.IsValid();
        if (Test->TestTrue(TEXT("Pipeline receipt exists"), bRead))
        {
            Test->TestTrue(TEXT("Receipt reports actual save success"), Receipt->GetBoolField(TEXT("success")));
            Test->TestEqual(TEXT("Receipt identifies saved asset"), Receipt->GetStringField(TEXT("asset_path")), Result.AssetPath);
        }
        UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, *Result.AssetPath);
        if (Test->TestNotNull(TEXT("Real generated animation loads"), Animation))
        {
            const IAnimationDataModel* Model = Animation->GetDataModel();
            if (Test->TestNotNull(TEXT("Generated animation data model"), Model))
            {
                Test->TestEqual(TEXT("31 animation keys"), Model->GetNumberOfKeys(), 31);
                Test->TestEqual(TEXT("89 Manny bone tracks"), Model->GetNumBoneTracks(), 89);
            }
        }
        Test->AddInfo(TEXT("Real model asset: ") + Result.AssetPath);
        return true;
    }

private:
    FAutomationTestBase* Test;
    double Deadline;
};
}

// Separate prefix: this opt-in test requires actual model files. Missing setup
// fails explicitly instead of counting a skipped model run as a passing test.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMannyRealModelTest, "MotionInbetweeningOptional.RealModel",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMannyRealModelTest::RunTest(const FString& Parameters)
{
    FString Python, Config;
    if (!FParse::Value(FCommandLine::Get(), TEXT("MIBInferencePython="), Python)
        || !FParse::Value(FCommandLine::Get(), TEXT("MIBModelConfig="), Config))
    {
        AddError(TEXT("Set -MIBInferencePython=<python> and -MIBModelConfig=<official model config>."));
        return false;
    }
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Tools/MotionInference"));
    auto Options = MotionInbetweening::GetInferenceOptions();
    Options.MappingPythonExecutable = Python;
    Options.InferencePythonExecutable = Python;
    Options.InferenceScript = Root / TEXT("infer.py");
    Options.ModelConfigFile = Config;
    Options.StatisticsFile.Reset();
    Options.TransitionFrames = 20;
    Options.TimeoutSeconds = 120;
    Options.bOpenAsset = false;
    FString Directory, Error;
    if (!MotionInbetweening::StartMannyInference(Root / TEXT("samples/start_input.json"),
        Root / TEXT("samples/end_input.json"), Options, Directory, Error))
    {
        AddError(Error);
        return false;
    }
    ADD_LATENT_AUTOMATION_COMMAND(FCheckRealModelOutput(this));
    return true;
}

#endif
