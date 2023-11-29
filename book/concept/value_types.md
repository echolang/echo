# Value Types

In echo we have next to familiar scalar value types like `int`, `float`, `bool` also complex value types which can hold multiple values. 
 
Value types are copied when assigned to a variable or passed to a function.


```php
$a = 1;
$b = $a; // $b is a copy of $a
$b = 2; // $a is still 1
echo $a; // 1
```

The complex value types can be constructed similar to classes with the `struct` keyword.

```php
struct Point {
    float $x;
    float $y;
}
```

These `struct` types are fundamentally different from classes, they are value types, in echo this means:
 * they allocate on the stack, and are therefore destroyed when they go out of scope.
 * they are copied by default. 
 * they can be moved, which is a cheap operation.


Structs are constructed without the `new` keyword:

```php
$a = Point(x: 1.0, y: 2.0);
$b = $a; // $b is a copy of $a
$b->x = 3.0; // $a is still (1.0, 2.0)
```

The copy behavior also applies to function arguments:

```php
function squared(Point $p) : Point {
    $p->x = $p->x * $p->x;
    $p->y = $p->y * $p->y;
    return $p;
}

$squared = squared($a); // $a is copied into $p
```

## Ownership & Movement

Value types have a single owner, when the owner goes out of scope the value type is destroyed. 

This ownership can be transferred to another variable, this is called moving.

```php
$a = Point(x: 1.0, y: 2.0);
$b = mv $a; // $b is now the owner of the value type, $a becomes unset
```

This seems like a silly example, but it becomes more useful when combined with functions and complex value types.