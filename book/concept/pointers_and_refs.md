# Pointers & References

If you are coming from PHP this one might be a bit uncomfortable at first. But once you get the hang of it, you will see how powerful pointers and references can be.

## References are Pointers

In Echo, references are pointers.

```echo
$var = 10;
$ref = &$var; // referencing $var creates a pointer to $var

// pointers are dereferenced automatically
// and act like the value they point to 
$ref = 20;

debug($ref); // ptr<int32>(int32(20))
debug($var); // int32(20)
```

Pointers are opaque objects in echo, meaning you can't access the objects properties or methods in a normal way.

This means a `int32` can be used in code exactly the same way as a `ptr<int32>`.

### Pointer operations

Even though pointers are opaque objects, you can still do pointer arithmetic. You can use a special operator `:` to access the pointer object itself.

```echo
$var = 10;
$ref = &$var;

debug($ref:) // 0x0000... <- pointer address
```

Using the same semantics you can do pointer arithmetic, check if two pointers point to the same object, check if a pointer is `null` and so on.

```echo
$var = 10;
$ref = &$var;
$ref2 = &$var;

ptr<int32> $empty: = null;

// checks if the address of $ref is the same as $ref2 not the value
debug($ref: == $ref2:); // true

// checks if the pointer is null
debug($empty: == null); // true
```

### Heap Allocation

Echo allows you to allocate a blob of memory on the heap and get a pointer to it. This obviosly is not a safe operation and should be used with caution.

```echo
ptr<uint8> $ptr = mem::alloc(4); // allocate 4 bytes of memory

$ptr:[0] = 1;
$ptr:[1] = 2;
$ptr:[2] = 3;

// copy the potiner
$it = ptr<uint8>($ptr:);

debug($it:[0]); // uint8(1)
debug($it:[1]); // uint8(2)
debug($it:[2]); // uint8(3)

$it:++;

debug($it:[0]); // uint8(2)
debug($it:[1]); // uint8(3)
```

#### Type indexed

You can also index the pointer by type. This will automatically calculate the offset based on the type size.

```echo
$ptr = mem::alloc<int32>(4); // allocate 16 bytes of memory

$ptr:[0] = 424242;
$ptr:[1] = 696969;

debug($ptr:[0]); // int32(424242)
debug($ptr:[1]); // int32(696969)
```

