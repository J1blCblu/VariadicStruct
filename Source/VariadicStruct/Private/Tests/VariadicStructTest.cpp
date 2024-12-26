// Copyright 2024 Ivan Baktenkov. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VariadicStruct.h"

#include "Math/IntPoint.h"	//sizeof()  < BUFFER_SIZE
#include "Math/Vector.h"	//sizeof() == BUFFER_SIZE
#include "Math/Transform.h"	//sizeof()  > BUFFER_SIZE
#include "Math/Plane.h"		//Different Base Class

/**
 * Investigate Low-Level Tests. https://dev.epicgames.com/documentation/en-us/unreal-engine/low-level-tests-in-unreal-engine
 */

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVariadicStructConstructorTest, "System.StructUtils.VariadicStruct",
								 EAutomationTestFlags::EditorContext |
								 EAutomationTestFlags::ClientContext |
								 EAutomationTestFlags::SmokeFilter);

bool FVariadicStructConstructorTest::RunTest(const FString&)
{
	FVariadicStruct Variadic;
	UTEST_INVALID_EXPR(Variadic);

	auto ValidateType = [this]<typename T>(T DefaultValue)
		{
			using Type = std::remove_cvref_t<T>;

			//InitializeAs<T>().
			FVariadicStruct Variadic = FVariadicStruct::Make(DefaultValue);
			UTEST_VALID_EXPR(Variadic);
			UTEST_NOT_NULL_EXPR(Variadic.GetValuePtr<Type>());
			UTEST_NOT_NULL_EXPR(Variadic.GetMutableValuePtr<Type>());
			UTEST_EQUAL_EXPR(Variadic.GetValue<Type>(), DefaultValue);
			UTEST_EQUAL_EXPR(Variadic.GetMutableValue<Type>(), DefaultValue);

			//InitializeAs(UScriptStruct, uint8*).
			FVariadicStruct ScriptVariadic = FVariadicStruct::Make(TBaseStructure<Type>::Get(), Variadic.GetMutableMemory());
			UTEST_VALID_EXPR(Variadic);
			UTEST_NOT_NULL_EXPR(Variadic.GetValuePtr<Type>());
			UTEST_NOT_NULL_EXPR(Variadic.GetMutableValuePtr<Type>());
			UTEST_EQUAL_EXPR(Variadic.GetValue<Type>(), DefaultValue);
			UTEST_EQUAL_EXPR(Variadic.GetMutableValue<Type>(), DefaultValue);

			UTEST_EQUAL_EXPR(ScriptVariadic, Variadic);
			ScriptVariadic = Variadic;
			UTEST_EQUAL_EXPR(ScriptVariadic, Variadic);

			Variadic.Reset();
			UTEST_INVALID_EXPR(Variadic);

			Variadic = MoveTemp(ScriptVariadic);
			UTEST_INVALID_EXPR(ScriptVariadic);
			UTEST_VALID_EXPR(Variadic);

			return true;
		};

	static const FIntPoint PointTemplate = FIntPoint(52452352, 3146436);
	static const FVector VectorTemplate = FVector(UE_PI, UE_GOLDEN_RATIO, UE_SMALL_NUMBER);
	static const FTransform TransformTemplate = FTransform::Identity;

	UTEST_TRUE_EXPR(ValidateType(PointTemplate));
	UTEST_TRUE_EXPR(ValidateType(VectorTemplate));
	UTEST_TRUE_EXPR(ValidateType(TransformTemplate));

	//Construct FPlane, but read FVector.
	FVariadicStruct BaseVariadic = FVariadicStruct::Make<FPlane>(VectorTemplate);
	UTEST_NOT_NULL_EXPR(BaseVariadic.GetValuePtr<FVector>());
	UTEST_NOT_NULL_EXPR(BaseVariadic.GetMutableValuePtr<FVector>());
	UTEST_EQUAL_EXPR(BaseVariadic.GetValue<FVector>(), VectorTemplate);
	UTEST_EQUAL_EXPR(BaseVariadic.GetMutableValue<FVector>(), VectorTemplate);

	return true;
}

#endif //WITH_TESTS
