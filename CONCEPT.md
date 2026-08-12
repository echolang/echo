# Echo Lang

Echo is a statically typed, natively compiled, general-purpose programming language.

Welcome to my highly opinionated and far from production-ready version of PHP that goes brrrr.

Echo takes a lot of inspiration from modern PHP. However, to be clear, you cannot simply write PHP code and expect it to work. Echo has some fundamental differences but is designed to be easily picked up by PHP developers.

## Language 

Echo is a statically typed language and there is no support for dynamic typing or union types.

```echo
function multiply(int $a, int $b): int {
    return $a * $b;
}

echo multiply(25, 25) . "\n"; // 625
```

Still feels like home, right? Also note the `<?php` tag is gone. Echo is not a templating language and does not do what PHP initially was only intended to do.

### Variables

Variables are declared just the way you would expect them to be from PHP, but with a catch. Variables have a static type which is determined at compile time. This means that you cannot change the type of a variable after it has been declared.

```echo
$a = 25; // works fine
$b = "Hello World"; // works fine
```

What you cannot do is declare a variable with an unknown type. In the example above, the type is determined by the value assigned to the variable. In this case, the `int` and `string` literal.

```echo
$a;
$a = 25; // invalid;

int $b;
$b = 25; // valid
```

#### Const Variables

Variables can be declared as `const` which means they cannot be changed after they have been declared.

```echo
const $a = 25;
$a = 50; // error
```

#### Data Types

From PHP you are probably used to the following data types `int`, `float`, `string`, `bool`, `array`, `object`, `resource`, `null`. Echo works a bit differently, only scalar types are supported. Everything else is an object.

Also, an `int` in PHP is a 64-bit integer, in Echo it is 32-bit by default.

```echo
int $a = 42; 
int8 $b = 42; // 8-bit integer
int16 $c = 42; // 16-bit integer
int32 $d = 42; // 32-bit integer (default)
int64 $e = 42; // 64-bit integer

float $f = 42.0; // 64-bit float
float32 $g = 42.0; // 32-bit float
float64 $h = 42.0; // 64-bit float (default)
```

Unsigned integers are also supported.

```echo
uint $a = 42;
uint8 $b = 42; // 8-bit unsigned integer
uint16 $c = 42; // 16-bit unsigned integer
uint32 $d = 42; // 32-bit unsigned integer (default)
uint64 $e = 42; // 64-bit unsigned integer
```

#### Block var declaration

The initalizer value of a variable can also be declared in a block.

```echo
$randomNumber = {
    $rng = new RandomNumberGenerator();
    $rng->seed(42);
    return $rng->next();
};
```


### Arrays / Container types

Arrays are where things get a bit different. In PHP arrays are a special type of hash map that can contain any type of value. In Echo, arrays are container objects that can only contain a single type of value. 

```echo
$numbers = [1, 2, 3, 4, 5]; // allowed

// will not compile:
$numbers = [1, 2, 3, 4, "Hello World"];
```

As arrays are objects, they can hold methods and properties just like any other object. This means many functions you know from the standard library are to be called on the array object directly.

```echo
$numbers = [5, 4, 3, 2, 1];
echo $numbers->count() . "\n"; // 5
echo $numbers->pop() . "\n"; // 1
echo $numbers->count() . "\n"; // 4
```

When the array's type cannot be determined at the declaration of the variable, the type can be specified:

```echo
array<int> $numbers = [];
for($i = 0; $i < 10; $i++) {
    $numbers[] = $i;
}
```

#### Maps

Maps or dictionaries allow you to store and retrieve values by a key. In PHP maps are just arrays with string keys. In Echo, maps are a special type of container object that can only contain a single type of value.

```echo
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

Also here, the type can be specified:

```echo
Map<string, string> $airports = [];
$airports['LHR'] = "London Heathrow";
$airports['CDG'] = "Paris Charles de Gaulle";
$airports['JFK'] = "New York John F. Kennedy";
```

### Entry Point

As this is a compiled language, there is no `<?php` tag. Instead, the entry point is a function called `main` that is called when the program starts.

```echo
function main(array<string> $argv): int {
    echo "Hello World\n";
    return 0;
}
```

### Functions

Functions are declared just like you would expect them to be from PHP.

```echo
function add(int $a, int $b): int {
    return $a + $b;
}
```

One difference is that the return type is required. The only exception to this is `void`, which is the default return type.

```echo
function doSomething(): void {
    echo "Hello World\n";
}

// same as 
function doSomething() {
    echo "Hello World\n";
}
```

#### Named arguments ^1

Named arguments are supported.

```echo
function lerp(Point $from, Point $to, float $alpha): Point {
    // ...
}

lerp(Point(0.0, 0.0), Point(1.0, 1.0), 0.5); // works

