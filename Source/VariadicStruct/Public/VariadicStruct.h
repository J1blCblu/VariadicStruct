// Copyright 2024 Ivan Baktenkov. All Rights Reserved.

#pragma once

#include <concepts>
#include <memory> // std::destroy_at/construct_at
#include <new> // std::launder

#include "CoreTypes.h"
#include "Containers/Array.h"
#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"
#include "Serialization/StructuredArchive.h"
#include "StructView.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectPtr.h"
#include "UObject/PropertyPortFlags.h"

#include "VariadicStruct.generated.h"

class FArchive;
class FOutputDevice;
class FProperty;
class FReferenceCollector;
class FString;
class UObject;
class UPackageMap;

struct FPropertyTag;

namespace VariadicStruct
{
	/** Supported template type parameters for FVariadicStruct. */
	template<typename T>
	concept CSupportedType = std::is_class_v<T>
		&& not std::is_const_v<T> // Otherwise, public API may eventually lead to UB due to type erasure.
		&& not std::derived_from<T, FVariadicStruct>
		&& not std::derived_from<T, FInstancedStruct>
		&& not std::derived_from<T, FSharedStruct>
		&& not std::derived_from<T, FConstSharedStruct>
		&& requires(T)
	{
		{ TBaseStructure<T>::Get() } -> std::convertible_to<UScriptStruct*>;
	};
	
	/** Type-converts the value at an existing memory location. */
	template<typename T> requires CSupportedType<std::remove_const_t<T>>
	T* GetTypePtr(std::conditional_t<std::is_const_v<T>, const uint8*, uint8*> Memory)
	{
		return std::launder(reinterpret_cast<T*>(Memory));
	}

	/** Returns FStructView from a generic type value. */
	template<typename T> requires(not std::derived_from<FStructView>)
	FStructView MakeView(T& InValue)
	{
		return FStructView(InValue.GetScriptStruct(), InValue.GetMutableMemory());
	}

	/** Returns FConstStructView from a generic type value. */
	template<typename T> requires(not std::derived_from<FConstStructView>)
	FConstStructView MakeConstView(const T& InValue)
	{
		return FConstStructView(InValue.GetScriptStruct(), InValue.GetMemory());
	}
}

/**
 * Implementation of FInstancedStruct with SBO (Small Buffer Optimization) with default buffer size of 24 bytes.
 * Particularly useful when the expected types rarely exceed the buffer size, such as optional payload data.
 * Serialization compatible (non-commutative) with FInstancedStruct.
 * 
 * FVariadicStruct has some key differences from FInstancedStruct that should be taken into account:
 * 1. Requires 32 bytes instead of 16 and 16-byte alignment instead of 8.
 * 2. Requires extra steps to access the data including 1 branching and 1 indirection if the type doesn't match.
 * 3. Move constructor and move assignment operator require a copy constructor for types that fit into the buffer.
 * 4. Similar to FInstancedStructContainer, not exposed to the Editor and BP as it doesn't make much sense.
 * 
 * @Note: FInstancedStructContainer might still be more preferable for contiguous heterogeneous data.
 */
USTRUCT()
struct alignas(16) VARIADICSTRUCT_API FVariadicStruct
{
	GENERATED_BODY()

public:

	FVariadicStruct() = default;
	FVariadicStruct(FVariadicStruct&& InOther);
	FVariadicStruct(const FVariadicStruct& InOther);
	FVariadicStruct& operator=(FVariadicStruct&& InOther);
	FVariadicStruct& operator=(const FVariadicStruct& InOther);

	/** FVariadicStruct::Make() should be used instead. */
	FVariadicStruct(const FInstancedStruct&)	= delete;
	FVariadicStruct(const FSharedStruct&)		= delete;
	FVariadicStruct(const FConstSharedStruct&)	= delete;
	FVariadicStruct(const FStructView&)			= delete;
	FVariadicStruct(const FConstStructView&)	= delete;

	~FVariadicStruct() { Reset(); }

	/** Initializes from struct template type and optional params in place. */
	template<VariadicStruct::CSupportedType T, typename... TArgs>
	T* InitializeAs(TArgs&&... InArgs)
	{
		const UScriptStruct* const InScriptStruct = TBaseStructure<T>::Get();
		uint8* MemoryPtr = nullptr;

		// If the existing type is valid and matches.
		if (InScriptStruct == ScriptStruct)
		{
			// We can reuse the same memory.
			MemoryPtr = TypeRequiresMemoryAllocation<T>() ? StructMemory : StructBuffer;

			// Destroy the existing struct directly.
			std::destroy_at(std::launder(reinterpret_cast<T*>(MemoryPtr)));
		}
		else // If the type doesn't match.
		{
			Reset(); // Destroy the existing struct.

			ScriptStruct = InScriptStruct;

			// Allocate a new space if the buffer is too small.
			if constexpr (TypeRequiresMemoryAllocation<T>())
			{
				MemoryPtr = StructMemory = static_cast<uint8*>(FMemory::Malloc(sizeof(T), alignof(T)));
			}
			else
			{
				MemoryPtr = StructBuffer;
			}
		}

		// Return the value pointer avoiding std::launder() if the type is immediately used.
		T* ResultPtr = nullptr;

		// Finally, construct a new struct.
		if (ensureAlways(MemoryPtr))
		{
			ResultPtr = new (MemoryPtr) T(Forward<TArgs>(InArgs)...);
		}

		return ResultPtr;
	}

