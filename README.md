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

It also serves the port's entity API (`defiant-storage/model.h`): each
entity keeps its truth (its row, states, revisions and every item version) and
its current table beside it, and a write works on copies of the entities it
touches, swapped in at commit, so a read keeps seeing the last commit while a
write is open. Entity queries are answered by the port's reference engine,
`gdefstor_model_scan()`, over the current tables. `gdefsmemory_keystore_create`
opens a key store (`defiant-storage/keystore.h`) over a cipher the caller
supplies; it keeps keys in the process, which makes it for tests only.

Depends on `defiant-storage` through pkg-config.

```bash
make test PREFIX=/path/to/prefix
```

Its tests compile the port's conformance suites, installed beside the port:
the key/bytes suite, the entity suite (`model.inc`) and the key store suite
(`keystore.inc`). `make test` also runs `check-model`, which plants the defects
a backend can make on its own (a `valid_to` written twice, a revert that
reopens old rows, a current table that drifts from the truth) and requires a
named test to fail for each.
