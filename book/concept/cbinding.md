# C Binding

You can use C functions in echo by creating a binding. This is done by creating a file with the `.echo` extension and using the `@c` directive to define the binding.

```echo
@c
int32 printf(string format, ...);
```