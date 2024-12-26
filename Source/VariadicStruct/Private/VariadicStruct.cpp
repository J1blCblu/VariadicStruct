// Copyright 2024 Ivan Baktenkov. All Rights Reserved.

#include "VariadicStruct.h"

#include <cstddef> //offsetof()

#include "Logging/LogMacros.h"
#include "Misc/CString.h"
#include "Serialization/CustomVersion.h"
#include "StructUtilsTypes.h"
#include "UObject/CoreRedirects.h"
#include "UObject/Linker.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectBaseUtility.h"

#if WITH_ENGINE
#include "Engine/PackageMapClient.h"
#include "Engine/NetConnection.h"
#include "Engine/UserDefinedStruct.h"
#include "Net/RepLayout.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/ArchiveUObject.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#endif // WITH_ENGINE

#include UE_INLINE_GENERATED_CPP_BY_NAME(VariadicStruct)

consteval void FVariadicStructValidateInvariants()
{
	//The following requirements needs to be met in order to avoid using std::align() to access the underlying structure memory.
	static_assert(sizeof(FVariadicStruct::ScriptStruct) == 8 && alignof(FVariadicStruct) >= 8 && FVariadicStruct::BUFFER_SIZE >= alignof(FVariadicStruct));
	static_assert(std::is_standard_layout_v<FVariadicStruct> && offsetof(FVariadicStruct, StructBuffer) == 0, "FVariadicStruct::StructBuffer needs to be the first member property.");
	static_assert((FVariadicStruct::BUFFER_SIZE - sizeof(FVariadicStruct::ScriptStruct)) % alignof(FVariadicStruct) == 0 , "FVariadicStruct needs to be effectively sized.");
}

namespace
{
	struct FVariadicStructCustomVersion
	{
		//Custom version unique GUID.
		static inline constexpr FGuid Guid{ 0x64fc2696, 0x589c216a, 0x95b4a289, 0xc72589ab };

		//Version history.
		enum Type
		{
			CustomVersionAdded = 0,

			// -----<new versions can be added above this line>-----
			VersionPlusOne,
			LatestVersion = VersionPlusOne - 1
		};

	private:

		//Register our custom version at startup.
		static inline const FCustomVersionRegistration Registration{ Guid, FVariadicStructCustomVersion::LatestVersion, TEXT("VariadicStructCustomVersion") };
	};
}

FVariadicStruct::FVariadicStruct(const FVariadicStruct& InOther)
{
	InitializeAs(InOther.GetScriptStruct(), InOther.GetMemory());
}

FVariadicStruct::FVariadicStruct(FVariadicStruct&& InOther)
{
	if (InOther.ScriptStruct && !RequiresMemoryAllocation(InOther.ScriptStruct))
	{
		//Copy construct within the buffer.
		InitializeAs(InOther.ScriptStruct, InOther.GetMemory());

		//Invalidate other data.
		InOther.Reset();
	}
	else
	{
		//Take ownership.
		ResetStructData(InOther.ScriptStruct, InOther.StructMemory);

		//Reset data.
		InOther.ResetStructData();
	}
}

FVariadicStruct& FVariadicStruct::operator=(const FVariadicStruct& InOther)
{
	if (this != &InOther)
	{
		InitializeAs(InOther.GetScriptStruct(), InOther.GetMemory());
	}

	return *this;
}

FVariadicStruct& FVariadicStruct::operator=(FVariadicStruct&& InOther)
{
	if (this != &InOther)
	{
		if (InOther.ScriptStruct && !RequiresMemoryAllocation(InOther.ScriptStruct))
		{
			//Copy construct within the buffer.
			InitializeAs(InOther.ScriptStruct, InOther.GetMemory());

			//Invalidate data.
			InOther.Reset();
		}
		else
		{
			//Invalidate data and release memory.
			Reset();

			//Take ownership.
			ResetStructData(InOther.ScriptStruct, InOther.StructMemory);

			//Reset data.
			InOther.ResetStructData();
		}
	}

	return *this;
}

