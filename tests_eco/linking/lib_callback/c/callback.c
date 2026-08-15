int eco_cb_apply(int (*fn)(int), int v)
{
    return fn(v);
}
