# Maps

> This chapter assumes [Ownership & Moving](ownership_and_moving.md), which is where copying, moving and
> destruction are defined, and it leans on [Arrays](arrays.md) — a map is the same bargain with a key
> instead of an index. It spends those rules rather than restating them.

In PHP there is one container and it is a map: an array with string keys, holding anything. In Echo a map
holds one key type and one value type, and — this is the part carried over from arrays — it's a value that
you own.

## Two types, two promises

```echo
map<string, int32> $counts = map<string, int32>();
ordered_map<string, int32> $steps = ordered_map<string, int32>();
```

| Type | Iteration order | Costs |
|---|---|---|
| `map<K, V>` | **unspecified**, and it changes as the table grows | a hash table, and nothing more |
| `ordered_map<K, V>` | **insertion order** | one extra copy of every key, and O(n) removal |

That's the only difference a caller sees. Everything below applies to both unless it says otherwise, and the
surfaces are deliberately identical so you can swap one for the other by changing the type.

Reach for `map` by default. Reach for `ordered_map` when the order is part of what you're expressing — a
config file's keys, the steps of a pipeline, anything a person will read back.

## The one big idea

```echo
map<string, int32> $a = map<string, int32>();
$a['x'] = 1;

map<string, int32> $b = $a;   // a whole second table
$b['y'] = 2;

echo $a->count();             // 1
echo $b->count();             // 2
```

`$b = $a` copies. Every key, every value, all the way down — a `map<string, array<int32>>` copied is a new
table, new strings sharing their buffers, and new arrays with copies of *their* elements. And when a map goes
out of scope it destroys everything it holds, exactly once.

If that surprises you, read [Arrays](arrays.md) first: it's the same decision, and everything else in this
chapter falls out of it.

## Keys

A key type owes two things, and neither is an interface you declare:

```echo
// 1. a hash
namespace hash;

function of(const point& $p) : uint64
{
    return combine(of($p->x), of($p->y));
}
```

```echo
// 2. an equality
operator (const point& $a) == (const point& $b) : bool
{
    if ($a->x == $b->x) {
        return $a->y == $b->y;
    }

    return false;
}
```

Every primitive already has both, and so do `string` and `string::view` — so `map<int32, …>`,
`map<string, …>` and `map<usize, …>` need nothing from you. It's only your own types that have to say.

**Why an overload set and not a `contract::hashable`?** Because a primitive can't declare a conformance. A
constrained `map<K: contract::hashable, V>` would refuse `map<int32, V>` — the first map anybody writes — and
no amount of interface design gets around that. An overload's parameter type, on the other hand, can be
anything at all.

There's a cost, and it's worth knowing before you hit it: **a namespace is a file-level statement**, so that
`namespace hash;` block needs a file of its own. It can't sit beside the `struct point` it's about, and a
single-file program can't extend the set at all. Two mistakes look like nothing happening — `namespace
app::hash;` is a *different* namespace, and an `of` at the root is invisible to the qualified `hash::of`.

`hash::of_bytes<T>` is there for the case where your type really is just its bytes. It's spelled explicitly
on purpose: byte-hashing anything that holds an address is wrong, and a `string` byte-hashed hashes its heap
pointer — so two equal strings would land in two buckets. That's the one way a hash table breaks that
nothing else catches.

A float makes a poor key. `+0.0` and `-0.0` are handled (they hash the same), but a NaN is never equal to
itself, so a NaN key can be stored and then never found.

## The bracket

```echo
$m['a'] = 1;        // insert
$m['a'] = 2;        // replace
echo $m['a'];       // read - 2
```

The bracket does what you'd expect, and `map<K, V>` is shaped like `array<T>` throughout: writing inserts or
replaces, reading and borrowing hand back what's there, and asking for a key that isn't there **asserts** —
exactly the bargain an array makes about an index past its end.

```echo
string $k = 'a';
int32& $slot = &$m[$k];     // a borrow of the value
$slot = 9;
echo $m['a'];               // 9