void FVariadicStruct::InitializeAs(const UScriptStruct* InScriptStruct, const uint8* InStructMemory /* = nullptr */)
{
	//If the existing type is valid and matches.
	if (ScriptStruct && InScriptStruct == ScriptStruct)
	{
		//Copy properties if needed.
		if (InStructMemory)
		{
			ScriptStruct->CopyScriptStruct(GetMutableMemory(), InStructMemory);
		}
		else //Otherwise, reset to default state.
		{
			ScriptStruct->ClearScriptStruct(GetMutableMemory());
		}
	}
	else //Invalid or mismatched type.
	{
		Reset();

		//Construct a new struct if needed.
		if ((ScriptStruct = InScriptStruct) != nullptr)
		{
			uint8* DestMemory = StructBuffer;
			
			//Allocate a new space if the buffer is too small.
			if (RequiresMemoryAllocation(InScriptStruct))
			{
				DestMemory = StructMemory = static_cast<uint8*>(FMemory::Malloc(InScriptStruct->GetStructureSize(), InScriptStruct->GetMinAlignment()));
			}

			//Default initialize.
			ScriptStruct->InitializeStruct(DestMemory);

			//Copy properties if needed.
			if (InStructMemory)
			{
				ScriptStruct->CopyScriptStruct(DestMemory, InStructMemory);
			}
		}
	}
}

void FVariadicStruct::Reset()
{
	if (uint8* const Memory = GetMutableMemory(); ScriptStruct && ensureAlways(Memory))
	{
		ScriptStruct->DestroyStruct(Memory);

		if (RequiresMemoryAllocation(ScriptStruct))
		{
			FMemory::Free(Memory);
		}
	}

	ResetStructData();
}

bool FVariadicStruct::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FVariadicStructCustomVersion::Guid);

	if (Ar.IsLoading())
	{
		UScriptStruct* SerializedScriptStruct = nullptr;
		Ar << SerializedScriptStruct;

		if (SerializedScriptStruct)
		{
			Ar.Preload(SerializedScriptStruct);
		}

		//Initialize only if the type changes.
		if (ScriptStruct != SerializedScriptStruct)
		{
			InitializeAs(SerializedScriptStruct);
		}

		int32 SerialSize = 0;
		Ar << SerialSize;

		//Check if the type is not missed.
		if (!ScriptStruct && SerialSize > 0)
		{
			//Step over missing data.
			Ar.Seek(Ar.Tell() + SerialSize);

			UE_LOG(LogCore, Warning, TEXT("FVariadicStruct: Failed to serialize UScriptStruct with SerialSize: %u, SerializedProperty: %s, LinkerRoot: %s."),
				   SerialSize, *GetPathNameSafe(Ar.GetSerializedProperty()), Ar.GetLinker() ? *GetPathNameSafe(Ar.GetLinker()->LinkerRoot) : TEXT("NoLinker"));
		}

		//Serialize the actual value.
		if (uint8* const MemPtr = GetMutableMemory(); ScriptStruct && ensureAlways(MemPtr))
		{
			ConstCast(ScriptStruct)->SerializeItem(Ar, MemPtr, /* Defaults */ nullptr);
		}
	}
	else if (Ar.IsSaving())
	{
#if WITH_EDITOR
		const UUserDefinedStruct* const UserDefinedStruct = Cast<UUserDefinedStruct>(ScriptStruct);
		if (UserDefinedStruct && UserDefinedStruct->Status == EUserDefinedStructureStatus::UDSS_Duplicate && UserDefinedStruct->PrimaryStruct.IsValid())
		{
			//If saving a duplicated UDS, save the primary type instead, so that the data is loaded with the original struct.
			//This is used as part of the user defined struct reinstancing logic.
			UUserDefinedStruct* PrimaryUserDefinedStruct = UserDefinedStruct->PrimaryStruct.Get();
			Ar << PrimaryUserDefinedStruct;
		}
		else
#endif
		{
			Ar << ScriptStruct;
		}

		//Reserve the buffer for SerialSize.
		const int64 SizeOffset = Ar.Tell();
		int32 SerialSize = 0;
		Ar << SerialSize;

		const int64 InitialOffset = Ar.Tell();

		//Serialize the actual value.
		if (uint8* const MemPtr = GetMutableMemory(); ScriptStruct && ensureAlways(MemPtr))
		{
			ConstCast(ScriptStruct)->SerializeItem(Ar, MemPtr, /* Defaults */ nullptr);
		}

		//Calculate the total serialized size.
		const int64 FinalOffset = Ar.Tell();
		SerialSize = IntCastChecked<int32>(FinalOffset - InitialOffset);

		//Write it to the reserved buffer.
		Ar.Seek(SizeOffset);
		Ar << SerialSize;

		//Restore the offset for further serialization.
		Ar.Seek(FinalOffset);
	}
	else if (Ar.IsCountingMemory() || Ar.IsModifyingWeakAndStrongReferences() || Ar.IsObjectReferenceCollector())
	{
		//Report type
		Ar << ScriptStruct;

		//Report value
		if (uint8* const MemPtr = GetMutableMemory(); ScriptStruct && ensureAlways(MemPtr))
		{
			ConstCast(ScriptStruct)->SerializeItem(Ar, MemPtr, /* Defaults */ nullptr);
		}
	}

	return true;
}

