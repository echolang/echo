# Echo Lang

Echo is a general-purpose programming language and a dialect of PHP that transpiles to C++ and is then compiled into a native binary.

Welcome to my highly opinionted and FAR from production ready version of PHP that goes fast.

To be clear, you cannnot just write PHP code and expect it to work. Echo does a lot of things differently but is desigend to be easly picked up by PHP developers.

This is not a new idea at all, and has also been tried before, most prominently by [Hack](https://hacklang.org/). One of the main differences between Echo and Hack is that this project does not aim to be a drop-in replacement for PHP, but rather a new language that is inspired by PHP. I really want to create a LLVM frontend at some point, but for now transpiling to c++ is a much more reachable target. 

## Language 

Echo is a statically typed language and there is no support for dynamic typing or union types.

```php
function multiply(int $a, int $b): int {
    return $a + $b;
}

echo multiply(25, 25) . "\n"; // 625
```

Still feels like home right? also note the `<?php` tag is gone. Echo is not a templating language and does not do what PHP initally was only intended to do.

### Variables

Variables are declared just the way you would expect them to be from PHP but with a catch. Variables have a static type which is determined at compile time. This means that you cannot change the type of a variable after it has been declared.

```php
$a = 25; // works fine
$b = "Hello World"; // works fine
```

What you cannot do is declare a variable with an unkonwn type. In the example above the type is determined by the value assigned to the variable. In this case the `int` and `string` literal.

```php
$a;
$a = 25; // invalid;

int $b;
$b = 25; // valid
```

#### Const Variables

Variables can be declared as `const` which means they cannot be changed after they have been declared.

```php
const $a = 25;
$a = 50; // error
```

#### Data Types

From PHP you are probably used to the following data types `int`, `float`, `string`, `bool`, `array`, `object`, `resource`, `null`. Echo works a bit differently, only scalar types are supported. Everything else is an object.

Also an `int` in PHP is a 64 bit integer, in Echo it is 32 bit by default.

```php
int $a = 42; 
int8 $b = 42; // 8 bit integer
int16 $c = 42; // 16 bit integer
int32 $d = 42; // 32 bit integer (default)
int64 $e = 42; // 64 bit integer

float $f = 42.0; // 64 bit float
float32 $g = 42.0; // 32 bit float
float64 $h = 42.0; // 64 bit float (default)
```

Unsigned integers are also supported.

```php
uint $a = 42;
uint8 $b = 42; // 8 bit unsigned integer
uint16 $c = 42; // 16 bit unsigned integer
uint32 $d = 42; // 32 bit unsigned integer (default)
uint64 $e = 42; // 64 bit unsigned integer
```

### Arrays / Container types

Arrays are where things get a bit different. In PHP arrays are a special type of hash map that can contain any type of value. In Echo arrays are container objects that can only contain a single type of value. 

```php
$numbers = [1, 2, 3, 4, 5]; // allowed

// will not compile:
$numbers = [1, 2, 3, 4, "Hello World"];
```

As arrays are objects they can hold methods and properties just like any other object. This means many functions you know from the standard library are to be called on the array object directly.

```php
$numbers = [5, 4, 3, 2, 1];
echo $numbers->count() . "\n"; // 5
echo $numbers->pop() . "\n"; // 1
echo $numbers->count() . "\n"; // 4
```

When the arrays type cannot be determined at the declartion of the variable, the type can be specified:

```php
Array<int> $numbers = [];
for($i = 0; $i < 10; $i++) {
    $numbers[] = $i;
}
```

#### Maps

Maps or dictonaries are allow you to restore and retrieve values by a key. In PHP maps are just arrays with string keys. In Echo maps are a special type of container object that can only contain a single type of value.

```php
// will work
$airports = [
    "LHR" => "London Heathrow",
    "CDG" => "Paris Charles de Gaulle",
    "JFK" => "New York John F. Kennedy"
];

$airports['LHR']; // "London Heathrow"

// will not work
$airports['LHR'] = 42; // error
```

Also here the type can be specified:

```php
Map<string, string> $airports = [];
$airports['LHR'] = "London Heathrow";
$airports['CDG'] = "Paris Charles de Gaulle";
$airports['JFK'] = "New York John F. Kennedy";
```

### Entry Point

As this is a compiled language, there is no `<?php` tag. Instead the entry point is a function called `main` that is called when the program starts.

```php
function main(Array<string> $argv): int {
    echo "Hello World\n";
    return 0;
}
```

### Generics

Generics are supported for classes and functions. The following example shows how to create a generic class.

```php
class Stack<T> {
    private Array<T> $items = [];

    public function push(T $item): void {
        $this->items->push($item);
    }

    public function pop(): T {
        return $this->items->pop();
    }
}
```


### Operator Overloading

Operator overloading is supported for the following operators:

```php
class Point {
    public float $x;
    public float $y;
}

operator +(Point $a, Point $b): Point {
    return new Point($a->x + $b->x, $a->y + $b->y);
}

operator +(Point $a, int $b): Point {
    return new Point($a->x + $b, $a->y + $b);
}

$pointA = new Point(1.0, 1.0);
$pointB = new Point(2.0, 2.0);

$pointC = $pointA + $pointB;

$pointD = $pointA + 2;
```

### Fixed Arrays

These represent actual arrays in the sense that they are a contiguous block of memory. They are fixed in size and cannot be resized. 

```php
FixedArray<int, 10> $numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
```

#### Structs

Structs are like classes in a sense, but they are passed by value instead of by reference. They are also not allowed to have methods.
And are allocated on the stack instead of the heap.

```php
struct Point {
    public float $x;
    public float $y;
}

$point = Point(1.0, 1.0);
```

This means when they for example are stored in an array there is no pointer indirection happening, this 
can lead to a performance boost in some cases due to better cache locality.

```php
$points = FixedArray<Point, 10>();
for($i = 0; $i < 10; $i++) {
    $points[] = Point($i, $i);
}

$points[0]->x = 42.0; // possible
$points[0]->y = 42.0; // possible
$points[1] = Point(42.0, 42.0); // also possible


$pointCopy = $points[0]; // makes a copy of the point

$pointRef = &$points[0]; // makes a reference to the point
```

### Unsafe Pointers

Unsafe pointers aka raw pointers can be used to access memory directly, this can be useful in the right hands but also be very dangerous, hence the name.

```php
```

### Standard Library

The standard library is still very much a work in progress, but im trying to have a more modern approach to it. The standard library is split into modules / namespaces.

```php
use Echo\Math;

echo Math\abs(-42); // 42
echo Math\round(42.5); // 43
echo Math\floor(42.5); // 42
```

You can also import specific functions from a module / namespace.

```php
use Echo\Math\{abs};

echo abs(-42); // 42
```

### Namespaces

To make a class or a function available to other namespaces you have to declare it as `public`.

```php
namespace MyModule\Logging;

public function makeLogger(): Logger {
    return new Logger();
}

public class Logger {
    public function log(int $level, string $message): void {
        echo '[' . $level . '] ' . $message . "\n";
    }
}
```

You can also extern values and even variables from other namespaces.

```php
namespace MyModule\Logging;

public const LOG_LEVEL_DEBUG = 0;
public const LOG_LEVEL_INFO = 1;
public const LOG_LEVEL_WARNING = 2;

public Array<string> $logEntries = [];
```

Which can then be imported like this:

```php
namespace MyModule;

use MyModule\Logging\{Logger, makeLogger};
use MyModule\Logging\{
    LOG_LEVEL_DEBUG,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING
};

use MyModule\Logging\$logEntries;

function doSomething(): void {
    $logger = makeLogger();
    $logger->log(LOG_LEVEL_DEBUG, "Debug message");
    $logEntries[] = "Debug message";
}
```

