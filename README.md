# Variadic Struct

Implementation of `FInstancedStruct` with **SBO** (Small Buffer Optimization) with default buffer size of **24 bytes**.  
Particularly useful when the expected types rarely exceed the buffer size, such as optional payload data.  
**Serialization** compatible (non-commutative) with `FInstancedStruct`.  

`FVariadicStruct` has some key differences from `FInstancedStruct` that should be taken into account:
1. Requires **32 bytes** instead of **16** and **16-byte** alignment instead of **8**.
2. Requires extra steps to access the data including **1** branching and **1** indirection if the type doesn't match.
3. Move constructor and move assignment operator require a *copy constructor* for types that fit into the buffer.
4. Similar to `FInstancedStructContainer`, not exposed to the *Editor* and *BP* as it doesn't make much sense.
 
> `FInstancedStructContainer` might still be more preferable for contiguous heterogeneous data.

# Performance

You should pay attention to how you assign a new structure value into the existing `FVariadicStruct`:
1. `Variadic = FVariadicStruct::Make(MyVector);` Constructs a new structure value from type and copies it using type erasure.
2. `Variadic.InitializeAs<FVector>(MyVector);` Copy constructs a new structure value using native constructor.
3. `Variadic.GetMutableValue<FVector>() = MyVector;` Copies a new structure value into the existing value without reconstructing.

## Specs

> The results are very **approximate** due to the superficiality of the tests performed under *Development* configuration.

| *x* | *type ctor* | *script ctor* | *load (est/seq/rnd)* | *store (est/seq/rnd)* | note |
|:-:|:-:|:-:|:-:|:-:|:-|
| **SBO** | 1.82 | 1.79 | 1.47/1.06/3.21 | 1.24/1.01/2.25 | The difference is greatest with a large number of cache misses and least with sequential access. |
| **HEAP** | 1.02 | 0.97 | 0.99/0.91/1 | 0.92/1.09/1.03 | The values ​​are within the error limits. |