lerp(from: Point(0.0, 0.0), to: Point(1.0, 1.0), alpha: 0.5); // also works

lerp(Point(0.0, 0.0), Point(1.0, 1.0), alpha: 0.5); // also works
```

This can become really useful when you have a function with a lot of arguments.

```echo
function printPerson(
    ?string $firstName = null,
    ?string $lastName = null,
    ?int $age = null,
    ?string $address = null,
    ?string $city = null,
    ?string $country = null,
    ?string $zipCode = null
): void {
    if ($firstName) echo "First Name: " . $firstName . "\n";
    if ($lastName) echo "Last Name: " . $lastName . "\n";
    if ($age) echo "Age: " . $age . "\n";
    // ...
}

printPerson(firstName: "John", lastName: "Doe", country: "USA");
printPerson(firstName: "Kim", age: 42, zipCode: "CH-8000");
```

You can force named arguments by using the `named` keyword.

```echo
function listen(named string $forEvent, function<void()> $callback): void {
    // ...
}

listen(forEvent: "click", function() {
    echo "Clicked\n";
});
```

Why would you want to force named arguments? They become part of the function signature, meaning you can have multiple functions with the same name but different named arguments.

```echo
function print(named int $fromDecimal, named string $currency): void {
    echo round($fromDecimal / 100) . "{$currency}\n";
}

function print(named int $fromMicro, named string $currency): void {
    echo round($fromMicro / 1000000) . "{$currency}\n";
}

print(fromDecimal: 100, currency: "€"); // 1€
print(fromMicro: 1000000, currency: "€"); // 1€
```

This would not be possible without the `named` keyword. Because the function signature would be the same.

```echo
print(int, string); // signature without named arguments
print(fromDecimal: int, currency: string); // signature with named arguments
```

So when a call is made the compiler will search for a function with a matching signature:

```echo
print(100, "€"); 
// no matching function found, signature search was:
// - print(int, string)

print(fromDecimal: 100, currency: "€");
// will match, signature search was:
// - print(fromDecimal: int, currency: string)

print(500, currency: "€");
// no matching function found, signature search was:
// - print(int, currency: string)
```

If I were to declare the following print function:

```echo
print(int $logLevel, string $message) {
    // ...
}
```

This would expose the following function signatures:

 * `print(int, string)`
 * `print(logLevel: int, string)`
 * `print(int, message: string)`
 * `print(logLevel: int, message: string)`

This means I could not declare the following function after the one above:

```echo
print(int $applicationId, string $message) { // will not compile, duplicate signature "print(int, string)"
    // ...
}

// below will compile, signature is "print(applicationId: int, string)" and "print(applicationId: int, message: string)"
// both signatures are unique
print(named int $applicationId, string $message) {
    // ...
}
```

#### Multiple return values ^1

Functions can return multiple values.

```echo
function getPerson(): (string, string, int) {
    return ("John", "Doe", 42);
}

($firstName, $lastName, $age) = getPerson();

echo $firstName . "\n"; // John
```

These can be named as well.

```echo
function getPerson(): (firstName: string, lastName: string, age: int) {
    return (firstName: "John", lastName: "Doe", age: 42);
}

(firstName: $firstName, lastName: $lastName, age: $age) = getPerson();

echo $firstName . "\n"; // John
```

And be used as an unnamed type.

```echo
function getPerson(): (firstName: string, lastName: string, age: int) {
    return (firstName: "John", lastName: "Doe", age: 42);
}

$person = getPerson();

