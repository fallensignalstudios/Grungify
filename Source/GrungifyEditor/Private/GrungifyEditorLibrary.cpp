#include "GrungifyEditorLibrary.h"

#include "GrungifyLibraryBuilder.h"
#include "GrungifyReferenceScene.h"

bool UGrungifyEditorLibrary::BuildReferenceScene(FString& OutMessage)
{
	return FGrungifyReferenceScene::Build(OutMessage);
}

bool UGrungifyEditorLibrary::BuildMaterialLibrary(bool bOverwrite, FString& OutMessage)
{
	return FGrungifyLibraryBuilder::Build(bOverwrite, OutMessage);
}
