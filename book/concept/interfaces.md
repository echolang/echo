# Interfaces

A struct or a class says what it *is*. An interface says what something can **do**.

```echo
interface Drawable
{
    function draw() : void;
    function area() : float64;
}
```

That's a contract and nothing else. The compiler refuses everything you might be tempted to add, and always for
the same reason: a requirement is behavior, not structure.

- **No properties.** A required field would fix every implementor's layout.
- **No bodies.** A method with a body is an implementation. End it with `;` and let implementors write one.
- **No constructor.** An interface is never constructed — a value of one is always some implementor's object.
- **No destructor.** What an erased value holds is torn down by the concrete class's own destructor.
- **No nested types.** An interface is a name and a contract, and holds no declarations of its own.

A type opts in with a `:` clause, and the compiler checks that it delivers:

```echo
struct Square : Drawable
{
    float64 $side;

    function draw() : void {
        echo "square";
    }

    function area() : float64 {
        return $this->side * $this->side;
    }
}
```

Leave out `area` and the error arrives at `Square`, not at some distant call:

```
'Square' says it conforms to 'Drawable' but does not satisfy 'area() : float64' - it declares no 'area'.
```

That location is the point of declaring conformance rather than inferring it. The compiler checks the claim once,
where the author can act on it, and every use site afterwards simply trusts it.

The clause takes interfaces, each at most once, and nothing else:

```echo
struct S : SomeStruct { }    // error: only an interface may appear after ':'
struct S : Drawable, Drawable { }   // error: the list is a set
interface J : Drawable { }   // error: no interface inheritance
```

**This is not inheritance**, which is worth saying plainly because the syntax borrows its punctuation. Nothing is
inherited — no fields, no bodies, no base. The list being a set is what lets the runtime dispatch table be a plain
array, and there's no interface-to-interface edge at all: if `J` needs what `Drawable` requires, `J` declares those
requirements itself.

## What an interface is for

Two things, and they are worth separating because a struct gets one of them and a class gets both.

### 1. A constraint a generic can name

Before interfaces, a type parameter's constraint was a *list of concrete types*:

```echo
function double<T: int32|int64|float64>(T $v) : T { return $v + $v; }
```

That cannot express "anything I can draw" — the set is open, and you would have to edit the list every
time you wrote a new shape. An interface is the capability itself:

```echo
function render<T: Drawable>(T& $shape) : void
{
    $shape->draw();
    echo $shape->area();
}

render($square);
render($circle);
```

**This is static.** `render` is monomorphized once per implementor, exactly as any other generic call is,
so `$shape->draw()` is a direct call and the conformance costs nothing at runtime. A struct pays nothing
at all — no vtable, no indirection, no metadata.

Hand it something that does not conform and the diagnostic names both sides:

```
Type parameter 'T' of 'render' is constrained to 'Drawable' but was given 'Rock'
```

A generic interface is spelled with its arguments, because the bare template is not a type any value
has:

```echo
interface Sized<T>
{
    function first() : T;
}

struct Bag<E> : Sized<E>          // conforms through its own parameter
{
    E $item;
    function first() : E { return $this->item; }
}

function head<C: Sized<int32>>(C& $c) : int32 { return $c->first(); }
```

`Bag<int32>` conforms to `Sized<int32>`, and `Bag<float64>` to `Sized<float64>` — the conformance is
substituted along with the layout.

### 2. A type a class value can have

A class carries runtime metadata, so a class can be **stored** as an interface:

```echo
class Circle : Drawable
{
    float64 $radius;
    function draw() : void { echo "circle"; }
    function area() : float64 { return 3.14159 * $this->radius * $this->radius; }
}

Drawable $shape = Circle(2.0);
$shape->draw();                   // dispatched at runtime
```

Here the static type really is `Drawable`. The call goes through a vtable, `$shape` can be reassigned to
any other conforming class, and it can be passed to a plain (non-generic) parameter:

```echo
function present(Drawable $d) : void
{
    $d->draw();
}
```

An erased value owns a reference, exactly as a class handle does — copying it retains, and it's released at the end
of its scope. The object is destroyed once, when the last handle to it goes, whether that handle was erased or
concrete.

**A class, and only a class.** This is the half of the feature a struct doesn't get:

```echo
Drawable $d = Square(2.0);
// error: 'Square' is a struct, so it cannot be stored as 'Drawable' - a struct carries no runtime
//        type to dispatch through. Take it through a constrained generic instead, e.g.
//        'function f<T: Drawable>(T& $v)'.
```

A struct has no runtime metadata, so there's nothing to dispatch through. That isn't a gap to be filled later —
it's what being a value type means, and the constrained generic in section 1 is the answer that costs nothing
anyway.