echo $person->firstName . "\n"; // John
```

### Generics

Generics are supported for classes and functions. The following example shows how to create a generic class.

```echo
class Stack<T> {
    private array<T> $items = [];

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

```echo
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

```echo
FixedArray<int, 10> $numbers = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
```

#### Structs

Structs are like classes in a sense, but they are passed by value instead of by reference. They are also not allowed to have methods and are allocated on the stack instead of the heap.

```echo
struct Point {
    public float $x;
    public float $y;
}

$point = Point(1.0, 1.0);
```

This means when they for example are stored in an array there is no pointer indirection happening. This can lead to a performance boost in some cases due to better cache locality.

```echo
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

Unsafe pointers, aka raw pointers, can be used to access memory directly, this can be useful in the right hands but also be very dangerous, hence the name.

### Standard Library

The standard library is still very much a work in progress, but I'm trying to have a more modern approach to it. The standard library is split into modules/namespaces.

```echo
use Echo\Math;

echo Math\abs(-42); // 42
echo Math\round(42.5); // 43
echo Math\floor(42.5); // 42
```

You can also import specific functions from a module/namespace.

```echo
use Echo\Math\{abs};

echo abs(-42); // 42
```

### Namespaces

A namespace keeps names apart; it says nothing about who may reach them. **Visibility is about modules and
files, not namespaces**: a declaration belongs to its own module unless it says `public`, `private` narrows it
to one file, and `internal` is the default written out. Inside a type, `private` means the type itself.

```echo
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

```echo
namespace MyModule\Logging;

public const LOG_LEVEL_DEBUG = 0;
public const LOG_LEVEL_INFO = 1;
public const LOG_LEVEL_WARNING = 2;

public array<string> $logEntries = [];
```

Which can then be imported like this:

```echo
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

### Memory Management

#### Copy Types

Basic scalar types like `int`, `float`, `bool` are copy types. This means that when they are passed to a function or assigned to a variable they are copied.

Example:

```echo
$a = 42;
$b = $a; // $b is now a copy of $a
$b = 50;

echo $a;// 42
```

You can reference a copy type by using the `&` operator.

```echo
$a = 42;
$b = &$a; // $b is now a reference to $a
$b = 50;

echo $a;// 50
```

#### Reference Types

Reference types are a bit more complicated as they give you more control over how memory is managed compared to how PHP does it.

The default behavior is just the same as PHP, when you assign a reference type to a variable or pass it to a function it is passed by reference. Internally we use reference counting to manage memory. This means that when the last reference to an object is removed it is automatically freed.

Alternative to that you can construct an object as unique, this means that it is not reference counted and will be freed when it goes out of scope.

```echo
class Car 
{
    public function makeNoise(): void {
        echo "Vroom\n";
    }
}


$foo = new Car();
$bar = $foo; // $bar and $foo now point to the same object
$bar->makeNoise(); // Vroom
```

You can construct an object as unique by using the `unique` keyword.

```echo
class Car 
{
    public function makeNoise(): void {
        echo "Vroom\n";
    }
}

$foo = Car(); // $bar and $foo now point to different objects
$bar = $foo; // $bar is now the owner of the object

$bar->makeNoise(); // Vroom
$foo->makeNoise(); // Error, ownership has been transferred to $bar
```

Echo uses reference counting to manage memory.

```echo
class Car 
{
    public function makeNoise(): void {
        echo "Vroom\n";
    }
}

//        $bar is a heap object which is reference counted
//                    |
function doSomething(Bar $bar): void {
    $bar->makeNoise();
}

$foo = new Bar();
$bar = $foo; // $bar and $foo now point to the same object
doSomething($foo); // Vroom
```


```echo
// a struct is almost the same as a class, but there is a key difference
// in its mutability. A struct is implicitly copied when mutated.
struct String {
    uint64 $size;
    array<uint8> $data;
}

class String2 {
    uint64 $size;
    array<uint8> $data;
}

$stackString = String("Hey Whats up"); // allocated on the stack 

// NOTE: `&` over a *class* does not give `String2&` - it gives `weak<String2>`, a reference that does
// not keep the object alive, and reading one needs `strong(...)`, `guard`, `??` or `?->`. this section
// is the original sketch; docs/memory/ownership.md and docs/memory/nullability.md are the
// current specification of both
$arcString = String2("Something else"); // a class: reference counted, on the heap. there is no `new`

// a struct is borrowed with `&`, which gives `String&`. a *class* is different - see the note below
$strRef1 = &$stackString;
$strRef2 = &$arcString;

// auto references when just beeing read.
// the compiler implicitly converts this to print(const String& $string)
function print(String $string) : void {
    echo $string;
}

// because the string is mutated inside the function scope, it is implicily copied before the linebreak is appended
// the implicit copy is only created because the "String" type is a struct.
function printLn(String $string) : void {
    echo $string->append("\n");
}

// move the string into the function, the function scope becomes the owner of $string
function printErr(mv String $string) : void {
    // ...
}

// stack behavior
print($stackString); // implicit reference 
print($strRef1); // explicit reference

// heap behavior
print($arcString); // implicit reference: a class handle borrows as `String2&`
print($strRef2); // explicit reference

// stack behavior
printLn($stackString); // implicit reference, implicit copy inside the function scope
printLn($strRef1); // explicit reference, implicit copy inside the function scope

// heap behavior
printLn($arcString); // implicit reference, implicit stakc copy inside the function scope
printLn($strRef2); // explicit reference, implicit stack copy inside the function scope


array<String> $logs = [];
function logString(String $string) : void {
    $logs[] = $string;
}

```

Scrap the above... It would mix types and require duplicate function implementations and make it hard to reason
which function is acutally used.. I want it simpler than that.

To fix this an object symbol must be either a stack or rc heap object during compile time.

```echo
// stack object 
struct String {
    uint64 $size;
    array<uint8> $data;
}

// reference counted heap object
class String2 {
    uint64 $size;
    array<uint8> $data;
}

$stackString = String("Hey Whats up");

// will auto convert to print(const String &$string)
function print(String $string) : void {
    echo $string;
}

function printLn(String $string) : void {
    echo $string->append("\n"); // implicit copy
}

function printErr(mv String $string) : void {
    // ...
}

// stack behavior
print($stackString); // implicit reference
printLn($arcString); // implicit reference, implicit copy inside the function scope
printErr($stackString); // takes ownership of the stack object

// now our string2 RC object
$arcString = new String2("Something else"); 

// passes the reference counted object 
function print(String2 $string) : void {
    echo $string;
}

// the string is mutated inside the function scope, no copy is created because the "String2" type is a class.
function printLn(String2 $string) : void {
    echo $string->append("\n");
}

// move the string into the function, the function scope becomes the owner of the counted reference object
function printErr(mv String2 $string) : void {
    // ...
}

// heap behavior
print($arcString); 
printLn($arcString);
printErr($arcString); // takes ownership of the reference counted object
```

Because structs do not have any runtime meta data they cannot be nulled or checked for nullability.
You can also not perform any runtime reflection checks on them.

```echo
struct A {
    int $a = 42;
}

class B {
    int $b = 420;
}

$struct = A();
$class = new B();

echo $class instanceof B; // true 
echo $class instanceof A; // false

echo $struct instanceof A; // error 
```


### Static functions and properties

Static function in echo can only be defined inside a class or struct. They are called on the class or struct itself and not on an instance of the class or struct.

```echo
class Session {
    static int32 $count = 0;

    public string $user;

    construct(string $user) {
        $this->user = $user;
        Session::$count = Session::$count + 1;
    }

    static function active(): int32 {
        return Session::$count;
    }
}

$a = Session('mario');
$b = Session('ada');

echo Session.active(); // 2
```

### Shorthand Constructors

In echo `.` is not used for concatenation, instead it is used to call a constructor when the required type can be inferred from the context.

```echo
struct Point {
    public float $x;
    public float $y;

    static function norm(float $x, float $y): Point {
        // create a normalized point
        $length = std::math::sqrt($x * $x + $y * $y);
        return Point($x / $length, $y / $length);
    }
}

function draw(Point $point) : void {
    // ...
}

draw(.norm(10, 20)); // calls Point::norm(10, 20) and passes the result to draw()
```

This also applies to return values 

```echo
struct Response {
    public int $statusCode;
    public string $body;

    static function ok(string $body): Response {
        return Response(200, $body);
    }

    static function notFound(string $body): Response {
        return Response(404, $body);
    }
}

function handleRequest(string $url): Response {
    if ($url == "/") {
        return .ok("Hello World");
    } else {
        return .notFound("Not Found");
    }
}
```

### Enums

A enum is what you expect a enum to be:

```php
enum DistanceUnit {
    case meter;
    case kilometer;
    case mile;
}
```

A enum can extend a type / associate a value with each case:

```php
enum DistanceUnit : string {
    case meter = "m";
    case kilometer = "km";
    case mile = "mi";
}
```

A enum can be compared.

```php
$unit = DistanceUnit::meter;
if ($unit == DistanceUnit::meter) {
    echo "Unit is meter\n";
}
if ($unit != DistanceUnit::kilometer) {
    echo "Unit is not kilometer\n";
}
```

You can use the enums value if needed

```php
$unit = DistanceUnit::meter;
echo $unit->value; // m
```

A enum can carry a value beyond the case value. 

```php
enum DistanceUnit {
    case meter(int $value);
    case kilometer(int $value);
    case mile(int $value);
}

$unit = DistanceUnit::meter(100);
match ($unit) {
    DistanceUnit::meter($value) => echo "Unit is meter with value {$value}\n",
    DistanceUnit::kilometer($value) => echo "Unit is kilometer with value {$value}\n",
    DistanceUnit::mile($value) => echo "Unit is mile with value {$value}\n",
}
```

A `match` is an expression, so the arms can produce a value instead of doing the
work themselves:

```php
$meters = match ($unit) {
    DistanceUnit::meter($value) => $value,
    DistanceUnit::kilometer($value) => $value * 1000,
    DistanceUnit::mile($value) => $value * 1609,
};
```

Or, when you already know which case you are holding, you can read the payload
directly:

```php
$unit = DistanceUnit::meter(100);
echo $unit->value; // 100
```

Reading it off the wrong case is a compile error, so a case you are not sure
about has to go through `match` — or a single-case check, which binds the
payload only where it is known to exist:

```php
if ($unit is DistanceUnit::kilometer($km)) {
    echo "{$km} km\n";
}
```


### Errors & Error Handling

```php
enum CurlError : error {
    case cannot_resolve_host;
    case cannot_connect;
    case timeout(int $after_seconds);
    case unknown;
}

struct CurlResult : result<CurlResponse, CurlError> {}

function request(string $url): CurlResult {
    // ...
    if (...) {
        return .error(.timeout(30));
    }

    return .ok($response);
}
```