# Expressions

## Literals 

A literal is a fixed value that is written directly in the code. The literal will directly determine the type of the value.

```php
$someInt = 1; // int
$someDouble = 1.0; // double
$someFloat = 1.0f; // float
```

A literal will be auto converted to the type its expected to be in by the context.

Note: If that operation will causes a loss of precision a compile time error will be thrown. (This error checking only applies to literals)

```php
int64 $someInt = 1.0; // works
int64 $someInt = 1.1; // compiler error

float $someFloat = 3.14; // works
double $someDouble = 3.14f; // works if there is no loss of precision
```

Literals will be converted at compile time and not at runtime.

## Implicit Type Conversion

echo will make assumptions about the type of a value based on the context in which it is used.

I understand that this is controversial to some, but I believe it greatly simplifies the language and experience using it.
Also its not like the converstions are semi random, they follow a strict set of rules.

### Floating point values win

Any operation that involves a floating point value will result in a float of the same type.

```php
echo 3.14f * 2; // float 6.28

// will be auto converted to
echo 3.14f * 2.0f 
```

Remember that without the `f` suffix we consider the value to be a double.

```php
echo 3.14 * 2; // double 6.28

// will be auto converted to
echo 3.14 * 2.0;
```

#### Higher precision wins

To avoid loss of precision, the higher precision type will be used.

```php
echo 3.14f * 2.0; // double 6.28

// will be auto converted to
echo 3.14 * 2.0;
```

This logic will apply to all arithmetic infix operators.

```php
echo 3.14f * 2 + 10;

// will be auto converted to
echo (3.14f * 2.0f) + 10.0f
```

### Expected types

If the context expects a specific type, the value will be converted to that type. Depending on the situation this will
happen at compile time or runtime.

```php
int $myNumber = 1.0; // int 1
```

Because 1.0 can be logically converted to an int, the compiler will do so at compile time.

Now if you have an expression the expected type will influence how the expression types are converted.

```php
$multiplier = 2;
float $val = 3.14 * $multiplier; // float 6.28

// will be auto converted to
float $val = 3.14f * cast<float>($multiplier);
```

Because `$multiplier` is a variable wich we can't modify for the sole purpose of this operation, a runtime conversion will be performed.
_In this specific scenraio an optmizer might optimize this away at compile time, but for the sake of the example lets assume it doesn't._

This means when the expected type is a float all literals in the expression will be converted to a float and vice versa.

```php
int $val = 3.0 * 2 + 5.000; // int 6

// will be auto converted to
int $val = (3 * 2) + 5
```

Non literal values will be converted at runtime to fit the expected type.

```php
function doubleMyValue(int $val): int {
    return $val * 2;
}

float $a = 5.0;
float $val = doubleMyValue($a * 3); // float 30.0

// will be auto converted to
float $val = cast<float>(doubleMyValue(cast<int>($a) * 3));
```


```php
$intVal = 1;
$floatVal = 1.0;

// performaing an arithmetic operation with an int and a float
// will impliclitly convert the int to a float and return a float
//   - float $sum = copy<float>($intVal) + $floatVal
$sum = $intVal + $floatVal; // 2.0

// except when the reciever of the operation has a datatype declared
//   - int $intSum = $intVal + copy<int>($floatVal)
int $intSum = $intVal + $floatVal; // 2
```

