# catalog (P2 real-world test)

A small shop app: product list, product detail, cart, checkout form.
Real View/XML app code under `src/` (including a shape drawable and
two themes), plus a flow + CSS preview sketch under `flow/` and
`styles/`.

Build:

```
ccb build
```

Preview:

```
ccb preview
ccb preview --json
ccb preview --scenario full
```

The sketch has more screens and edges than P1 (13 declared edges),
a product list with 8 scenario rows, an `Image` component, and a
loading/loaded branch on Detail.

Maven stretch (verified): `[deps] maven` declares
`androidx.recyclerview:recyclerview:1.3.2`, with `maven_central =
true` (required: the transitive `kotlin-stdlib` lives on Maven
Central, not Google Maven — without it `ccb setup` fails with
CCB-E3019). `ccb setup` prefetches 18 artifacts (9 aar, 9 jar) and
writes `ccb.lock` (committed, as its header instructs); the build
with the dep succeeds and the recyclerview classes are present in
`classes.dex`. The Kotlin code only imports the class; no
RecyclerView UI is built.