	/** Initializes from UScriptStruct type and copies the value if needed. */
	void InitializeAs(const UScriptStruct* InScriptStruct, const uint8* InStructMemory = nullptr);

public: // Factories

	/** Copy/move constructs a new FVariadicStruct from a template struct. */
	template<typename T> requires VariadicStruct::CSupportedType<std::remove_cvref_t<T>>
	[[nodiscard]] static FVariadicStruct Make(T&& InStruct)
	{
		FVariadicStruct Variadic;
		Variadic.InitializeAs<std::remove_cvref_t<T>>(Forward<T>(InStruct));
		return Variadic;
	}

	/** Emplace constructs a new FVariadicStruct from a template struct type and arguments. */
	template<VariadicStruct::CSupportedType T, typename... TArgs>
	[[nodiscard]] static FVariadicStruct Make(TArgs&&... InArgs)
	{
		FVariadicStruct Variadic;
		Variadic.InitializeAs<T>(Forward<TArgs>(InArgs)...);
		return Variadic;
	}

	/** Default constructs a new FVariadicStruct from UScriptStruct and copies the value if needed. */
	[[nodiscard]] static FVariadicStruct Make(const UScriptStruct* InScriptStruct, const uint8* InStructMemory = nullptr)
	{
		FVariadicStruct Variadic;
		Variadic.InitializeAs(InScriptStruct, InStructMemory);
		return Variadic;
	}

public: // Data Access

	/** Returns a const pointer to the struct value, or nullptr if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] const T* GetValuePtr() const
	{
		// Use faster path if the type matches.
		if (const UScriptStruct* const BaseStructure = TBaseStructure<T>::Get(); ScriptStruct == BaseStructure)
		{
			return VariadicStruct::GetTypePtr<const T>(GetTypeMemory<T>());
		}
		else if (!bExactType && ScriptStruct && ScriptStruct->IsChildOf(BaseStructure))
		{
			return VariadicStruct::GetTypePtr<const T>(GetMemory());
		}

		return nullptr;
	}

	/** Returns a const reference to the struct value, or asserts if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] const T& GetValue() const
	{
		// bExactType can be used to avoid branching and assert unexpected types.
		if constexpr (bExactType)
		{
			check(ScriptStruct == TBaseStructure<T>::Get());
			return *VariadicStruct::GetTypePtr<const T>(GetTypeMemory<T>());
		}
		else
		{
			// Use faster path if the type matches.
			if (const UScriptStruct* const BaseStructure = TBaseStructure<T>::Get(); ScriptStruct == BaseStructure)
			{
				return *VariadicStruct::GetTypePtr<const T>(GetTypeMemory<T>());
			}
			else
			{
				check(ScriptStruct && ScriptStruct->IsChildOf(BaseStructure) && GetMemory());
				return *VariadicStruct::GetTypePtr<const T>(GetMemory());
			}
		}
	}

	/** Returns a mutable pointer to the struct value, or nullptr if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] T* GetMutableValuePtr()
	{
		// Use faster path if the type matches.
		if (const UScriptStruct* const BaseStructure = TBaseStructure<T>::Get(); ScriptStruct == BaseStructure)
		{
			return VariadicStruct::GetTypePtr<T>(GetMutableTypeMemory<T>());
		}
		else if (!bExactType && ScriptStruct && ScriptStruct->IsChildOf(BaseStructure))
		{
			return VariadicStruct::GetTypePtr<T>(GetMutableMemory());
		}

		return nullptr;
	}

	/** Returns a mutable reference to the struct value, or asserts if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] T& GetMutableValue()
	{
		// bExactType can be used to avoid branching and assert unexpected types.
		if constexpr(bExactType)
		{
			check(ScriptStruct == TBaseStructure<T>::Get());
			return *VariadicStruct::GetTypePtr<T>(GetMutableTypeMemory<T>());
		}
		else
		{
			// Use faster path if the type matches.
			if (const UScriptStruct* const BaseStructure = TBaseStructure<T>::Get(); ScriptStruct == BaseStructure)
			{
				return *VariadicStruct::GetTypePtr<T>(GetMutableTypeMemory<T>());
			}
			else
			{
				check(ScriptStruct && ScriptStruct->IsChildOf(BaseStructure) && GetMutableMemory());
				return *VariadicStruct::GetTypePtr<T>(GetMutableMemory());
			}
		}
	}

public: // Utility

	/** Whether FVariadicStruct wraps any struct value. */
	bool IsValid() const
	{
		return GetScriptStruct() && ensureAlways(GetMemory());
	}

