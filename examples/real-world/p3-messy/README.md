# messy (P3 real-world test)

Deliberately average-user-messy code to stress preview accuracy.
Real View/XML app code under `src/` (minimal), plus a flow + CSS
preview sketch under `flow/` and `styles/` that contains, on
purpose:

1. nav via a helper function (`openDetail()`)
2. a handler defined in another file (`onCrossClick` in Other.kt)
3. a method reference held in a variable first (`val h = ::onVarClick`)
4. a dynamic route built from a string template (`Detail/${noteId}`)
5. nav inside a nested/coroutine-like block (`runLater { nav(...) }`)
6. duplicated button labels (`Go` on Home and Detail; `Share` on
   Home and Detail via XML `android:onClick`)
7. one missing `[preview.bindings]` entry (`mysteryButton`)

Build:

```
ccb build
```

Measure coverage:

```
ccb preview --json
```

See `docs/real-world-testing.md` for the measured counts.
