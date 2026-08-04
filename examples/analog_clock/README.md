# analog_clock

A live analog clock in the terminal. It clears the screen once, then redraws a dial, three hands and a
digital readout five times a second until you press Ctrl-C. Give it a time and it draws that one frame
and stops.

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

Give it an `HH:MM:SS` and it draws that one frame and exits instead — which is how the picture above was
made, and the quickest way to see whether a change to the drawing did what you meant:

```bash
./clock 18:15:34
../../build/echoc run -- 18:15:34  # the `--` is what stops echoc reading it as a filename
```

## What is in it

| | |
|---|---|
| [module.eco](module.eco) | the manifest: a name, a version, and `src/*.eco` |
| [src/clock.eco](src/clock.eco) | `extern` bindings for `time`, `localtime` and `usleep`; `Time`; reading one off the command line; where each hand points |
| [src/canvas.eco](src/canvas.eco) | the character grid, and turning it into one printable frame |
| [src/face.eco](src/face.eco) | `Dial`, the `Renderable` interface, and the three layers that conform to it — the dial, the hands, the `HH:MM:SS` readout |
| [src/renderer.eco](src/renderer.eco) | `Renderer` — a canvas, a list of layers, and `frame` — one whole picture |
| [src/main.eco](src/main.eco) | the program — an Echo program is the entry module's top-level statements, so there is no `main` to declare |

## How it is put together

The drawing is a stack of layers. `Face` draws the ring and the hour marks, `Hands` draws the three
hands and the pin, and `Readout` draws the digits. All three are classes conforming to one interface:

```echo
interface Renderable
{
    function render(Canvas& $c, Time& $t) : void;
}
```

The `Renderer` holds them in an `array<Renderable>` and draws them in the order they were added, which
is the whole of the compositing rule. It never names `Face` or `Hands` or `Readout`, so a fourth layer is
one more `add` in `main.eco` and no change anywhere else.

They have to be classes. An interface value is a reference, so only a heap type can be stored as one. A
struct can conform just as well, but you reach that conformance through a constrained generic
(`function render<T: Renderable>(T& $layer)`) — and a `T` is one type per instantiation, so it can't hold
three different layers in one list.

Everything else stays a struct. `Canvas` has exactly one owner and is only ever borrowed, and `Dial` owns
nothing at all, so each layer just keeps a copy of it.
