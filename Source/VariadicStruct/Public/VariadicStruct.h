// Copyright 2024 Ivan Baktenkov. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "CoreTypes.h"
#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"
#include "Misc/EngineVersionComparison.h"
#include "Serialization/StructuredArchive.h"
#include "UObject/Class.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectPtr.h"
#include "UObject/PropertyPortFlags.h"

#if UE_VERSION_OLDER_THAN(5, 5, 0)
#include "StructView.h"
#else
#include "StructUtils/StructView.h"
#include "Templates/Function.h"
#endif // UE_VERSION_OLDER_THAN

#include <concepts>
#include <memory> // std::destroy_at
#include <new>	  // std::launder

#include "VariadicStruct.generated.h"

class FArchive;
class FOutputDevice;
class FProperty;
class FReferenceCollector;
class FString;
class UObject;
class UPackageMap;

struct FPropertyTag;
struct FPropertyVisitorData;
struct FPropertyVisitorInfo;
struct FPropertyVisitorPath;

enum class EPropertyVisitorControlFlow : uint8;

namespace VariadicStruct
{
	template<typename... Args>
	struct TypePack final {};

	/** List of unsupported types for FVariadicStruct. */
	using UnsupportedTypes = TypePack<FVariadicStruct, FInstancedStruct, FSharedStruct, FConstSharedStruct>;

	/** Generic concept of UScriptStruct wrappers. */
	template<typename T>
	concept CScriptStructWrapper = requires(T Value)
	{
		{ Value.GetScriptStruct() } -> std::convertible_to<const UScriptStruct*>;
		{ Value.GetMemory() } -> std::convertible_to<const uint8*>;
		{ Value.IsValid() } -> std::convertible_to<bool>;
	};

	/** Supported template type parameters for FVariadicStruct. Top-level constness is not supported due to type erasure. */
	template<typename T>
	concept CSupportedType = not CScriptStructWrapper<T> && std::is_class_v<T> && not std::is_const_v<T> && requires(T Value)
	{
		{ TBaseStructure<T>::Get() } -> std::convertible_to<const UScriptStruct*>;
	};

	/** Type-converts the value at an existing memory location. */
	template<typename T> requires(CSupportedType<std::remove_const_t<T>>)
	T* GetTypedPtr(std::conditional_t<std::is_const_v<T>, const uint8*, uint8*> MemoryPtr)
	{
		// Formally speaking, reinterpreting the non standard_layout Derived* as Base* is UB.
		return std::launder(reinterpret_cast<T*>(MemoryPtr));
	}

	/** Returns FStructView from a generic type value. */
	template<typename T> requires(not std::derived_from<T, FStructView>)
	FStructView MakeView(T& InValue)
	{
		return FStructView(InValue.GetScriptStruct(), InValue.GetMutableMemory());
	}

	/** Returns FConstStructView from a generic type value. */
	template<typename T> requires(not std::derived_from<T, FConstStructView>)
	FConstStructView MakeConstView(const T& InValue)
	{
		return FConstStructView(InValue.GetScriptStruct(), InValue.GetMemory());
	}

	/** Validates UScriptStruct to be used with FVariadicStruct. */
	bool ValidateScriptStruct(const UScriptStruct* InScriptStruct);
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
	FVariadicStruct(const FInstancedStruct&)   = delete;
	FVariadicStruct(const FSharedStruct&)	   = delete;
	FVariadicStruct(const FConstSharedStruct&) = delete;
	FVariadicStruct(const FStructView&)		   = delete;
	FVariadicStruct(const FConstStructView&)   = delete;

	~FVariadicStruct() { Reset(); }

