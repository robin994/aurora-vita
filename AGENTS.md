# Aurora Vita engineering contract

Aurora translates the original Dolphin GX frontend into native GXM or VitaGL.
Preserve game callbacks, FIFO order, EFB semantics and the CPU fallback.

Use GPT-6 Astra with high reasoning for cross-cutting renderer changes; use the
model picker in Codex. Do not add an OpenAI API dependency to the runtime.
Once implementation is authorized, complete relevant local edits and checks
without repeated approval requests. Delegate only when the user requests it.

Inspect the current source before using historical performance audit proposals.
GXM must re-establish context state on every BeginScene: caching across scenes
caused a hardware suspend/resume crash. Partial scissors remain pixel exact.
Never reorder draws across clear, copy, target changes or barriers. Preserve
uniform reservation rules and keep immutable snapshots alive through execute.
Guest source updates, TLUT, EFB and runtime mode changes invalidate their caches.

Validation:

```sh
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests
VITASDK=/usr/local/vitasdk cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8
```

Add equivalence/transition tests for state, indexing and resource lifetime changes.
Record the commit, options and artifact hash for device comparisons. Host tests
and builds establish no Vita FPS or visual correctness claim. Do not sum nested
frontend timings. Diagnostic finish modes perturb CPU/GPU overlap.
