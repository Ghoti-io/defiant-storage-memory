# defiant-storage-memory

The in-memory backend for `defiant-storage`. It keeps committed bytes in
the process. Isolation, the writer queue, the formats and the outbox runner
stay in the port; this library fills the port's backend vtable.

`gdefsmemory_store_create` returns a `GDEFSTOR_Store`. Close it with
`gdefstor_store_destroy`.

Queries over a projection type (`defiant-storage/query.h`) are answered by the
port's own scan: this backend sets no `query` hook, so the port lists the keys
of the type, reads each row and evaluates the predicate tree row by row. That
scan is the reference answer every other backend is checked against, by the
conformance suite below. Nothing is indexed; the cost is linear in the rows of
the type.

Depends on `defiant-storage` through pkg-config.

```bash
make test PREFIX=/path/to/prefix
```

Its tests compile the port's conformance driver, installed beside the port.
