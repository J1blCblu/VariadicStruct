# VariadicStruct

Implementation of `FInstancedStruct` with **SBO** (Small Buffer Optimization) with default buffer size of **24 bytes**.  
Always allocates memory for structures with alignment greater than the alignment of FVariadicStruct.  
Particularly useful when the expected types rarely exceed the buffer size, like FVector.  
Serialization compatible with `FInstancedStruct`.  

`FVariadicStruct` has some key differences from `FInstancedStruct` that should be taken into account:
1. By default, requires **32 bytes** instead of **16** and **16-byte** alignment instead of **8**.
2. Requires extra steps to access the data including **1** branching and **1** indirection if the type doesn't match.
3. Move constructor and move assignment operator require a *copy constructor* for types that fit into the buffer.
4. Not exposed to the Editor and BP as it doesn't make much sense.
 
> [!NOTE]
> `FInstancedStructContainer` might still be more preferable for contiguous heterogeneous data.
