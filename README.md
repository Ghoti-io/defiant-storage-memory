# defiant-storage-memory

The in-memory backend for `defiant-storage`. It keeps committed bytes in
the process. Isolation, the writer queue, the formats and the outbox runner
stay in the port; this library fills the port's backend vtable.

`gdefsmemory_store_create` returns a `GDEFSTOR_Store`. Close it with
`gdefstor_store_destroy`.

Depends on `defiant-storage` through pkg-config.

```bash
make test PREFIX=/path/to/prefix
```

Its tests compile the port's conformance driver, installed beside the port.