bool FVariadicStruct::ExportTextItem(FString& ValueStr, const FVariadicStruct& DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const
{
	if (const uint8* const MemPtr = GetMemory(); ScriptStruct && ensureAlways(MemPtr))
	{
		ValueStr += ScriptStruct->GetPathName();

		//Force the default to nullptr to disable delta serialization because we reset the memory in import text.
		ScriptStruct->ExportText(ValueStr, MemPtr, nullptr, Parent, PortFlags, ExportRootScope);
	}
	else
	{
		ValueStr += TEXT("None");
	}

	return true;
}

bool FVariadicStruct::ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText, FArchive* InSerializingArchive /* = nullptr */)
{
	FNameBuilder StructPathName;

	//UHT uses "()" as a general "empty struct" marker, so allow importing that as an alias for "None"
	if (FCString::Strcmp(Buffer, TEXT("()")) == 0)
	{
		Buffer += 2;
	}
	else if (const TCHAR* const Result = FPropertyHelpers::ReadToken(Buffer, StructPathName, /* bDottedNames */true))
	{
		Buffer = Result;
	}
	else
	{
		return false;
	}

	if (StructPathName.Len() == 0 || FCString::Stricmp(StructPathName.ToString(), TEXT("None")) == 0)
	{
		InitializeAs(nullptr);
	}
	else
	{
		{
			//Redirect the struct name if required.
			const FCoreRedirectObjectName OldName = FCoreRedirectObjectName(StructPathName.ToString());
			const FCoreRedirectObjectName NewName = FCoreRedirects::GetRedirectedName(ECoreRedirectFlags::Type_Struct, OldName, ECoreRedirectMatchFlags::AllowPartialMatch);

			if (OldName != NewName)
			{
				StructPathName.Reset();
				StructPathName.Append(NewName.ToString());
			}
		}

		//Make sure the struct is actually loaded before trying to import the text (this boils down to FindObject if the struct is already loaded).
		//This is needed for user defined structs, BP pin values, config, copy/paste, where there's no guarantee that the referenced struct has actually been loaded yet.
		if (const UScriptStruct* const StructTypePtr = LoadObject<UScriptStruct>(nullptr, StructPathName.ToString()))
		{
			InitializeAs(StructTypePtr);

			if (const TCHAR* const Result = StructTypePtr->ImportText(Buffer, GetMutableMemory(), Parent, PortFlags, ErrorText, [StructTypePtr]() { return StructTypePtr->GetName(); }))
			{
				Buffer = Result;
			}
			else
			{
				return false;
			}
		}
		else
		{
			return false;
		}
	}

	return true;
}

