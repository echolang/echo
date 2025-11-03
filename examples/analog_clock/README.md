# analog_clock

A live analog clock in the terminal. It clears the screen once, then redraws a dial, three hands and a
digital readout five times a second until you press Ctrl-C.

```
                ······◆······
           ●····             ····●
        ····                     ····
      ···                           ···
    ···                               ···
   ●·                                   ·●
  ··                                     ··
 ··                                       ··
 ·                                         ·
··                                         ··
·                                           ·
◆                     ┼▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒     ◆
·                    •█                ▒▒   ·
··                  •█                     ··
 ·                 ••█                     ·
 ··               •• █                    ··
  ··             ••  █                   ··
   ●·           ••  █                   ·●
    ···        ••                     ···
      ···      •                    ···
        ····  •                  ····
           ●····             ····●
                ······◆······

                   18:15:34
```

## Running it

The module is found by being in its directory — `echoc` looks for a `module.eco` beside you when you
name neither a source file nor `-m`:

```bash
cd examples/analog_clock
../../build/echoc run              # parse, compile, JIT
```

Or as a native binary:

```bash
../../build/echoc build -o clock   # -O to optimize
./clock
```

Ctrl-C quits. Nothing about the terminal is switched on that would need switching back off, so the
shell you return to is the one you left.

## What is in it

| | |
|---|---|
| [module.eco](module.eco) | the manifest: a name, a version, and `src/*.eco` |
| [src/clock.eco](src/clock.eco) | `extern` bindings for `time`, `localtime` and `usleep`; `Time`; where each hand points |
| [src/canvas.eco](src/canvas.eco) | the character grid, and turning it into one printable frame |
| [src/face.eco](src/face.eco) | the dial, the hands, the `HH:MM:SS` readout |
| [src/main.eco](src/main.eco) | the program — an Echo program is the entry module's top-level statements, so there is no `main` to declare |

## What this example says about Echo

Nearly every decision in here is the language showing you where it currently is, so it is worth
reading in that light rather than as a style guide.

**The clock, the sleep and the timezone all come from libc.** The stdlib has no time and no way to
wait, so `src/clock.eco` declares `time`, `localtime` and `usleep` in an `extern` block and that is
the whole binding — no wrapper the compiler needs to know about. Reading the `struct tm` that
`localtime` returns means declaring the fields we read as an ordinary Echo struct and reaching through
the pointer with `->`.

**A frame is one string.** `echo` takes a single expression and always appends a newline, and there is
no `printf` — `extern` refuses variadics. So `Canvas::to_frame` builds the entire picture, cursor-home
escape included, and one `echo` per frame prints it. That is also why it does not flicker.

**The cells hold glyph ids, not bytes.** The dial is Unicode, so a glyph is two or three bytes and
cannot live in a `uint8`. Instead an id below 32 is one of the example's own glyphs and an id of 32 or
above is that ASCII byte, which keeps the grid a flat `array<uint8>` and lets the digits stay plain
arithmetic. Flat matters: `array<array<T>>` copies its elements bytewise today and would be silently
wrong for an owning element type.

**The digits are arithmetic.** There is no int-to-string and no formatting anywhere, so `two_digits`
pushes `48 + $value / 10` and `48 + $value % 10`.

**The `Canvas` is only ever borrowed.** It owns an `array`, and a struct is copied field by field — so
passing one by value would duplicate the buffer's pointer and free it twice. It is built once as a
local in `main.eco`, and everything that draws takes a `Canvas&`. `Dial`, which owns nothing, is passed
around freely.

And a few smaller ones: ESC is `"\x1b"` (`\e` and `\033` are not escapes Echo knows), there is no
`for` loop so every loop here is a `while`, there is no global variable so π is a function, and `&&`
does not short-circuit yet so `Canvas::set` tests one bound per `if`.
