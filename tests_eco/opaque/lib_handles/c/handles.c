struct Handle
{
    int value;
};

struct Resource
{
    int value;
};

static struct Handle g_handle;

struct Handle *eco_handle_make(int value)
{
    g_handle.value = value;
    return &g_handle;
}

int eco_handle_read(struct Handle *h)
{
    return h->value;
}

void eco_handle_free(struct Handle *h)
{
    (void)h;
}

int eco_resource_start(struct Resource *r)
{
    return r->value;
}