bool FVariadicStruct::SerializeFromMismatchedTag(const FPropertyTag& Tag, FStructuredArchive::FSlot Slot)
{
	constexpr FGuid InstancedStructGuid{ 0xE21E1CAA, 0xAF47425E, 0x89BF6AD4, 0x4C44A8BB };
	static const FName NAME_InstancedStruct = "InstancedStruct";

	//We should be fully serialization compatible with FInstancedStruct.
	if (Tag.GetType().IsStruct(NAME_InstancedStruct))
	{
		FArchive& Ar = Slot.GetUnderlyingArchive();

		if (!ensureMsgf(Ar.CustomVer(InstancedStructGuid) == 0, TEXT("FVariadicStruct: Failed to backport FInstancedStruct. The data might be lost.")))
		{
			return false;
		}

		if (!ensureMsgf(!Ar.IsTextFormat(), TEXT("FVariadicStruct: Failed to port FInstancedStruct from text format.")))
		{
			return false;
		}

		//If the archive is very old.
		if (Ar.CustomVer(InstancedStructGuid) < 0)
		{
			//The old format had "header+version" in editor builds, and just "version" otherwise.
			//If the first thing we read is the old header, consume it, if not go back and assume that we have just the version.
			const int64 HeaderOffset = Ar.Tell();
			uint32 Header = 0;
			Ar << Header;

			if (constexpr uint32 LegacyEditorHeader = 0xABABABAB; Header != LegacyEditorHeader)
			{
				Ar.Seek(HeaderOffset);
			}

			uint8 Version = 0;
			Ar << Version;
		}

		{
			UScriptStruct* SerializedScriptStruct = nullptr;
			Ar << SerializedScriptStruct;

			if (SerializedScriptStruct)
			{
				Ar.Preload(SerializedScriptStruct);
			}

			//Initialize only if the type changes.
			if (ScriptStruct != SerializedScriptStruct)
			{
				InitializeAs(SerializedScriptStruct);
			}

			int32 SerialSize = 0;
			Ar << SerialSize;

			//Check if the type is not missed.
			if (!ScriptStruct && SerialSize > 0)
			{
				//Step over missing data.
				Ar.Seek(Ar.Tell() + SerialSize);

				UE_LOG(LogCore, Warning, TEXT("FVariadicStruct: Failed to serialize UScriptStruct with SerialSize: %u, SerializedProperty: %s, LinkerRoot: %s."),
					   SerialSize, *GetPathNameSafe(Ar.GetSerializedProperty()), Ar.GetLinker() ? *GetPathNameSafe(Ar.GetLinker()->LinkerRoot) : TEXT("NoLinker"));
			}

			//Serialize the actual value.
			if (uint8* const MemPtr = GetMutableMemory(); ScriptStruct && ensureAlways(MemPtr))
			{
				ConstCast(ScriptStruct)->SerializeItem(Ar, MemPtr, /* Defaults */ nullptr);
			}
		}

		return true;
	}

	return false;
}

void FVariadicStruct::GetPreloadDependencies(TArray<UObject*>& OutDeps)
{
	if (uint8* const MemPtr = GetMutableMemory(); ScriptStruct && ensureAlways(MemPtr))
	{
		OutDeps.Add(ConstCast(ScriptStruct));

		//Report direct dependencies.
		if (UScriptStruct::ICppStructOps* const CppStructOps = ScriptStruct->GetCppStructOps())
		{
			CppStructOps->GetPreloadDependencies(MemPtr, OutDeps);
		}

		//Recursively report indirect dependencies.
		for (TPropertyValueIterator<FStructProperty> It(ScriptStruct, MemPtr); It; ++It)
		{
			const UScriptStruct* StructType = It.Key()->Struct;

			if (UScriptStruct::ICppStructOps* const CppStructOps = StructType->GetCppStructOps())
			{
				void* const StructDataPtr = const_cast<void*>(It.Value());
				CppStructOps->GetPreloadDependencies(StructDataPtr, OutDeps);
			}
		}
	}
}

bool FVariadicStruct::Identical(const FVariadicStruct* Other, uint32 PortFlags) const
{
	if (ScriptStruct && ScriptStruct == Other->ScriptStruct)
	{
		return ScriptStruct->CompareScriptStruct(GetMemory(), Other->GetMemory(), PortFlags);
	}

	return false;
}

