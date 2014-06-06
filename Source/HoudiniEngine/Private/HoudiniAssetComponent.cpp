/*
 * PROPRIETARY INFORMATION.  This software is proprietary to
 * Side Effects Software Inc., and is not to be reproduced,
 * transmitted, or disclosed in any way without written permission.
 *
 * Produced by:
 *      Damian Campeanu
 *      Side Effects Software Inc
 *      123 Front Street West, Suite 1401
 *      Toronto, Ontario
 *      Canada   M5J 2M2
 *      416-504-9876
 *
 */

#include "HoudiniEnginePrivatePCH.h"
#include <vector>
#include <string>
#include <stdint.h>


UHoudiniAssetComponent::UHoudiniAssetComponent(const FPostConstructInitializeProperties& PCIP) : 
	Super(PCIP),
	HoudiniAssetInstance(nullptr),
	bIsNativeComponent(false),
	Material(nullptr)
{
	// Create generic bounding volume.
	HoudiniMeshSphereBounds = FBoxSphereBounds(FBox(-FVector(1.0f, 1.0f, 1.0f) * HALF_WORLD_MAX, FVector(1.0f, 1.0f, 1.0f) * HALF_WORLD_MAX));

	// This component can tick.
	PrimaryComponentTick.bCanEverTick = false;

	// Zero scratch space.
	FMemory::Memset(ScratchSpaceBuffer, 0x0, HOUDINIENGINE_ASSET_SCRATCHSPACE_SIZE);
}


void 
UHoudiniAssetComponent::SetNative(bool InbIsNativeComponent)
{
	bIsNativeComponent = InbIsNativeComponent;
}


void 
UHoudiniAssetComponent::OnRep_HoudiniAsset(UHoudiniAsset* OldHoudiniAsset)
{
	HOUDINI_LOG_MESSAGE(TEXT("OnRep_HoudiniAsset, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);

	// Only do stuff if this actually changed from the last local value.
	if(OldHoudiniAsset != HoudiniAsset)
	{
		// We have to force a call to SetHoudiniAsset with a new HoudiniAsset.
		UHoudiniAsset* NewHoudiniAsset = HoudiniAsset;
		HoudiniAsset = nullptr;

		SetHoudiniAsset(NewHoudiniAsset);
	}
}


void 
UHoudiniAssetComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHoudiniAssetComponent, HoudiniAsset);
}


void 
UHoudiniAssetComponent::NotifyAssetInstanceCookingFailed(UHoudiniAssetInstance* InHoudiniAssetInstance, HAPI_Result Result)
{
	HOUDINI_LOG_ERROR(TEXT("Failed cooking for asset = %0x0.8p"), InHoudiniAssetInstance);
}


void 
UHoudiniAssetComponent::NotifyAssetInstanceCookingFinished(UHoudiniAssetInstance* InHoudiniAssetInstance, HAPI_AssetId AssetId, const std::string& AssetInternalName)
{
	// Make sure we received instance we requested to be cooked.
	if(InHoudiniAssetInstance != HoudiniAssetInstance)
	{
		HOUDINI_LOG_ERROR(TEXT("Received mismatched asset instance, Owned = %0x0.8p, Recieved = %0x0.8p"), HoudiniAssetInstance, InHoudiniAssetInstance);
		return;
	}

	if(HoudiniAssetInstance->IsInitialized())
	{
		// We can recreate geometry.
		if(!FHoudiniEngineUtils::GetAssetGeometry(HoudiniAssetInstance->GetAssetId(), HoudiniMeshTris, HoudiniMeshSphereBounds))
		{
			HOUDINI_LOG_MESSAGE(TEXT("Preview actor, failed geometry extraction."));
		}
	}

	// Reset cooked status.
	HoudiniAssetInstance->SetCooked(false);

	AHoudiniAssetActor* HoudiniAssetActor = CastChecked<AHoudiniAssetActor>(GetOwner());
	if(!HoudiniAssetActor->IsUsedForPreview())
	{
		// Since this is not a preview actor, we need to patch component RTTI to reflect properties for this asset.
		ReplaceClassInformation();

		// Need to send this to render thread at some point.
		MarkRenderStateDirty();

		// Update physics representation right away.
		RecreatePhysicsState();

		// Click actor to update details information.
		//HoudiniAssetActor->OnClicked.Broadcast();
		//DispatchOnClicked();
	}
}


