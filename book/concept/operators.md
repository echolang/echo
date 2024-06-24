# Operators

Operators allow you to formulate expressions that perform operations on variables and values.

These include arithmetic, logical, comparison and entirely custom operators.


## Arithmetic Operators

These are used to perform the fundamental mathematical operations like addition, subtraction, multiplication, etc.

```echo
$a = 1 + 2; // 3
```

## Operator Overloading

In Echo you can override the behavior of operators for your own types.

```echo
struct Point {
    public float $x;
    public float $y;
}

operator (Point $a) + (Point $b): Point {
    return Point($a->x + $b->x, $a->y + $b->y);
}

operator (Point $a) + (int $b): Point {
    return Point($a->x + $b, $a->y + $b);
}

$pointA = Point(1.0, 1.0);
$pointB = Point(2.0, 2.0);

$pointC = $pointA + $pointB;

$pointD = $pointA + 2;
```

## Custom Operators

You can define entirely new operators in Echo.

```echo
// returns the average of two numbers
operator (float $a) avg (float $b): float {
    return ($a + $b) / 2;
}

$percentage = 10 avg 20; // 15
```

### Operator Precedence

Operators have a precedence, which determines the order in which they are evaluated.

```echo
// same as above but specifies the precedence and associativity
operator(100, left) (float $a) avg (float $b): float {
    return ($a + $b) / 2;
}
```

### Unary Operators

These operators work on a single operand and can be prefix or suffix.

The precedence of unary operators is higher than that of binary operators.

```echo
operator (int $a)++: int {
    return $a + 1;
}
```

With unary operators it does actually matter if the operator is prefix or suffix.

```echo
operator !!(Str $s): bool {
    return Str::upper($s);
}

echo !!'hello'; // HELLO
```
#### Example String hash

Passing a single int handle around can be very efficent but very inconvenient to use. You can utilize a custom operator to hash string literals to a handle.

If all values are known at compile time, the compiler can optimize the hash to a constant.

```echo
operator (String $s)_handle: int {
    return cheapHashFunction($s);
}

$handle = "my.resource.texture.albedo"_handle;
```

### Example custom units 

This mechanism can be used to define custom units, making some calculations and expressions more human readable.

```echo
struct Distance {
    public uint64 $millimeters;
}

operator (Distance $a) + (Distance $b): Distance {
    return Distance($a->millimeters + $b->millimeters);
}

operator (int $a)mm: Distance {
    return Distance($a);
}

operator (int $a)cm: Distance {
    return Distance($a * 10);
}

operator (int $a)m: Distance {
    return Distance($a * 1000);
}

$distance = 1m + 50cm + 500mm;
```

