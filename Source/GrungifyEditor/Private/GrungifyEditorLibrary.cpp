// Copyright Fallen Signal Studios. All Rights Reserved.

#include "GrungifyEditorLibrary.h"

#include "Editor.h"
#include "GrungifyLibraryBuilder.h"
#include "GrungifyQuickStart.h"
#include "GrungifyReferenceScene.h"

bool UGrungifyEditorLibrary::BuildReferenceScene(FString& OutMessage)
{
	return FGrungifyReferenceScene::Build(OutMessage);
}

bool UGrungifyEditorLibrary::BuildMaterialLibrary(bool bOverwrite, FString& OutMessage)
{
	return FGrungifyLibraryBuilder::Build(bOverwrite, OutMessage);
}

AGrungifyWeatheringZone* UGrungifyEditorLibrary::WeatherActors(const TArray<AActor*>& Actors, bool bPreview, FString& OutMessage)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	return FGrungifyQuickStart::WeatherActors(World, Actors, bPreview, OutMessage);
}