One related refusal, at a method rather than at a value: **`#[implicit]` cannot return an interface.** A value
reaches an interface-typed destination by widening, which the compiler already knows how to rank and lower.
Accepting a marker as well would give one conversion two routes, and which one fired would depend on nothing you
wrote. Conformance is declared on the type, and that's the only way in.

## `instanceof`

A class-typed value can be asked which interfaces it answers, and an erased value can be asked which
class is inside it:

```echo
$circle = Circle(1.0);

echo $circle instanceof Drawable;    // true - conformance
echo $circle instanceof Circle;      // true - exact identity

Drawable $shape = $circle;
echo $shape instanceof Drawable;     // true
echo $shape instanceof Circle;       // true - the downcast test
echo $shape instanceof Square;       // false

Circle $none = null;
echo $none instanceof Drawable;      // false - null is an instance of nothing
```

A **struct** on the left is still an error, and it is the same error it always was:

```
'instanceof' needs a class or an interface on the left - a 'Square' carries no runtime type to check
```

That is not an omission. A struct has no header, no identity word and no metadata of any kind — the
question has no answer to give, which is what [value types](value_types.md) already says about them.

## Requiring an operator

An interface can require an **operator**, not only a method:

```echo
interface Comparable<T>
{
    operator (T $a) < (T $b) : bool;
}

struct Money : Comparable<Money>
{
    int32 $cents;
}

// the operator itself is declared at file scope, as every operator is
operator (Money $a) < (Money $b) : bool
{
    return $a->cents < $b->cents;
}
```

The interface holds the *requirement*; the operator itself still lives in the one global operator set,
because an operator is not a member of either of its operand types (see [operators](operators.md)). So
the conformance check looks for it there, and reports against the type:

```
'Money' says it conforms to 'Comparable<Money>' but does not satisfy 'operator <(Money) : bool'
    - no 'operator <' over 'Money' is declared at file scope.
```

An interface that requires an operator can be used as a constraint and with `instanceof`. It **cannot** be used as
a stored type — an operator has no receiver, so there's no slot to dispatch it through.

While we're here: **there is no `Self`.** A requirement that has to mention the implementor takes it as a type
parameter instead, which is exactly why `Comparable<T>` reads the way it does and why `Money` names itself in its
own conformance clause. It costs the compiler nothing and costs you one word, so `Self` has never earned its keep.

## Associated types

Sometimes a requirement's type is not something the *interface* can name — only the implementor knows
it. An iterable hands back a cursor, but every collection has a different one:

```echo
interface contract::iterable<V>
{
    type Iter : contract::iterator<V>;
    function iterate() : Iter;
}
```

`type Iter : contract::iterator<V>` declares an **associated type**: a type the implementor chooses, which the
interface only constrains. The constraint is required — a body reading `Iter` knows nothing else about it, so a bare
`type Iter;` would promise nothing at all and is refused.

Two placement rules go with it.

A `type` has to come *before* any requirement that mentions it. A requirement's signature may name it, so it
has to be a name by the time the compiler gets there.

And a `type` belongs to an interface. A struct or class body can't declare one, because an implementor never
declares its binding — it declares the member that answers the requirement, and the compiler reads the binding
off that.

The implementor never writes `Iter`. It writes the member, and the compiler reads the binding off it:

```echo
struct Bag : contract::iterable<int32>
{
    function iterate() : bag_cursor { ... }     // Iter is inferred to be `bag_cursor`
}
```

Get it wrong and the error names your type, not the interface's placeholder:

```
'Bag' says it conforms to 'contract::iterable<int32>', but 'Iter' would be 'NotACursor',
which does not satisfy 'contract::iterator<int32>'.
```

Note that the constraint is reported **substituted** — `contract::iterator<int32>`, not the `contract::iterator<V>` the
interface's author wrote. `V` is a name you never saw and could not act on.

The inference is one pass: the first member that fits a requirement mentioning `Iter` proposes what it
is, and then *every* requirement is re-checked exactly through that binding. So a second member that
disagrees is an error rather than a silent overwrite, and a proposal that fits one requirement but
breaks another is refused by the ordinary unmet-requirement check.

Using it costs nothing, and needs no way to spell the associated type:

```echo
function total<C: contract::iterable<int32>>(C& $c) : int32
{
    $it = $c->iterate();          // typed from the call. you never name `C::Iter`
    ...
}
```

**An interface with an associated type cannot be stored.** An erased value has forgotten which
implementor it holds, so `iterate() : Iter` has no return type at the use site. Use it as a constraint
— which is what the free, monomorphized path above already is — or erase the `contract::iterator<V>` itself.
