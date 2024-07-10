# C Binding

You can use C functions in echo by using the `ffi` keyword. It is recommended to wrap the C function in a eco function to convert the C types to echo types.

```echo
extern {
    void time(ptr<uint64> time) as c_time;
}

function time() : int {
    uint64 $time
    c_time(&$time)
    return $time
}

echo time();
```