echo $m['zz'];              // aborts: 'map key not found'
```

Two things are worth knowing about it.

**Writing and reading are two different declarations.** `$m[$k] = $v` resolves to one that takes the value and
owns the whole insert-or-replace; every other position resolves to one that hands back a borrow of an existing
element. Which one answers is decided by *where the bracket sits*, and nothing else. You'd never notice,
except that it's what makes the two behaviours possible at once: a single operator can't promise both "the
value that was here is gone" and "hand me the value that's here". Writing one of your own is
[Operators](operators.md), "Indexing".

**A key that's a temporary needs a variable when the bracket is being written *through*.** That's the `&$m[$k]`
above, and the reason it says `$k` rather than `'a'`: an assignment target and an `&` operand refuse to give a
temporary storage — the same rule that refuses `f().field = 1` — and a literal key is a temporary. Plain
reading is unaffected, so `echo $m['a']` is fine.

A `const` map reads the same way, through its own overload of the bracket:

```echo
const map<string, int32>& $ro = &$m;
echo $ro['a'];      // fine - and there is no write contract to reach through a const map
```

## The surface

Counts and capacities are `usize` throughout, matching `mem::`.

| Method | Meaning |
|---|---|
| `->count() : usize` | how many entries |
| `->capacity() : usize` | how many **slots**. See below — this is not "how many fit" |
| `->is_empty() : bool` | |
| `->has(K) : bool` | |
| `->get(K) : V` | a **copy** of the value. Asserts the key is there |
| `->at(K) : V&` | a **borrow** of it. Asserts. `&$m[$k]` is the same thing, written through the bracket |
| `->set(K, V) : void` | the call form of `$m[$k] = $v`, and literally so — its body *is* that write |
| `->remove(K) : bool` | destroys key and value. True when there was something |
| `->take(K) : V` | removes it and **hands the value over**. Asserts |
| `->extend(map<K,V>) : void` | copies every entry across, replacing shared keys |
| `->clear() : void` | destroys every entry, keeps the buffers |
| `->reserve(usize) : void` | room for that many entries without rehashing again |
| `->shrink_to_fit() : void` | gives back what the entries no longer need |
| `->keys() : array<K>` | a fresh array of copies. Ordered, for an `ordered_map` |
| `->values() : array<V>` | the same, lining up with `keys()` index for index |
| `->iterate()` | the cursor `foreach` drives |
| `->clone() : map<K,V>` | the explicit deep copy |

`get` and `take` are the pair worth being deliberate about. `get` copies — for a `map<string, string>` that's
one more reference to the value's buffer. `take` hands the value over and the map stops holding it, which for
an owning `V` is the whole difference:

```echo
string $v = $m->take('k');   // the map no longer has 'k' at all
echo $m->count();            // one fewer
```

## Growth and invalidation

An array only moves its buffer when it grows past its capacity, and `capacity()` tells you how much room is
left. A map's `capacity()` is **slots**, and the load factor stands between slots and entries — so you can't
read off how many more inserts are free.

Which leaves one safe assumption: **every insert re-seats the table.**

```echo
int32& $slot = $m->at('a');
$m['b'] = 2;                  // may have rehashed
$slot = 99;                   // undefined. do not do this
```

So a `V&` — from `->at()`, or from a `foreach` binding — is valid until the next insert and no further.
`->remove()` and `->take()` are safe: they only write a state word. And `->reserve($n)` up front is how you
make a run of inserts leave your borrows alone.

This is the pointer chapter's rule arriving again: see
[Pointers & references](pointers_and_refs_v2.md). It's stricter for a map than for an array only because you
can't see the growth coming.

## Ownership

```echo
map<string, string> $a = map<string, string>();
$a['k'] = $v;                 // the map holds a copy of the value

map<string, string> $b = $a;  // a second table, and the value retained
$b->remove('k');              // $a still has it

$m->clear();                  // every key and every value destroyed
```

The cost is the array chapter's cost, keyed: copying a map copies every value, so a
`map<string, array<int32>>` copied twice is a lot of copying. `->take()` when you want to move a value out,
and a borrow when you only want to look.

An element type that owns something and hasn't said what copying means is a compile error, exactly as it is
for an array. And for `ordered_map` there's one extra requirement: **`K` must be copyable**, because the order
is a second array of keys. A key with a destructor and no copy constructor can be a `map` key and cannot be an
`ordered_map` key.

## Iterating

```echo
foreach ($m as $k => $v) {
    echo $k;
    echo $v;
}

foreach ($m as $v) {          // the key is optional
    echo $v;
}

foreach ($m as &$v) {         // a write-through loop
    $v = $v * 2;
}
```

The key arrives **by value** and the value by borrow, which is the protocol
[Iteration](iteration.md) describes — a map is the first place you meet a key that isn't a `usize`.

For a `map`, **the order is unspecified** and will change as the table grows. Don't write a test that depends
on it; write one that sums or counts. For an `ordered_map` the order is insertion order, and re-writing an
existing key does *not* move it to the back:

```echo
$m['c'] = 1;
$m['a'] = 2;
$m['c'] = 3;   // still first

foreach ($m as $k => $v) { echo $k; }   // c, a
```

Don't insert while iterating — see the invalidation section. Removing is fine.

A `const` map reads but does not iterate, exactly as a `const` array doesn't: the cursor hands out mutable
borrows and there's no read-only cursor type yet.

## What it costs

- **A lookup** is one hash, one integer divide, and then a walk that reads a single dense array. `==` only
  runs on a slot whose stored hash already matched, so a `string` key costs a byte comparison only on a real
  candidate.
- **An insert** may rehash. Amortised that's fine; individually it's O(n).
- **A remove** leaves a tombstone, which later probes walk past. Churn reclaims them rather than growing, so
  a map used as a working set doesn't creep.
- **`ordered_map`** adds a key copy per entry, an O(n) removal, and one hash lookup per iteration step — the
  keys are read in order, the values are not.

## What has no spelling yet

- **`["LHR" => "Heathrow"]`.** The literal isn't spelled, so both maps are built explicitly today. The
  bracket contract has always been general enough — an index need not be a number — and `=>` is already a
  token; it's the expression form that's missing.
- **An optional lookup.** `$m->lookup($k) ?? 0` is what you want and `has` + `get` is what there is.
- **A lazy `keys()`.** It returns a real array today, because a type may declare only one iteration contract
  and a map's is over its values.
- **Iterating a `const` map**, as above.
- **Choosing the hasher.** `map<K, V, Hasher>` waits on the same work `array<T, Allocator>` does.