void FVariadicStruct::AddStructReferencedObjects(FReferenceCollector& Collector)
{
#if WITH_ENGINE && WITH_EDITOR
	//Reference collector is used to visit all instances of instanced structs and replace their contents.
	if (const UUserDefinedStruct* StructureToReinstance = UE::StructUtils::Private::GetStructureToReinstance())
	{
		if (const UUserDefinedStruct* UserDefinedStruct = Cast<UUserDefinedStruct>(ScriptStruct))
		{
			if (StructureToReinstance->Status == EUserDefinedStructureStatus::UDSS_Duplicate)
			{
				// On the first pass we replace the UDS with a duplicate that represents the currently allocated struct.
				// GStructureToReinstance is the duplicated struct, and StructureToReinstance->PrimaryStruct is the UDS that is being reinstanced.

				if (UserDefinedStruct == StructureToReinstance->PrimaryStruct)
				{
					ScriptStruct = StructureToReinstance;
				}
			}
			else
			{
				// On the second pass we reinstantiate the data using serialization.
				// When saving, the UDSs are written using the duplicate which represents current layout, but PrimaryStruct is serialized as the type.
				// When reading, the data is initialized with the new type, and the serialization will take care of reading from the old data.

				if (UserDefinedStruct->PrimaryStruct == StructureToReinstance)
				{
					if (UObject* const Outer = UE::StructUtils::Private::GetCurrentReinstanceOuterObject())
					{
						if (!Outer->IsA<UClass>() && !Outer->HasAnyFlags(RF_ClassDefaultObject))
						{
							Outer->MarkPackageDirty();
						}
					}

					TArray<uint8> Data;

					FMemoryWriter Writer(Data);
					FObjectAndNameAsStringProxyArchive WriterProxy(Writer, /* bInLoadIfFindFails */ true);
					Serialize(WriterProxy);

					FMemoryReader Reader(Data);
					FObjectAndNameAsStringProxyArchive ReaderProxy(Reader, /* bInLoadIfFindFails */ true);
					Serialize(ReaderProxy);
				}
			}
		}
	}
#endif

	if (uint8* const MemPtr = GetMutableMemory(); ScriptStruct && ensureAlways(MemPtr))
	{
		Collector.AddReferencedObject(ScriptStruct);
		Collector.AddPropertyReferencesWithStructARO(ScriptStruct, MemPtr);
	}
}

bool FVariadicStruct::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
#if WITH_ENGINE
	uint8 bIsValid = 0;

	if (Ar.IsSaving())
	{
		bIsValid = IsValid();
	}

	Ar.SerializeBits(&bIsValid, 1);

	if (!bIsValid)
	{
		//Reset the existing struct.
		if (Ar.IsLoading())
		{
			Reset();
		}
	}
	else
	{
		//Serialize UScripStruct.
		if (Ar.IsSaving())
		{
			Ar << ScriptStruct;
		}
		else if (Ar.IsLoading())
		{
			UScriptStruct* SerializedScriptStruct = nullptr;
			Ar << SerializedScriptStruct;

			//Initialize only if the type changes.
			if (ScriptStruct != SerializedScriptStruct)
			{
				InitializeAs(SerializedScriptStruct);
			}

			if (!IsValid())
			{
				UE_LOG(LogCore, Error, TEXT("FVariadicStruct: Failed to NetSerialize UScriptStruct and recover the corrupted archive."));
				bOutSuccess = false;
				Ar.SetError();
			}
		}

		//Serialize the actual value. 
		if (uint8* const Memory = GetMutableMemory(); ScriptStruct && ensureAlways(Memory))
		{
			if (ScriptStruct->StructFlags & STRUCT_NetSerializeNative)
			{
				ScriptStruct->GetCppStructOps()->NetSerialize(Ar, Map, bOutSuccess, Memory);
			}
			else
			{
				UPackageMapClient* const MapClient = Cast<UPackageMapClient>(Map);

				if (ensureAlways(MapClient && MapClient->GetConnection() && MapClient->GetConnection()->GetDriver()))
				{
					const TSharedPtr<FRepLayout> RepLayout = MapClient->GetConnection()->GetDriver()->GetStructRepLayout(ConstCast(ScriptStruct));

					if (ensureAlways(RepLayout.IsValid()))
					{
						bool bHasUnmapped = false;
						RepLayout->SerializePropertiesForStruct(ConstCast(ScriptStruct), static_cast<FBitArchive&>(Ar), Map, Memory, bHasUnmapped);
					}
				}
			}
		}
	}

	return true;

#else // WITH_ENGINE

	return false; //The implementation above relies on types in the Engine module, so it can't be compiled without the engine.

#endif // WITH_ENGINE
}

bool FVariadicStruct::FindInnerPropertyInstance(FName PropertyName, const FProperty*& OutProp, const void*& OutData) const
{
	if (const uint8* const MemPtr = GetMemory(); ScriptStruct && ensureAlways(MemPtr))
	{
		for (const FProperty* const Prop : TFieldRange<FProperty>(ScriptStruct))
		{
			if (Prop->GetFName() == PropertyName)
			{
				OutProp = Prop;
				OutData = MemPtr;
				return true;
			}
		}
	}

	return false;
}