	/** Initializes from struct template type and optional params in place. */
	template<VariadicStruct::CSupportedType T, typename... TArgs>
	T* InitializeAs(TArgs&&... InArgs)
	{
		uint8* MemoryPtr = StructBuffer;

		// If the existing type is valid and matches.
		if (const UScriptStruct* const InScriptStruct = TBaseStructure<T>::Get(); InScriptStruct == ScriptStruct)
		{
			// We can reuse the same memory.
			if constexpr (TypeRequiresMemoryAllocation<T>())
			{
				MemoryPtr = StructMemory;
			}

			// Destroy the existing struct directly.
			std::destroy_at(VariadicStruct::GetTypedPtr<T>(MemoryPtr));
		}
		else
		{
			Reset();

			ScriptStruct = InScriptStruct;

			// Allocate a new space if the buffer is too small.
			if constexpr (TypeRequiresMemoryAllocation<T>())
			{
				MemoryPtr = StructMemory = static_cast<uint8*>(FMemory::Malloc(sizeof(T), alignof(T)));
				checkSlow(MemoryPtr != nullptr);
			}
		}

		// Return the value pointer avoiding std::launder() if the type is immediately used.
		return new (MemoryPtr) T(Forward<TArgs>(InArgs)...);
	}

	/** Initializes from UScriptStruct type and copies the value if needed. */
	void InitializeAs(const UScriptStruct* InScriptStruct, const uint8* InStructMemory = nullptr);

public: // Factories

	/** Copy/move constructs a new FVariadicStruct from a template struct. */
	template<typename T> requires(VariadicStruct::CSupportedType<std::remove_cvref_t<T>>)
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

	/** Default constructs a new FVariadicStruct from a generic struct wrapper. */
	template<VariadicStruct::CScriptStructWrapper T>
	[[nodiscard]] static FVariadicStruct Make(const T& InStructWrapper)
	{
		FVariadicStruct Variadic;
		Variadic.InitializeAs(InStructWrapper.GetScriptStruct(), InStructWrapper.GetMemory());
		return Variadic;
	}

public: // Data Access

	/** Helper for validating the underlying type. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] bool IsTypeOf() const
	{
		return TBaseStructure<T>::Get() == ScriptStruct || (!bExactType && ScriptStruct && ScriptStruct->IsChildOf(TBaseStructure<T>::Get()));
	}

	/** Returns a const pointer to the struct value, or nullptr if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] const T* GetValuePtr() const
	{
		// Use faster path if the type matches.
		if (const UScriptStruct* const BaseStructure = TBaseStructure<T>::Get(); ScriptStruct == BaseStructure)
		{
			return VariadicStruct::GetTypedPtr<const T>(GetTypeMemory<T>());
		}
		else if (!bExactType && ScriptStruct && ScriptStruct->IsChildOf(BaseStructure))
		{
			return VariadicStruct::GetTypedPtr<const T>(GetMemory());
		}

		return nullptr;
	}

	/** Returns a const reference to the struct value, or asserts if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] const T& GetValue() const
	{
		// bExactType can be used to avoid branching and assert unexpected types.
		if (bExactType || TBaseStructure<T>::Get() == ScriptStruct)
		{
			checkf(!bExactType || TBaseStructure<T>::Get() == ScriptStruct, TEXT("FVariadicStruct: Exact type mismatch."));
			return *VariadicStruct::GetTypedPtr<const T>(GetTypeMemory<T>());
		}
		else
		{
			checkf(ScriptStruct && ScriptStruct->IsChildOf(TBaseStructure<T>::Get()), TEXT("FVariadicStruct: Type mismatch."));
			return *VariadicStruct::GetTypedPtr<const T>(GetMemory());
		}
	}

	/** Returns a mutable pointer to the struct value, or nullptr if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] T* GetMutableValuePtr()
	{
		// Use faster path if the type matches.
		if (const UScriptStruct* const BaseStructure = TBaseStructure<T>::Get(); ScriptStruct == BaseStructure)
		{
			return VariadicStruct::GetTypedPtr<T>(GetMutableTypeMemory<T>());
		}
		else if (!bExactType && ScriptStruct && ScriptStruct->IsChildOf(BaseStructure))
		{
			return VariadicStruct::GetTypedPtr<T>(GetMutableMemory());
		}

		return nullptr;
	}

	/** Returns a mutable reference to the struct value, or asserts if the type doesn't match. */
	template<VariadicStruct::CSupportedType T, bool bExactType = false>
	[[nodiscard]] T& GetMutableValue()
	{
		// bExactType can be used to avoid branching and assert unexpected types.
		if (bExactType || TBaseStructure<T>::Get() == ScriptStruct)
		{
			checkf(!bExactType || TBaseStructure<T>::Get() == ScriptStruct, TEXT("FVariadicStruct: Exact type mismatch."));
			return *VariadicStruct::GetTypedPtr<T>(GetMutableTypeMemory<T>());
		}
		else
		{
			checkf(ScriptStruct && ScriptStruct->IsChildOf(TBaseStructure<T>::Get()), TEXT("FVariadicStruct: Type mismatch."));
			return *VariadicStruct::GetTypedPtr<T>(GetMutableMemory());
		}
	}

public: // Utility