	/** Returns UScriptStruct of the underlying struct. */
	const UScriptStruct* GetScriptStruct() const
	{
		return ScriptStruct;
	}

	/** Returns a const pointer to the underlying struct memory. */
	const uint8* GetMemory() const
	{
		if (ScriptStruct && !RequiresMemoryAllocation(ScriptStruct))
		{
			return StructBuffer;
		}

		return StructMemory;
	}

	/** Returns a mutable pointer to the underlying struct memory. */
	uint8* GetMutableMemory()
	{
		if (ScriptStruct && !RequiresMemoryAllocation(ScriptStruct))
		{
			return StructBuffer;
		}

		return StructMemory;
	}

	/** Deep compares the struct instance when identical. */
	bool operator==(const FVariadicStruct& Other) const
	{
		return Identical(&Other, PPF_None);
	}

	/** Deep compares the struct instance when identical. */
	bool operator!=(const FVariadicStruct& Other) const
	{
		return !Identical(&Other, PPF_None);
	}

	/** Destroy the underlying struct value. StructBuffer retains garbage. */
	void Reset();

public: // StructOpsTypeTraits

	// Mostly copy pasted from FInstancedStruct.
	bool Serialize(FArchive& Ar);
	bool Identical(const FVariadicStruct* Other, uint32 PortFlags) const;
	void AddStructReferencedObjects(FReferenceCollector& Collector);
	bool ExportTextItem(FString& ValueStr, const FVariadicStruct& DefaultValue, UObject* Parent, int32 PortFlags, UObject* ExportRootScope) const;
	bool ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Parent, FOutputDevice* ErrorText, FArchive* InSerializingArchive = nullptr);
	bool SerializeFromMismatchedTag(const FPropertyTag& Tag, FStructuredArchive::FSlot Slot);
	void GetPreloadDependencies(TArray<UObject*>& OutDeps);
	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
	bool FindInnerPropertyInstance(FName PropertyName, const FProperty*& OutProp, const void*& OutData) const;

protected:

	void ResetStructData(const UScriptStruct* InScriptStruct = nullptr, uint8* InStructMemory = nullptr)
	{
		// This will also formally make StructMemory active in union.
		StructMemory = InStructMemory;
		ScriptStruct = InScriptStruct;
	}

	/** Determines whether the type requires memory allocation. */
	bool RequiresMemoryAllocation(const UScriptStruct* InScriptStruct) const
	{
		// We can skip the extra alignment check at runtime if the buffer is properly sized.
		if constexpr (BUFFER_SIZE < alignof(FVariadicStruct) * 2)
		{
			return InScriptStruct->GetStructureSize() > BUFFER_SIZE;
		}
		else
		{
			return InScriptStruct->GetStructureSize() > BUFFER_SIZE || InScriptStruct->GetMinAlignment() > alignof(FVariadicStruct);
		}
	}

	/** Determines whether the type requires memory allocation at compile time. */
	template<VariadicStruct::CSupportedType T>
	static consteval bool TypeRequiresMemoryAllocation()
	{
		return sizeof(T) > BUFFER_SIZE || alignof(T) > alignof(FVariadicStruct);
	}

	/** Returns resolved memory location at compile time. */
	template<VariadicStruct::CSupportedType T>
	const uint8* GetTypeMemory() const
	{
		return TypeRequiresMemoryAllocation<T>() ? StructMemory : StructBuffer;
	}

	/** Returns resolved memory location at compile time. */
	template<VariadicStruct::CSupportedType T>
	uint8* GetMutableTypeMemory()
	{
		return TypeRequiresMemoryAllocation<T>() ? StructMemory : StructBuffer;
	}

private:

	/** Defined in .cpp to statically validate invariants. */
	friend consteval void FVariadicStructValidateInvariants();
	friend consteval void FVariadicStructValidateTestInvariants();

	static inline constexpr int32 BUFFER_SIZE = 24;

	union
	{
		/** Pointer to the heap for large structs. */
		 uint8* StructMemory = nullptr;

		/** Inline memory buffer for small structs. */
		 uint8 StructBuffer[BUFFER_SIZE];
	};

	/** UScriptStruct type of the underlying struct value. */
	TObjectPtr<const UScriptStruct> ScriptStruct = nullptr;
};

template<>
struct TStructOpsTypeTraits<FVariadicStruct> : public TStructOpsTypeTraitsBase2<FVariadicStruct>
{
	enum
	{
		WithSerializer = true,
		WithIdentical = true,
		WithExportTextItem = true,
		WithImportTextItem = true,
		WithAddStructReferencedObjects = true,
		WithStructuredSerializeFromMismatchedTag = true,
		WithGetPreloadDependencies = true,
		WithNetSerializer = true,
		WithFindInnerPropertyInstance = true,
	};
};
