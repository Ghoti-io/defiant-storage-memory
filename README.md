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
`gdefstor_model_scan()`, over the current tables. Records, counters and outbox
rows (story S6a) live beside the entities: a write keeps its record puts and
deletes and its counter values apart and moves them in at commit, copies the
outbox on first touch, and writes each counter's floor into the committed
tenant at once, so a value an aborted write took is never taken again.
`gdefsmemory_keystore_create`
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
reopens old rows, a current table that drifts from the truth, a record written
outside its transaction, a counter that reuses an aborted value, an outbox
claim out of order or under another live lease, a parked row deleted, records
cleared by a rebuild) and requires a named test to fail for each.
