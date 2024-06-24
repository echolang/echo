# Expressions

## Literals 

A literal is a fixed value that is written directly in the code. The literal will directly determine the type of the value.

```echo
$someInt = 1; // int32
$someDouble = 1.0; // float64 aka double
```

Almost all native types have a literal representation.

```echo
$someFloat = 1.0f; // float32

// ints 
$myInt = 420i32; // int32(420)
$myUnsignedInt = 420u32; // uint32(420)
$myLong = 1i64; // int64(1)
$myUnsignedLong = 1u64; // uint64(1)
$myShort = 1i16; // int16(1)
$myUnsignedShort = 1u16; // uint16(1)
$myByte = 1i8; // int8(1)
$myUnsignedByte = 1u8; // uint8(1)
```

A literal will be auto-converted to the type it's expected to be in by the context.

Note: If that operation causes a loss of precision, a compile-time error will be thrown. (This error checking only applies to literals)

```echo
int64 $someInt = 1.0; // works
int64 $someInt = 1.1; // compiler error

float $someFloat = 3.14; // works
double $someDouble = 3.14f; // works if there is no loss of precision
```

Literals will be converted at compile time and not at runtime.

## Implicit Type Conversion

Echo will make assumptions about the type of a value based on the context in which it is used.

I understand that this is controversial to some, but I believe it greatly simplifies the language and the experience of using it.
At the cost of some potential for unexpected behavior, also, it's not like the conversions are semi-random; they follow a strict set of rules.

### Floating point values win

Any operation that involves a floating point value will result in a float of the same type.

```echo
echo 3.14f * 2; // float 6.28

// will be auto converted to
echo 3.14f * 2.0f 
```

Remember that without the `f` suffix we consider the value to be a double.

```echo
echo 3.14 * 2; // double 6.28

// will be auto converted to
echo 3.14 * 2.0;
```

#### Higher precision wins

To avoid loss of precision, the higher precision type will be used.

```echo
echo 3.14f * 2.0; // double 6.28

// will be auto converted to
echo 3.14 * 2.0;
```

This logic will apply to all arithmetic infix operators.

```echo
echo 3.14f * 2 + 10;

// will be auto converted to
echo (3.14f * 2.0f) + 10.0f
```

### Expected types

If the context expects a specific type, the value will be converted to that type. Depending on the situation, this will happen at compile time or runtime.

```echo
int $myNumber = 1.0; // int 1
```

Because 1.0 can be logically converted to an int, the compiler will do so at compile time.

Now, if you have an expression, the expected type will influence how the expression types are converted.

```echo
$multiplier = 2;
float $val = 3.14 * $multiplier; // float 6.28

// will be auto converted to
float $val = 3.14f * float($multiplier);
```

Because `$multiplier` is a variable which we can't modify for the sole purpose of this operation, a runtime conversion will be performed. 
_In this specific scenario, an optimizer might optimize this away at compile time, but for the sake of the example, let's assume it doesn't._

This means when the expected type is a float, all literals in the expression will be converted to a float and vice versa.

```echo
int $val = 3.0 * 2 + 5.000; // int 6

// will be auto converted to
int $val = (3 * 2) + 5
```

Non-literal values will be converted at runtime to fit the expected type.

```echo
function doubleMyValue(int $val): int {
    return $val * 2;
}

float $a = 5.0;
float $val = doubleMyValue($a * 3); // float 30.0

// will be auto converted to
float $val = float(doubleMyValue(int($a) * 3));
```

If the types are incompatible, a compile-time error will be thrown.

```echo
uint8 $myVal = 10 + -1; // compile-time error, because "-1" cannot be converted to uint8
```

This is also true for non literal values, but no overflow / underflow checks will be performed.

```echo
uint8 $a = 10;
uint8 $b = 1;

$val = $b - $a; // results in a (uint8) if $a and $b would not be literals this would underflow without error
```

While this is valid code, echo will throw a warning that the implicit conversion might cause a loss of precision.



```echo
$intVal = 1;
$floatVal = 1.0;

// performing an arithmetic operation with an int and a float
// will implicitly convert the int to a float and return a float
//   - float $sum = copy<float>($intVal) + $floatVal
$sum = $intVal + $floatVal; // 2.0

// except when the receiver of the operation has a datatype declared
//   - int $intSum = $intVal + copy<int>($floatVal)
int $intSum = $intVal + $floatVal; // 2
```