#if WITH_DEV_AUTOMATION_TESTS

#include "MotionInbetweeningInference.h"
#include "MotionInbetweeningOutputMapping.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
bool ReadJson(const FString& Path, TSharedPtr<FJsonObject>& Object)
{
	FString Text;
	return FFileHelper::LoadFileToString(Text, *Path)
		&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object) && Object.IsValid();
}

bool WriteJson(const FString& Path, const TSharedRef<FJsonObject>& Object)
{
	FString Text;
	return FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Text))
		&& FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

class FCheckInferencePipeline : public IAutomationLatentCommand
{
public:
	FCheckInferencePipeline(FAutomationTestBase* InTest, FString InInput, MotionInbetweening::FInferenceOptions InOptions)
		: Test(InTest), Input(MoveTemp(InInput)), Options(MoveTemp(InOptions)), Deadline(FPlatformTime::Seconds() + 45) {}

	bool Update() override
	{
		if (FPlatformTime::Seconds() > Deadline)
		{
			MotionInbetweening::CancelMannyInference();
			Test->AddError(TEXT("Inference integration test exceeded its deadline."));
			return true;
		}
		if (Phase == 2 && MotionInbetweening::IsMannyInferenceRunning())
		{
			TSharedPtr<FJsonObject> Marker;
			if (!ReadJson(MotionInbetweening::GetLastInferenceResult().OutputDirectory / TEXT("test_backend_started.json"), Marker)) return false;
			MotionInbetweening::CancelMannyInference();
		}
		if (MotionInbetweening::IsMannyInferenceRunning()) return false;
		const auto Result = MotionInbetweening::GetLastInferenceResult();
		TSharedPtr<FJsonObject> Receipt;
		if (Test->TestTrue(TEXT("Pipeline writes a receipt"), ReadJson(Result.OutputDirectory / TEXT("pipeline_result.json"), Receipt)))
		{
			Test->TestEqual(TEXT("Receipt stage matches final state"), Receipt->GetStringField(TEXT("stage")), Result.Stage);
			Test->TestEqual(TEXT("Receipt identifies this request"), Receipt->GetStringField(TEXT("request_id")), Result.RequestId);
			Test->TestEqual(TEXT("Receipt success matches final state"), Receipt->GetBoolField(TEXT("success")), Result.bSuccess);
		}
		Test->TestFalse(TEXT("Output worker finishes with pipeline"), MotionInbetweening::IsMannyOutputMappingRunning());
		Test->AddInfo(FString::Printf(TEXT("Inference phase %d: %s | %s"), Phase, *Result.Stage, *Result.OutputDirectory));
		if (Phase == 0)
		{
			if (!Test->TestTrue(*FString::Printf(TEXT("Inference creates an asset: %s"), *Result.Error), Result.bSuccess)) return true;
			Test->TestEqual(TEXT("Output includes context + transition + target"), Result.SampleCount, 31);
			UAnimSequence* Animation = LoadObject<UAnimSequence>(nullptr, *Result.AssetPath);
			if (Test->TestNotNull(TEXT("Generated animation loads"), Animation))
			{
				const auto* Model = Animation->GetDataModel();
				if (Test->TestNotNull(TEXT("Animation data exists"), Model))
				{
					Test->TestEqual(TEXT("31 keys reach the asset"), Model->GetNumberOfKeys(), 31);
					Test->TestEqual(TEXT("All Manny tracks reach the asset"), Model->GetNumBoneTracks(), 89);
				}
			}
			auto Bad = Options; Bad.InferenceScript.Reset();
			FString Directory, Error;
			Test->TestFalse(TEXT("Missing backend is rejected after prior success"), MotionInbetweening::StartMannyInference(Input, Input, Bad, Directory, Error));
			const auto Failed = MotionInbetweening::GetLastInferenceResult();
			Test->TestFalse(TEXT("Failed start clears previous success"), Failed.bSuccess);
			Test->TestTrue(TEXT("Failed start clears previous asset"), Failed.AssetPath.IsEmpty());
			Test->TestEqual(TEXT("Failed start clears previous count"), Failed.SampleCount, 0);
		}
		else
		{
			Test->TestFalse(TEXT("Failed/cancelled job is not successful"), Result.bSuccess);
			Test->TestTrue(TEXT("Failed/cancelled job does not report an asset"), Result.AssetPath.IsEmpty());
			Test->TestEqual(TEXT("Expected terminal stage"), Result.Stage, Phase == 2 ? FString(TEXT("cancelled")) : FString(TEXT("failed")));
			if (Phase == 1) Test->TestTrue(TEXT("Backend exit error is retained"), Result.Error.Contains(TEXT("code 7")));
			if (Phase == 3) Test->TestTrue(TEXT("Timeout is explained"), Result.Error.Contains(TEXT("timed out")));
			if (Phase >= 2)
			{
				TSharedPtr<FJsonObject> Marker;
				if (Test->TestTrue(TEXT("Backend really started before cancellation/timeout"), ReadJson(Result.OutputDirectory / TEXT("test_backend_started.json"), Marker)))
				{
					FProcHandle Child = FPlatformProcess::OpenProcess(static_cast<uint32>(Marker->GetNumberField(TEXT("pid"))));
					Test->TestFalse(TEXT("Termination also stops backend child"), Child.IsValid() && FPlatformProcess::IsProcRunning(Child));
					if (Child.IsValid()) FPlatformProcess::CloseProc(Child);
				}
			}
		}
		if (++Phase == 4) return true;
		TSharedRef<FJsonObject> Config = MakeShared<FJsonObject>();
		Config->SetStringField(TEXT("test_mode"), Phase == 1 ? TEXT("failure") : TEXT("sleep"));
		if (!Test->TestTrue(TEXT("Write backend test configuration"), WriteJson(Options.ModelConfigFile, Config))) return true;
		Options.TimeoutSeconds = Phase == 3 ? 3 : 30;
		if (Phase == 1 || Phase == 3)
			Test->AddExpectedError(TEXT("Manny inference failed:"), EAutomationExpectedErrorFlags::Contains, 1);
		FString Directory, Error;
		if (!Test->TestTrue(TEXT("Start next inference case"), MotionInbetweening::StartMannyInference(Input, Input, Options, Directory, Error))) return true;
		Deadline = FPlatformTime::Seconds() + 45;
		return false;
	}
private:
	FAutomationTestBase* Test;
	FString Input;
	MotionInbetweening::FInferenceOptions Options;
	double Deadline;
	int32 Phase = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMannyInferenceTest, "MotionInbetweening.MannyInference",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMannyInferenceTest::RunTest(const FString& Parameters)
{
	const FString Plugin = IPluginManager::Get().FindPlugin(TEXT("MotionInbetweeningHello"))->GetBaseDir();
	TSharedPtr<FJsonObject> Fixture, Calibration;
	if (!TestTrue(TEXT("Read known-pose test fixture"), ReadJson(Plugin / TEXT("Tests/Fixtures/context_model_output.json"), Fixture))
		|| !TestTrue(TEXT("Read joint calibration"), ReadJson(FPaths::ProjectDir() / TEXT("Tools/MannyLafanMapping/rest_pose_corrections_22_prototype.json"), Calibration))) return false;
	TSharedRef<FJsonObject> Input = MakeShared<FJsonObject>();
	Input->SetStringField(TEXT("schema"), TEXT("motion_inbetweening.manny_input.v1"));
	Input->SetStringField(TEXT("position_space"), TEXT("lafan_start_centered"));
	Input->SetNumberField(TEXT("sample_rate_hz"), 30);
	Input->SetNumberField(TEXT("sample_count"), 31);
	Input->SetNumberField(TEXT("context_len"), 10);
	Input->SetBoolField(TEXT("normalization_applied"), false);
	Input->SetArrayField(TEXT("vectors_tx135"), Fixture->GetArrayField(TEXT("predictions_tx135")));
	Input->SetArrayField(TEXT("joint_order"), Calibration->GetArrayField(TEXT("joint_order")));
	Input->SetArrayField(TEXT("parents"), Calibration->GetArrayField(TEXT("parents")));
	TSharedRef<FJsonObject> Alignment = MakeShared<FJsonObject>();
	Alignment->SetArrayField(TEXT("position_offset_xz"), Fixture->GetArrayField(TEXT("heading_position_offset_xz")));
	Alignment->SetArrayField(TEXT("rotation_offset"), Fixture->GetArrayField(TEXT("heading_rotation_offset")));
	Input->SetObjectField(TEXT("alignment"), Alignment);
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("MotionInbetweening/InferenceTests") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	if (!TestTrue(TEXT("Create test folder"), IFileManager::Get().MakeDirectory(*Root, true))) return false;
	const FString InputFile = Root / TEXT("model input.json");
	auto Options = MotionInbetweening::GetInferenceOptions();
	Options.InferencePythonExecutable = Options.MappingPythonExecutable;
	Options.InferenceScript = Plugin / TEXT("Tests/Fixtures/inference_fixture_backend.py");
	Options.ModelConfigFile = Root / TEXT("model config.json");
	Options.StatisticsFile.Reset(); Options.TransitionFrames = 20; Options.TimeoutSeconds = 30; Options.bOpenAsset = false;
	if (!TestTrue(TEXT("Write input fixture"), WriteJson(InputFile, Input))
		|| !TestTrue(TEXT("Write fixture config"), WriteJson(Options.ModelConfigFile, MakeShared<FJsonObject>()))) return false;
	FString Directory, Error;
	if (!TestTrue(TEXT("Start complete inference pipeline"), MotionInbetweening::StartMannyInference(InputFile, InputFile, Options, Directory, Error)))
	{
		AddError(Error); return false;
	}
	const FString RequestId = MotionInbetweening::GetLastInferenceResult().RequestId;
	TestFalse(TEXT("Reject concurrent inference"), MotionInbetweening::StartMannyInference(InputFile, InputFile, Options, Directory, Error));
	TestEqual(TEXT("Rejected start preserves active job"), MotionInbetweening::GetLastInferenceResult().RequestId, RequestId);
	TestFalse(TEXT("Reserve output importer during inference"), MotionInbetweening::StartMannyOutputMapping(InputFile, MotionInbetweening::GetOutputMappingOptions(), Directory, Error));
	ADD_LATENT_AUTOMATION_COMMAND(FCheckInferencePipeline(this, InputFile, Options));
	return true;
}

#endif
