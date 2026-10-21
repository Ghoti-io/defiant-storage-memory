# defiant-storage-memory

The in-memory backend for `defiant-storage`. It keeps committed rows in
the process. Isolation, the writer queue and the formats stay in the port;
this library fills the port's entity backend (`model_backend.h`).

`gdefsmemory_store_create` returns a `GDEFSTOR_Store`. Close it with
`gdefstor_store_destroy`.

It serves the port's entity API (`defiant-storage/model.h`): each
entity keeps its truth (its row, states, revisions and every item version) and
its current table beside it, and a write works on copies of the entities it
touches, swapped in at commit, so a read keeps seeing the last commit while a
write is open. Entity queries are answered by the port's reference engine,
`gdefstor_model_scan()`, over the current tables. Records, counters and outbox
rows (story S6a) live beside the entities: a write keeps its record puts and
deletes and its counter values apart and moves them in at commit, copies the
outbox on first touch, and writes each counter's floor into the committed
tenant at once, so a value an aborted write took is never taken again. It
has the restore members a snapshot import writes through (story S6a3): a
revision that keeps its commit sequence, a counter set to its value, an
outbox row with its lease, failures and parked flag. It also keeps the blind
index (blind rows beside the versions they index, the history and subject
lookups, deletion by subject and putting a row back) and lists an entity's
languages from its root items.
`gdefsmemory_keystore_create`
opens a key store (`defiant-storage/keystore.h`) over a cipher the caller
supplies; it keeps keys in the process, which makes it for tests only.

Depends on `defiant-storage` through pkg-config.

```bash
make test PREFIX=/path/to/prefix
```

Its tests compile the port's conformance suites, installed beside the port:
the entity suite (`model.inc`) and the key store suite (`keystore.inc`). `make test` also runs `check-model`, which plants the defects
a backend can make on its own (a `valid_to` written twice, a revert that
reopens old rows, a current table that drifts from the truth, a record written
outside its transaction, a counter that reuses an aborted value, an outbox
claim out of order or under another live lease, a parked row deleted, records
cleared by a rebuild, a scan handed the oldest revision instead of the newest,
a blind row put back beside the one it replaces) and requires a named test to
fail for each.
