// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyCommandlet.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGrungifyCommandletBakeAdmissionTest, "Grungify.Commandlet.BakeRequiresSaveBeforeMapLoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGrungifyCommandletBakeAdmissionTest::RunTest(const FString& Parameters)
{
	UWorld* OriginalWorld = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	const bool bOriginalDirty = OriginalWorld && OriginalWorld->GetPackage()->IsDirty();
	const FString ReportDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Grungify"),
		TEXT("BakeAdmission_") + FGuid::NewGuid().ToString(EGuidFormats::Digits));
	UGrungifyCommandlet* Commandlet = NewObject<UGrungifyCommandlet>();
	AddExpectedError(TEXT("Bake requires -Save"), EAutomationExpectedErrorFlags::Contains, 1);
	// A deliberately nonexistent map distinguishes admission from the later map lookup/load path.
	const int32 Exit = Commandlet->Main(FString::Printf(
		TEXT("-Map=/Game/Grungify/NonexistentBakeAdmissionMap -Operation=bAkE -Report=\"%s\""), *ReportDir));
	TestEqual(TEXT("Bake without Save is a usage error"), Exit, 2);
	TestTrue(TEXT("Rejection creates no report folder"), !IFileManager::Get().DirectoryExists(*ReportDir));
	TestTrue(TEXT("Rejection does not replace the editor world"), !GEditor || GEditor->GetEditorWorldContext().World() == OriginalWorld);
	TestTrue(TEXT("Rejection does not change the current map dirty flag"), !OriginalWorld || OriginalWorld->GetPackage()->IsDirty() == bOriginalDirty);
	return true;
}

#endif