bool 
UHoudiniAssetComponent::SetHoudiniAsset(UHoudiniAsset* NewHoudiniAsset)
{
	HOUDINI_LOG_MESSAGE(TEXT("Setting asset, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);

	// Do nothing if we are already using the supplied Houdini asset.
	if(NewHoudiniAsset == HoudiniAsset)
	{
		return false;
	}

	// Don't allow changing Houdini assets if "static" and registered.
	AActor* Owner = GetOwner();
	if(Mobility == EComponentMobility::Static && IsRegistered() && Owner != NULL)
	{
		FMessageLog("PIE").Warning(FText::Format(LOCTEXT("SetHoudiniAssetOnStatic", "Calling SetHoudiniAsset on '{0}' but Mobility is Static."), FText::FromString(GetPathName(this))));
		return false;
	}

	// Store the new asset.
	HoudiniAsset = NewHoudiniAsset;

	AHoudiniAssetActor* HoudiniAssetActor = CastChecked<AHoudiniAssetActor>(GetOwner());
	if(HoudiniAssetActor && HoudiniAssetActor->IsUsedForPreview())
	{
		// If this is a preview actor, check if we need to cook an asset for thumbnail.
		if(HoudiniAsset)
		{
			if(!HoudiniAssetInstance)
			{
				// Create asset instance and cook it.
				HoudiniAssetInstance = NewObject<UHoudiniAssetInstance>();
				HoudiniAssetInstance->SetHoudiniAsset(HoudiniAsset);

				// Start asynchronous task to perform the cooking from the referenced asset instance.
				FHoudiniTaskCookAssetInstance* HoudiniTaskCookAssetInstance = new FHoudiniTaskCookAssetInstance(this, HoudiniAssetInstance);

				// Create a new thread to execute our runnable.
				FRunnableThread* Thread = FRunnableThread::Create(HoudiniTaskCookAssetInstance, TEXT("HoudiniTaskCookAssetInstance"), true, true, 0, TPri_Normal);
			}
		}
	}

	// Need to send this to render thread at some point.
	MarkRenderStateDirty();

	// Update physics representation right away.
	RecreatePhysicsState();

	// Notify the streaming system. Don't use Update(), because this may be the first time the mesh has been set
	// and the component may have to be added to the streaming system for the first time.
	//GStreamingManager->NotifyPrimitiveAttached(this, DPT_Spawned);

	// Since we have new asset, we need to update bounds.
	UpdateBounds();
	return true;
}


FBoxSphereBounds 
UHoudiniAssetComponent::CalcBounds(const FTransform & LocalToWorld) const
{
	return HoudiniMeshSphereBounds;
}


int32 
UHoudiniAssetComponent::GetNumMaterials() const
{
	return 1;
}


FPrimitiveSceneProxy* 
UHoudiniAssetComponent::CreateSceneProxy()
{
	FPrimitiveSceneProxy* Proxy = nullptr;
	
	if(HoudiniMeshTris.Num() > 0)
	{
		Proxy = new FHoudiniMeshSceneProxy(this);
	}
	
	return Proxy;
}


/*
void 
UHoudiniAssetComponent::BeginDestroy()
{
	Super::BeginDestroy();
	HOUDINI_LOG_MESSAGE(TEXT("Starting destruction, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}

void 
UHoudiniAssetComponent::FinishDestroy()
{
	Super::FinishDestroy();
	HOUDINI_LOG_MESSAGE(TEXT("Finishing destruction, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}
*/


void 
UHoudiniAssetComponent::ReplaceClassInformation()
{	
	// Grab default object of UClass.
	UObject* ClassOfUClass = UClass::StaticClass()->GetDefaultObject();

	// Grab class of UHoudiniAssetComponent.
	UClass* ClassOfUHoudiniAssetComponent = UHoudiniAssetComponent::StaticClass();

	// Construct unique name for this class.
	FString PatchedClassName = FString::Printf(TEXT("%s_%d"), *GetClass()->GetName(), HoudiniAssetInstance->GetAssetId());

	// Create new class instance.
	static const EObjectFlags PatchedClassFlags = RF_Public | RF_Standalone | RF_Transient | RF_Native | RF_RootSet;
	//UClass* PatchedClass = ConstructObject<UClass>(UClass::StaticClass(), this->GetOutermost(), FName(*this->GetName()), PatchedClassFlags, ClassOfUHoudiniAssetComponent, true);
	UClass* PatchedClass = ConstructObject<UClass>(UClass::StaticClass(), this->GetOutermost(), FName(*PatchedClassName), PatchedClassFlags, ClassOfUHoudiniAssetComponent, true);
	PatchedClass->ClassFlags = UHoudiniAssetComponent::StaticClassFlags;
	PatchedClass->ClassCastFlags = UHoudiniAssetComponent::StaticClassCastFlags();
	PatchedClass->ClassConfigName = UHoudiniAssetComponent::StaticConfigName();
	PatchedClass->ClassDefaultObject = this->GetClass()->ClassDefaultObject;
	PatchedClass->ClassConstructor = ClassOfUHoudiniAssetComponent->ClassConstructor;
	PatchedClass->ClassAddReferencedObjects = ClassOfUHoudiniAssetComponent->ClassAddReferencedObjects;
	PatchedClass->MinAlignment = ClassOfUHoudiniAssetComponent->MinAlignment;
	PatchedClass->PropertiesSize = ClassOfUHoudiniAssetComponent->PropertiesSize;
	PatchedClass->SetSuperStruct(ClassOfUHoudiniAssetComponent->GetSuperStruct());
	PatchedClass->ClassReps = ClassOfUHoudiniAssetComponent->ClassReps;
	PatchedClass->NetFields = ClassOfUHoudiniAssetComponent->NetFields;
	PatchedClass->ReferenceTokenStream = ClassOfUHoudiniAssetComponent->ReferenceTokenStream;
	PatchedClass->NativeFunctionLookupTable = ClassOfUHoudiniAssetComponent->NativeFunctionLookupTable;

	// Before patching, grab the previous class.
	UClass* PreviousClass = this->GetClass();

	// If RTTI has been previously patched, we need to restore the data.
	if(PreviousClass != UHoudiniAssetComponent::StaticClass())
	{
		// Not yet implemented.
	}

	// Insert necessary properties.
	ReplaceClassProperties(PatchedClass);

	// Patch class information.
	{
		UClass** Address = (UClass**) this;
		UClass** EndAddress = (UClass**) this + sizeof(UHoudiniAssetComponent);

		//Grab class of UHoudiniAssetComponent.
		UClass* HoudiniAssetComponentClass = GetClass();

		while(*Address != HoudiniAssetComponentClass && Address < EndAddress)
		{
			Address++;
		}

		if(*Address == HoudiniAssetComponentClass)
		{
			*Address = PatchedClass;
		}
		else
		{
			HOUDINI_LOG_MESSAGE(TEXT("Failed class patching, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
		}
	}
}


bool 
UHoudiniAssetComponent::ReplaceClassProperties(UClass* ClassInstance)
{
	HAPI_AssetId AssetId = HoudiniAssetInstance->GetAssetId();
	HAPI_Result Result = HAPI_RESULT_SUCCESS;
	HAPI_AssetInfo AssetInfo;
	HAPI_NodeInfo NodeInfo;

	std::vector<HAPI_ParmInfo> ParmInfo;
	std::vector<int> ParmValuesIntegers;
	std::vector<float> ParmValuesFloats;
	std::vector<HAPI_StringHandle> ParmStringFloats;
	std::vector<char> ParmName;
	std::vector<char> ParmLabel;

	HOUDINI_CHECK_ERROR_RETURN(HAPI_GetAssetInfo(AssetId, &AssetInfo), false);
	HOUDINI_CHECK_ERROR_RETURN(HAPI_GetNodeInfo(AssetInfo.nodeId, &NodeInfo), false);

	// Retrieve parameters.
	ParmInfo.reserve(NodeInfo.parmCount);
	HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParameters(AssetInfo.nodeId, &ParmInfo[0], 0, NodeInfo.parmCount), false);

	// Retrieve integer values for this asset.
	ParmValuesIntegers.reserve(NodeInfo.parmIntValueCount);
	if(NodeInfo.parmIntValueCount > 0)
	{
		HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParmIntValues(AssetInfo.nodeId, &ParmValuesIntegers[0], 0, NodeInfo.parmIntValueCount), false);
	}

	// Retrieve float values for this asset.
	ParmValuesFloats.reserve(NodeInfo.parmFloatValueCount);
	if(NodeInfo.parmFloatValueCount > 0)
	{
		HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParmFloatValues(AssetInfo.nodeId, &ParmValuesFloats[0], 0, NodeInfo.parmFloatValueCount), false);
	}

	// Retrieve string values for this asset.
	ParmStringFloats.reserve(NodeInfo.parmStringValueCount);
	if(NodeInfo.parmStringValueCount > 0)
	{
		HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParmStringValues(AssetInfo.nodeId, true, &ParmStringFloats[0], 0, NodeInfo.parmStringValueCount), false);
	}

	// We need to insert new properties and new children in the beginning of single link list.
	// This way properties and children from the original class can be reused and will not have
	// their next pointers altered.
	UProperty* PropertyFirst = nullptr;
	UProperty* PropertyLast = PropertyFirst;

	UField* ChildFirst = nullptr;
	UField* ChildLast = ChildFirst;

	uint32 ValuesOffsetStart = offsetof(UHoudiniAssetComponent, ScratchSpaceBuffer);
	uint32 ValuesOffsetEnd = ValuesOffsetStart;

	for(int idx = 0; idx < NodeInfo.parmCount; ++idx)
	{
		// Retrieve param info at this index.
		const HAPI_ParmInfo& ParmInfoIter = ParmInfo[idx];
			
		// Skip unsupported param types for now.
		switch(ParmInfoIter.type)
		{
			case HAPI_PARMTYPE_INT:
			case HAPI_PARMTYPE_FLOAT:
			case HAPI_PARMTYPE_TOGGLE:
			case HAPI_PARMTYPE_COLOR:
			case HAPI_PARMTYPE_STRING:
			{
				break;
			}

			default:
			{
				// Just ignore unsupported types for now.
				continue;
			}
		}

		// Retrieve length of this parameter's name.
		int32 ParmNameLength = 0;
		HOUDINI_CHECK_ERROR(HAPI_GetStringBufLength(ParmInfoIter.nameSH, &ParmNameLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving length of this parameter's name, continue onto next parameter.
			continue;
		}

		// If length of name of this parameter is zero, continue onto next parameter.
		if(!ParmNameLength)
		{
			continue;
		}

		// Retrieve name for this parameter.
		ParmName.reserve(ParmNameLength);
		HOUDINI_CHECK_ERROR(HAPI_GetString(ParmInfoIter.nameSH, &ParmName[0], ParmNameLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving the name of this parameter, continue onto next parameter.
			continue;
		}

		// We need to convert name to a string Unreal understands.
		FUTF8ToTCHAR ParamNameStringConverter(&ParmName[0]);
		FName ParmNameConverted = ParamNameStringConverter.Get();

		// Retrieve length of this parameter's label.
		int32 ParmLabelLength = 0;
		HOUDINI_CHECK_ERROR(HAPI_GetStringBufLength(ParmInfoIter.labelSH, &ParmLabelLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving length of this parameter's label, continue onto next parameter.
			continue;
		}

		// Retrieve label for this parameter.
		ParmLabel.reserve(ParmLabelLength);
		HOUDINI_CHECK_ERROR(HAPI_GetString(ParmInfoIter.labelSH, &ParmLabel[0], ParmLabelLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving the label of this parameter, continue onto next parameter.
			continue;
		}

		// We need to convert label to a string Unreal understands.
		FUTF8ToTCHAR ParamLabelStringConverter(&ParmLabel[0]);

		UProperty* Property = nullptr;
		switch(ParmInfoIter.type)
		{
			case HAPI_PARMTYPE_INT:
			{
				Property = CreatePropertyInt(ClassInstance, ParmNameConverted, ParmInfoIter.size, ParmValuesIntegers[ParmInfoIter.intValuesIndex], ValuesOffsetEnd);
				break;
			}

			case HAPI_PARMTYPE_FLOAT:
			{
				Property = CreatePropertyFloat(ClassInstance, ParmNameConverted, ParmInfoIter.size, ParmValuesFloats[ParmInfoIter.floatValuesIndex], ValuesOffsetEnd);
				break;
			}

			case HAPI_PARMTYPE_TOGGLE:
			{
				Property = CreatePropertyToggle(ClassInstance, ParmNameConverted, ParmInfoIter.size, (ParmValuesIntegers[ParmInfoIter.intValuesIndex] != 0), ValuesOffsetEnd);
				break;
			}

			case HAPI_PARMTYPE_COLOR:
			case HAPI_PARMTYPE_STRING:
			default:
			{
				break;
			}
		}

		if(!Property)
		{
			// Unsupported type property - skip to next parameter.
			continue;
		}
		
		// Use label instead of name if it is present.
		if(ParmLabelLength)
		{
			Property->SetMetaData(TEXT("DisplayName"), ParamLabelStringConverter.Get());
		}

		// Set UI and physical ranges, if present.
		if(ParmInfoIter.hasUIMin)
		{
			Property->SetMetaData(TEXT("UIMin"), *FString::SanitizeFloat(ParmInfoIter.UIMin));
		}

		if(ParmInfoIter.hasUIMax)
		{
			Property->SetMetaData(TEXT("UIMax"), *FString::SanitizeFloat(ParmInfoIter.UIMax));
		}

		if(ParmInfoIter.hasMin)
		{
			Property->SetMetaData(TEXT("ClampMin"), *FString::SanitizeFloat(ParmInfoIter.min));
		}
		
		if(ParmInfoIter.hasMax)
		{
			Property->SetMetaData(TEXT("ClampMax"), *FString::SanitizeFloat(ParmInfoIter.max));
		}

		// Insert this newly created property in link list of properties.
		if(!PropertyFirst)
		{
			PropertyFirst = Property;
			PropertyLast = Property;
		}
		else
		{
			PropertyLast->PropertyLinkNext = Property;
			PropertyLast = Property;
		}

		// Insert this newly created property into link list of children.
		if(!ChildFirst)
		{
			ChildFirst = Property;
			ChildLast = Property;
		}
		else
		{
			ChildLast->Next = Property;
			ChildLast = Property;
		}
	}

	if(PropertyFirst)
	{
		ClassInstance->PropertyLink = PropertyFirst;
		PropertyLast->PropertyLinkNext = GetClass()->PropertyLink;
	}

	if(ChildFirst)
	{
		ClassInstance->Children = ChildFirst;
		ChildLast->Next = GetClass()->Children;
	}

	return true;
}


UProperty* 
UHoudiniAssetComponent::CreatePropertyInt(UClass* ClassInstance, const FName& Name, int Count, int32 Value, uint32& Offset)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient | RF_Native;
	static const uint64 PropertyFlags =  UINT64_C(69793219077);

	// Construct property.
	UProperty* Property = new(ClassInstance, Name, PropertyObjectFlags) UIntProperty(FPostConstructInitializeProperties(), EC_CppProperty, Offset, 0x0);
	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniAsset"));
	Property->PropertyFlags = PropertyFlags;

	// Write property data to which it refers by offset.
	*(int*)((char*) this + Offset) = Value;

	// Increment offset for next property.
	Offset += sizeof(int);

	return Property;
}


UProperty* 
UHoudiniAssetComponent::CreatePropertyFloat(UClass* ClassInstance, const FName& Name, int Count, float Value, uint32& Offset)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient | RF_Native;
	static const uint64 PropertyFlags =  UINT64_C(69793219077);

	// Construct property.
	UProperty* Property = new(ClassInstance, Name, PropertyObjectFlags) UFloatProperty(FPostConstructInitializeProperties(), EC_CppProperty, Offset, 0x0);
	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniAsset"));
	Property->PropertyFlags = PropertyFlags;

	// Write property data to which it refers by offset.
	*(float*)((char*) this + Offset) = Value;

	// Increment offset for next property.
	Offset += sizeof(float);

	return Property;
}


UProperty* 
UHoudiniAssetComponent::CreatePropertyToggle(UClass* ClassInstance, const FName& Name, int Count, bool bValue, uint32& Offset)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient | RF_Native;
	static const uint64 PropertyFlags =  UINT64_C(69793219077);

	// Construct property.
	UProperty* Property = new(ClassInstance, Name, PropertyObjectFlags) UBoolProperty(FPostConstructInitializeProperties(), EC_CppProperty, Offset, 0x0, ~0, sizeof(bool), true);
	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniAsset"));
	Property->PropertyFlags = PropertyFlags;

	// Write property data to which it refers by offset.
	*(bool*)((char*) this + Offset) = bValue;

	// Increment offset for next property.
	Offset += sizeof(bool);

	return Property;
}


void 
UHoudiniAssetComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Retrieve property which changed.
	UProperty* Property = PropertyChangedEvent.Property;

	// Retrieve property category.
	static const FString CategoryHoudiniAsset = TEXT("HoudiniAsset");
	const FString& Category = Property->GetMetaData(TEXT("Category"));

	if(Category != CategoryHoudiniAsset)
	{
		// This property is not in category we are interested in, just jump out.
		Super::PostEditChangeProperty(PropertyChangedEvent);
		return;
	}

	// Retrieve offset into scratch space for this property.
	uint32 ValueOffset = Property->GetOffset_ForDebug();
	
	// Get the name of property.
	std::wstring PropertyName(*Property->GetName());
	std::string PropertyNameConverted(PropertyName.begin(), PropertyName.end());

	HAPI_AssetId AssetId = HoudiniAssetInstance->GetAssetId();
	HAPI_Result Result = HAPI_RESULT_SUCCESS;
	HAPI_AssetInfo AssetInfo;
	HAPI_ParmId ParamId;
	HAPI_ParmInfo ParamInfo;

	// Retrieve asset information.
	HOUDINI_CHECK_ERROR(HAPI_GetAssetInfo(AssetId, &AssetInfo));
	if(HAPI_RESULT_SUCCESS != Result)
	{
		// Error retrieving asset information, do not proceed.
		Super::PostEditChangeProperty(PropertyChangedEvent);
		return;
	}
	
	// Locate corresponding param.
	HOUDINI_CHECK_ERROR(HAPI_GetParmIdFromName(AssetInfo.nodeId, PropertyNameConverted.c_str(), &ParamId));
	if(HAPI_RESULT_SUCCESS != Result)
	{
		// Error locating corresponding param, do not proceed.
		Super::PostEditChangeProperty(PropertyChangedEvent);
		return;
	}

	// Get parameter information.
	HOUDINI_CHECK_ERROR(HAPI_GetParameters(AssetInfo.nodeId, &ParamInfo, ParamId, 1));
	if(HAPI_RESULT_SUCCESS != Result)
	{
		// Error retrieving param information, do not proceed.
		Super::PostEditChangeProperty(PropertyChangedEvent);
		return;
	}
	
	// Based on type, upload new values to Houdini Engine.
	if(UIntProperty::StaticClass() == Property->GetClass())
	{
		int Value = *(int*)((const char*) this + ValueOffset);
		HOUDINI_CHECK_ERROR(HAPI_SetParmIntValues(AssetInfo.nodeId, &Value, ParamInfo.intValuesIndex, ParamInfo.size));

		if(HAPI_RESULT_SUCCESS != Result)
		{
			// Error setting a parameter.
			Super::PostEditChangeProperty(PropertyChangedEvent);
			return;
		}
	}
	else if(UFloatProperty::StaticClass() == Property->GetClass())
	{
		float Value = *(float*)((const char*) this + ValueOffset);
		HOUDINI_CHECK_ERROR(HAPI_SetParmFloatValues(AssetInfo.nodeId, &Value, ParamInfo.floatValuesIndex, ParamInfo.size));

		if(HAPI_RESULT_SUCCESS != Result)
		{
			// Error setting a parameter.
			Super::PostEditChangeProperty(PropertyChangedEvent);
			return;
		}
	}
	else if(UBoolProperty::StaticClass() == Property->GetClass())
	{
		int Value = *(bool*)((const char*) this + ValueOffset);
		HOUDINI_CHECK_ERROR(HAPI_SetParmIntValues(AssetInfo.nodeId, &Value, ParamInfo.intValuesIndex, ParamInfo.size));

		if(HAPI_RESULT_SUCCESS != Result)
		{
			// Error setting a parameter.
			Super::PostEditChangeProperty(PropertyChangedEvent);
			return;
		}
	}

	// Recook synchronously as user needs to see the changes.
	HOUDINI_CHECK_ERROR(HAPI_CookAsset(AssetId, nullptr));
	if(HAPI_RESULT_SUCCESS != Result)
	{
		// Error recooking.
		Super::PostEditChangeProperty(PropertyChangedEvent);
		return;
	}

	// At this point we can recook the data.
	//FHoudiniTaskCookAssetInstance* HoudiniTaskCookAssetInstance = new FHoudiniTaskCookAssetInstance(this, HoudiniAssetInstance);

	// Create a new thread to execute our runnable.
	//FRunnableThread* Thread = FRunnableThread::Create(HoudiniTaskCookAssetInstance, TEXT("HoudiniTaskCookAssetInstance"), true, true, 0, TPri_Normal);

	if(HoudiniAssetInstance->IsInitialized())
	{
		// We can recreate geometry.
		if(!FHoudiniEngineUtils::GetAssetGeometry(HoudiniAssetInstance->GetAssetId(), HoudiniMeshTris, HoudiniMeshSphereBounds))
		{
			HOUDINI_LOG_MESSAGE(TEXT("Preview actor, failed geometry extraction."));
		}

		// Need to send this to render thread at some point.
		MarkRenderStateDirty();

		// Update physics representation right away.
		RecreatePhysicsState();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}


void 
UHoudiniAssetComponent::OnRegister()
{
	Super::OnRegister();
	HOUDINI_LOG_MESSAGE(TEXT("Registering component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);

	// Make sure we have a Houdini asset to operate with.
	if(HoudiniAsset)
	{
		// Make sure we are attached to a Houdini asset actor.
		AHoudiniAssetActor* HoudiniAssetActor = CastChecked<AHoudiniAssetActor>(GetOwner());
		if(HoudiniAssetActor)
		{
			if(bIsNativeComponent)
			{
				// This is a native component ~ belonging to a c++ actor, make sure actor is not used for preview.
				if(!HoudiniAssetActor->IsUsedForPreview())
				{
					HOUDINI_LOG_MESSAGE(TEXT("Native::OnRegister"));
					
					if(!HoudiniAssetInstance)
					{
						// Create asset instance and cook it.
						HoudiniAssetInstance = NewObject<UHoudiniAssetInstance>();
						HoudiniAssetInstance->SetHoudiniAsset(HoudiniAsset);

						// Start asynchronous task to perform the cooking from the referenced asset instance.
						FHoudiniTaskCookAssetInstance* HoudiniTaskCookAssetInstance = new FHoudiniTaskCookAssetInstance(this, HoudiniAssetInstance);

						// Create a new thread to execute our runnable.
						FRunnableThread* Thread = FRunnableThread::Create(HoudiniTaskCookAssetInstance, TEXT("HoudiniTaskCookAssetInstance"), true, true, 0, TPri_Normal);
					}
				}
			}
			else
			{
				// This is a dynamic component ~ part of blueprint.
				HOUDINI_LOG_MESSAGE(TEXT("Dynamic::OnRegister"));
			}
		}
	}
}


void 
UHoudiniAssetComponent::OnUnregister()
{
	Super::OnUnregister();
	HOUDINI_LOG_MESSAGE(TEXT("Unregistering component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}


void 
UHoudiniAssetComponent::OnComponentCreated()
{
	// This event will only be fired for native Actor and native Component.
	Super::OnComponentCreated();
	HOUDINI_LOG_MESSAGE(TEXT("Creating component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}


void 
UHoudiniAssetComponent::OnComponentDestroyed()
{
	Super::OnComponentDestroyed();
	HOUDINI_LOG_MESSAGE(TEXT("Destroying component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);	
}


void 
UHoudiniAssetComponent::GetComponentInstanceData(FComponentInstanceDataCache& Cache) const
{
	// Called before we throw away components during RerunConstructionScripts, to cache any data we wish to persist across that operation.
	Super::GetComponentInstanceData(Cache);
	HOUDINI_LOG_MESSAGE(TEXT("Requesting data for caching, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}


void 
UHoudiniAssetComponent::ApplyComponentInstanceData(const FComponentInstanceDataCache& Cache)
{
	// Called after we create new components during RerunConstructionScripts, to optionally apply any data backed up during GetComponentInstanceData.
	Super::ApplyComponentInstanceData(Cache);
	HOUDINI_LOG_MESSAGE(TEXT("Restoring data from caching, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}