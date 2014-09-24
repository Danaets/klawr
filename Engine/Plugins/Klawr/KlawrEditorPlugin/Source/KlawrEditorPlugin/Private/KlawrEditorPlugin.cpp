//-------------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2014 Vadim Macagon
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//-------------------------------------------------------------------------------

#include "KlawrEditorPluginPrivatePCH.h"
#include "KlawrBlueprint.h"
#include "KlawrBlueprintCompiler.h"
#include "KismetCompilerModule.h"
#include "AssetTypeActions_KlawrBlueprint.h"
#include "IKlawrRuntimePlugin.h"
#include "KlawrGameProjectBuilder.h"
#include "KlawrScriptsReloader.h"

DEFINE_LOG_CATEGORY(LogKlawrEditorPlugin);

namespace Klawr {

class FEditorPlugin : public IKlawrEditorPlugin
{
private:
	TArray<TSharedRef<IAssetTypeActions>> RegisteredAssetTypeActions;
	int PIEAppDomainID;
	// true when the PIE app domain needs to be destroyed
	bool bPIEAppDomainDestructionPending;
	bool bPIEGarbageCollected;
	bool bRegisteredForOnLevelActorListChanged;
	bool bRegisteredForPostGarbageCollect;

private:
	/** Called by the Blueprint compiler. */
	FReply CompileBlueprint(
		UBlueprint* Blueprint, const FKismetCompilerOptions& CompileOptions,
		FCompilerResultsLog& Results, TArray<UObject*>* ObjLoaded
	)
	{
		if (auto KlawrBlueprint = Cast<UKlawrBlueprint>(Blueprint))
		{
			FBlueprintCompiler Compiler(KlawrBlueprint, Results, CompileOptions, ObjLoaded);
			Compiler.Compile();
			check(Compiler.NewClass);
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	void RegisterAssetTypeAction(IAssetTools& AssetTools, TSharedRef<IAssetTypeActions> Action)
	{
		AssetTools.RegisterAssetTypeActions(Action);
		RegisteredAssetTypeActions.Add(Action);
	}

	void RegisterOnLevelActorListChanged()
	{
		if (!bRegisteredForOnLevelActorListChanged)
		{
			check(GEngine);
			GEngine->OnLevelActorListChanged().AddRaw(this, &FEditorPlugin::OnLevelActorListChanged);
			bRegisteredForOnLevelActorListChanged = true;
		}
	}

	void UnregisterOnLevelActorListChanged()
	{
		if (bRegisteredForOnLevelActorListChanged)
		{
			check(GEngine);
			GEngine->OnLevelActorListChanged().RemoveAll(this);
			bRegisteredForOnLevelActorListChanged = false;
		}
	}

	void RegisterPostGarbageCollect()
	{
		if (!bRegisteredForPostGarbageCollect)
		{
			FCoreDelegates::PostGarbageCollect.AddRaw(this, &FEditorPlugin::OnPostGarbageCollect);
			bRegisteredForPostGarbageCollect = true;
		}
	}

	void UnregisterPostGarbageCollect()
	{
		if (bRegisteredForPostGarbageCollect)
		{
			FCoreDelegates::PostGarbageCollect.RemoveAll(this);
			bRegisteredForPostGarbageCollect = false;
		}
	}

	void OnBeginPIE(const bool bIsSimulating)
	{
		UE_LOG(LogKlawrEditorPlugin, Display, TEXT("Creating a new app domain for PIE."));
		if (ensure(PIEAppDomainID == 0))
		{
			auto& Runtime = IKlawrRuntimePlugin::Get();
			if (Runtime.CreateAppDomain(PIEAppDomainID))
			{
				Runtime.SetPIEAppDomainID(PIEAppDomainID);
			}
			else
			{
				UE_LOG(LogKlawrEditorPlugin, Error, TEXT("Failed to create PIE app domain!"));
			}
		}
	}

	void OnEndPIE(const bool bIsSimulating)
	{
		if (ensure(PIEAppDomainID != 0))
		{
			// At this point it's too early to destroy the PIE app domain because actors in the
			// PIE world haven't even been notified play has ended, so they haven't had a chance
			// to release references to any managed objects. 
			RegisterOnLevelActorListChanged();
			bPIEAppDomainDestructionPending = true;
			bPIEGarbageCollected = false;
		}
	}

	void OnLevelActorListChanged()
	{
		if (bPIEAppDomainDestructionPending && ensure(PIEAppDomainID != 0))
		{
			// At this point actors have been notified play has ended, and have unregistered their
			// components. However, neither the actors in the PIE world, nor their components have
			// been destroyed yet, that will only happen when the garbage collector runs.
			UnregisterOnLevelActorListChanged();
			RegisterPostGarbageCollect();
			// TODO: run the .NET garbage collector to ensure any unused references to native 
			// UObjects are collected before the UE garbage collector runs
			bPIEGarbageCollected = true;
		}
	}

	void OnPostGarbageCollect()
	{
		if (bPIEGarbageCollected && ensure(PIEAppDomainID != 0))
		{
			// At this point all actors and components in the PIE world should've been destroyed,
			// along with any other UObjects.
			// FIXME: Unfortunately, if there are any circular references between native UObjects 
			// and managed code then the UE garbage collector will not be able to collect all the
			// native UObjects in the PIE world, in which case UnrealEd will display an error...
			// So, I need to figure out a way to deal with circular references.
			check(bPIEAppDomainDestructionPending);

			UnregisterPostGarbageCollect();

			UE_LOG(LogKlawrEditorPlugin, Display, TEXT("Unloading PIE app domain."));
			auto& Runtime = IKlawrRuntimePlugin::Get();
			if (!Runtime.DestroyAppDomain(PIEAppDomainID))
			{
				UE_LOG(
					LogKlawrEditorPlugin, Warning, TEXT("Failed to unload PIE app domain!")
				);
			}
			PIEAppDomainID = 0;
			Runtime.SetPIEAppDomainID(PIEAppDomainID);
			bPIEAppDomainDestructionPending = false;
		}
	}

public:
	
	FEditorPlugin()
		: PIEAppDomainID(0)
		, bPIEAppDomainDestructionPending(false)
		, bPIEGarbageCollected(false)
		, bRegisteredForOnLevelActorListChanged(false)
		, bRegisteredForPostGarbageCollect(false)
	{
	}

public: // IModuleInterface interface
	
	virtual void StartupModule() override
	{
		// check if game scripts assembly exists, if not build it
		if (!FPaths::FileExists(FGameProjectBuilder::GetProjectAssemblyFilename()))
		{
			// FIXME: this shouldn't display any dialogs or anything, we're still loading modules
			// (though this seems to work fine for now)
			if (!FGameProjectBuilder::BuildProject(GWarn) || 
				!FPaths::FileExists(FGameProjectBuilder::GetProjectAssemblyFilename()))
			{
				UE_LOG(LogKlawrEditorPlugin, Error, TEXT("Failed to build scripts assembly."));
				return;
			}
		}

		IKlawrRuntimePlugin::Get().CreatePrimaryAppDomain();
				
		// register asset types
		auto& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		AssetTools.RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_KlawrBlueprint));
		
		// register Blueprint compiler
		auto& CompilerModule = FModuleManager::LoadModuleChecked<IKismetCompilerInterface>("KismetCompiler");
		FBlueprintCompileDelegate CompileDelegate;
		CompileDelegate.BindRaw(this, &FEditorPlugin::CompileBlueprint);
		CompilerModule.GetCompilers().Add(CompileDelegate);

		FEditorDelegates::BeginPIE.AddRaw(this, &FEditorPlugin::OnBeginPIE);
		FEditorDelegates::EndPIE.AddRaw(this, &FEditorPlugin::OnEndPIE);
		
		
		FScriptsReloader::Startup();
		FScriptsReloader::Get().Enable();
	}

	virtual void ShutdownModule() override
	{
		FScriptsReloader::Shutdown();

		FEditorDelegates::BeginPIE.RemoveAll(this);
		FEditorDelegates::EndPIE.RemoveAll(this);
		
		check(!bRegisteredForOnLevelActorListChanged);
		check(!bRegisteredForPostGarbageCollect);

		// at this point the editor may have already unloaded the AssetTools module, 
		// in that case there's no need to unregister the previously registered asset types
		auto AssetToolsModule = FModuleManager::GetModulePtr<FAssetToolsModule>("AssetTools");
		if (AssetToolsModule)
		{
			auto& AssetTools = AssetToolsModule->Get();
			for (auto Action : RegisteredAssetTypeActions)
			{
				AssetTools.UnregisterAssetTypeActions(Action);
			}
		}

		IKlawrRuntimePlugin::Get().DestroyPrimaryAppDomain();
	}
};

} // namespace Klawr

IMPLEMENT_MODULE(Klawr::FEditorPlugin, KlawrEditorPlugin)