	/** Whether FVariadicStruct wraps any struct value. */
	bool IsValid() const
	{
		return GetScriptStruct() != nullptr;
	}

	/** Returns UScriptStruct of the underlying struct. */
	const UScriptStruct* GetScriptStruct() const
	{
		return ScriptStruct;
	}

	/** Returns a const pointer to the underlying struct memory. */
	const uint8* GetMemory() const
	{
		return ScriptStruct && !RequiresMemoryAllocation(ScriptStruct) ? StructBuffer : StructMemory;
	}

	/** Returns a mutable pointer to the underlying struct memory. */
	uint8* GetMutableMemory()
	{
		return ScriptStruct && !RequiresMemoryAllocation(ScriptStruct) ? StructBuffer : StructMemory;
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
	bool Serialize(FArchive& Ar, const FConstStructView* Defaults = nullptr);
	bool Identical(const FVariadicStruct* Other, uint32 PortFlags = PPF_None) const;
	void AddStructReferencedObjects(FReferenceCollector& Collector);
	bool ExportTextItem(FString& ValueStr, const FVariadicStruct& DefaultValue = FVariadicStruct(), UObject* Parent = nullptr, int32 PortFlags = PPF_None, UObject* ExportRootScope = nullptr) const;
	bool ImportTextItem(const TCHAR*& Buffer, int32 PortFlags = PPF_None, UObject* Parent = nullptr, FOutputDevice* ErrorText = nullptr, FArchive* InSerializingArchive = nullptr);
	bool SerializeFromMismatchedTag(const FPropertyTag& Tag, FStructuredArchive::FSlot Slot);
	void GetPreloadDependencies(TArray<UObject*>& OutDeps);
	bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
	bool FindInnerPropertyInstance(FName PropertyName, const FProperty*& OutProp, const void*& OutData) const;
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
	EPropertyVisitorControlFlow Visit(FPropertyVisitorPath& Path, const FPropertyVisitorData& Data, const TFunctionRef<EPropertyVisitorControlFlow(const FPropertyVisitorPath& /*Path*/, const FPropertyVisitorData& /*Data*/)> InFunc) const;
	void* ResolveVisitedPathInfo(const FPropertyVisitorInfo& Info) const;
#endif // UE_VERSION_OLDER_THAN

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
#if !UE_VERSION_OLDER_THAN(5, 5, 0)
		WithClearOnFinishDestroy = true,
		WithVisitor = true,
#endif // UE_VERSION_OLDER_THAN
	};
};

inline bool VariadicStruct::ValidateScriptStruct(const UScriptStruct* InScriptStruct)
{
	return !InScriptStruct || [=]<typename... Args>(TypePack<Args...>) { return (... && (TBaseStructure<Args>::Get() != InScriptStruct)); }(UnsupportedTypes());
}
