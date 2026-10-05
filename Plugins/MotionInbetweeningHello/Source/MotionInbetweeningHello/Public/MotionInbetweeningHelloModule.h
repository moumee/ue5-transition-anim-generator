#pragma once

#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogMotionInbetweeningHello, Log, All);

class FMotionInbetweeningHelloModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void ExecuteTestCommand();
	void ExecuteGenerateAnimationCommand();
	void ExecuteExportMannyInputCommand();
	void ExecuteImportMannyOutputCommand();
	void ExecuteMappingSettingsCommand();
};
