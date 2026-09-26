# Audit ottimizzazioni — renderer nativo sceGxm (2026-09-26)

Branch di lavoro: `gxm-optimization` (da `vita-experiment` @ `c7e57c3`).
Ambito: solo il percorso **SceGxm** (`AURORA_VITA_RENDERER=GXM`): `platforms/vita/gxm/*`,
la facade `gxm_facade.cpp` e le parti condivise che lo alimentano per ogni draw
(`gx/aurora_vita_draw_sink.cpp`, `gfx/vita_draw_adapter.cpp`, `gfx/vita_streaming_arena.cpp`,
`gfx/vita_static_geometry.hpp`, `gfx/vita_pipeline_key.cpp`). vitaGL è fuori ambito.

## Stato sul branch

Base riallineata a `experiment/vita-native-gxm` @ `f96d00b` (la revisione usata da Strikers).
I riferimenti di riga sono stati presi su `c7e57c3`: in `gxm_renderer.cpp` le righe dopo la
~190 sono spostate di ~+30. Rispetto a `c7e57c3` la base contiene già **M4** (opt-in
`AURORA_VITA_GXM_DIRECT_STREAM_WRITE`, attivo nella build Strikers) e il parameter buffer
configurabile (**G4**, `nativeParameterBufferBytes`).

| Voce | Stato |
|---|---|
| Fase 0 | Contatori per frame: `depth_load_scenes`, `depth_store_scenes`, `depthless_scenes`, `finish_calls`, `scissor_free_draws` (riga RENDERER e `PerformanceSnapshot`) |
| G1/G3 | Implementato: un clear depth prima della prima draw della scena azzera `depthValid` → nessun force-load |
| G2 | Implementato: `blit_to_default` e `copy_display_region` aprono scene senza depth surface; una draw che usa il depth chiude la scena depthless |
| S1 fase 1 | Implementato: variante senza discard creata insieme alla pipeline (coperta dal prewarm), usata solo con scissor a pieno target |
| M2 | Implementato: display copy con triangolo statico + `u_tex_transform`; nessun `update_buffer`/`finish`, `blitVertices` non più condiviso |
| C1 | Implementato: `BeginScene` invalida solo reservation uniform e region clip; binding texture/programma invalidati esplicitamente su reinit/distruzione |
| C2 | Implementato: fast path del binding texture prima di lookup e validazioni |
| C8 | Implementato: copie di `FrameStats` in DrawSink solo con telemetria |
| S2 | Implementato: `nativeTextureWrapMask` = texture swizzled o clamp su entrambi gli assi (solo GXM) |
| S4 | Implementato: copy mode R4/A8 e opacità RGB565 come bit `PipelineDesc`; nessun branch su uniform. Cambia `sizeof(PipelineDesc)`: il manifest hot viene ricostruito una volta |
| Banco di prova | strikersVita, branch `experiment/gxm-optimization-testbed`, `GXM_TESTBED.md` |
| Altre | Da fare (in attesa dei risultati hardware delle fasi 1-3) |

## Metodo e regole

- Analisi statica del codice confrontata con la documentazione SDK ufficiale della GPU
  (SGX543MP4+/libgxm, SDK 3.55). I riferimenti sono nella forma *GPU UG §x* (GPU User's
  Guide), *libgxm Ov. §x* (libgxm Overview), *USSE Tut. §x* (USSE Performance and Shader
  Optimizations Tutorial), *Shader UG* (Shader Compiler User's Guide). La documentazione
  non è nel repository (materiale riservato): il digest locale è in
  `~/Documents/Code/PSVita/sdk-docs/gpu/GPU_KNOWLEDGE.md`.
- **Nessun numero di questo documento è una misura.** Le stime di banda sono aritmetica
  sulle dimensioni delle superfici. Vale la lezione di
  [REGRESSION_AUDIT_2026-09-19.md](../REGRESSION_AUDIT_2026-09-19.md): nessuna ottimizzazione
  cambia la semantica GX globalmente senza un confronto framebuffer su hardware; ogni
  intervento va dietro a un confronto A/B e, se tocca l'aspetto, a un flag.
- Ogni voce indica: **Dove**, **Cosa succede oggi** (verificato nel codice), **Perché costa**
  (con il riferimento SDK), **Proposta**, **Rischio/validazione**.

Legenda priorità: 🔴 alto guadagno atteso, rischio basso · 🟠 alto guadagno, serve validazione
visiva · 🟡 guadagno medio · ⚪ pulizia/abilitante.

## Sintesi

| ID | Area | Problema | Prio |
|---|---|---|---|
| G1 | Banda tile | `depthValid` non torna mai a false: **ogni scena fa force-load + force-store del depth** | 🔴 |
| G2 | Banda tile | Scene che non usano depth (display copy, blit, clear di presentazione) caricano/salvano comunque il depth condiviso | 🔴 |
| G3 | Banda tile | Un clear depth a inizio scena non evita il load del depth | 🔴 |
| G4 | Memoria GPU | Parameter buffer 4 MiB (default SDK 16 MiB): rischio partial render | 🟡 |
| S1 | Shader | Il `discard` dello scissor è compilato in quasi tutte le pipeline anche con scissor a pieno target → HSR degradato | 🔴 |
| S2 | Shader | `nativeTextureWrapMask` non viene mai impostato → wrap software in ogni fetch | 🔴 |
| S3 | Shader | Trasformazione UV (`gx_sample_uv`) nel fragment shader → ogni fetch è *dependent* | 🟠 |
| S4 | Shader | Branch su uniform (`u_tex_copy_mode`) e `lerp` force-opaque in ogni fetch texture | 🟠 |
| S5 | Shader | Varying `TEXCOORD` sempre `float3` anche quando serve `.xy` | 🟡 |
| S6 | Shader | Fog range: doppia catena di 9 ternari annidati | ⚪ |
| C1 | CPU/draw | `ensure_scene` invalida cache di stato che GXM mantiene tra scene | 🟡 |
| C2 | CPU/draw | `draw`/`bind_texture`: lookup hash e validazioni prima dei fast path | 🟡 |
| C3 | CPU/draw | Upload uniform con `sceGxmSetUniformDataF` + query del parametro a ogni chiamata | 🟡 |
| C4 | CPU/draw | `FixedVertexUniforms` (~3 KiB) costruito, copiato, confrontato e ricopiato a ogni draw GPU | 🟠 |
| C5 | CPU/draw | Confronto uniform fragment ricostruito a ogni draw (array temporanei + catena `memcmp`) | 🟡 |
| C6 | CPU/draw | Draw streamed consecutive compatibili non vengono mai unite | 🟡 |
| C7 | CPU/draw | `pipeline_key` (~600 hash FNV) ricalcolato più volte sui percorsi GPU/facade | 🟡 |
| C8 | CPU/draw | Copie di `FrameStats` per draw anche senza telemetria | ⚪ |
| M1 | Sync | `inFlight` booleano + `finish()` globale: distruzioni/aggiornamenti = stallo CPU↔GPU | 🔴 |
| M2 | Sync | `copy_display_region` aggiorna un VBO in volo → `finish()`; VBO condiviso con `blit_to_default` (**bug**) | 🔴 |
| M3 | Memoria | Un memblock + `sceGxmMapMemory` per ogni buffer (geometria statica: 2 per mesh) | 🟡 |
| M4 | Memoria | Streaming: staging cached + `memcpy` verso uncached (doppia scrittura) | 🟡 |
| M5 | Upload | Texture: decode → lineare → swizzle per-texel → `memcpy`, con vettori nuovi per ogni mip | 🟡 |
| M6 | Cache | Eviction/invalidazioni O(N) e O(N²) in `TextureCache`, `PipelineCache`, `destroy_texture` | ⚪ |

Cose già fatte bene e da **non** rompere: cache viewport/scissor/stream/programma, riuso delle
reservation uniform quando i dati coincidono, region clip usato solo come clip grossolano
(corretto: è a granularità tile, libgxm Ov. §6.8), transfer PTLA per le copie EFB 1:1,
program binary cache persistente + prewarm, `vertexSyncObject=NULL` in `BeginScene`
(la reference SDK lo dichiara "reserved, must be NULL": spiega l'`INVALID_POINTER` storico).

---

## G — Banda on-chip, scene e superfici

### 🔴 G1 — Il depth viene caricato e salvato su ogni scena, per sempre

**Dove:** `gxm_renderer.cpp:384-392` (`end_scene`), `:394-416` (`ensure_scene`).

**Oggi:** `end_scene()` imposta `depthValid = (result>=0)`; nessun altro punto lo rimette a
`false` (solo `shutdown()`, `:1829`). `ensure_scene()` usa `loadDepth = depthValid` e imposta
sempre `SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED`. Dalla seconda scena dell'esecuzione in
poi, **ogni scena sul target di display forza load e store del depth per ogni tile**; lo
stesso vale per ogni target offscreen dopo il suo primo uso (`Texture::depthValid`).

**Perché costa:** su SGX il depth vive on-chip e il comportamento di default è non toccare mai
la memoria salvo partial render (libgxm Ov. §4.5 "Depth/Stencil Bandwidth Is Optional",
§6.7). Force-load/store lo trasforma in traffico pieno: con DF32 a 960×544 la superficie è
960×544×4 = **~2,0 MiB**; load+store = **~4 MiB per scena**. Con 3-5 scene/frame (EFB copy,
display copy) sono 12-20 MiB/frame di banda sprecata nel caso tipico in cui il gioco pulisce
il depth all'inizio del frame.

**Proposta:** non cambiare la politica di store (serve davvero quando una GXCopyTex spezza il
frame in più scene, vedi G3/G4 dell'audit precedente), ma rendere esatta l'informazione
"il contenuto del depth in memoria serve ancora":
1. G3 (sotto) azzera `depthValid` quando un clear sovrascrive tutto il depth prima di qualsiasi
   draw della scena → la scena successiva parte dal *background depth* senza load.
2. Esporre in telemetria per scena `depth_load`/`depth_store` per verificare sul titolo reale
   quante scene continuano a caricare.

**Rischio:** basso se limitato a G3 (semantica identica: stesso valore finale per ogni pixel).

### 🔴 G2 — Scene senza uso del depth che toccano comunque il depth condiviso

**Dove:** `copy_display_region` (`:1681-1803`), `blit_to_default` (`:1635-1679`),
`clear` per il letterbox (`:1776`).

**Oggi:** queste scene disegnano sul target di display con `depthTest=false`, ma
`ensure_scene` passa comunque `&depthSurface` con force-load (G1) e force-store: la scena di
presentazione legge e riscrive ~4 MiB di depth che non usa.

**Perché costa / cosa dice l'SDK:** `sceGxmBeginScene` accetta `depthStencilSurface = NULL`
("stesso comportamento di una surface inizializzata con `SceGxmDepthStencilSurfaceInitDisabled`";
la struttura è copiata alla chiamata). Una scena color-only non ha traffico depth e non
altera la memoria del depth, quindi `depthValid` resta vero per il frame successivo.

**Proposta:** flag interno `nextSceneNeedsDepth` (default true) impostato a false dai percorsi
display-copy/blit prima di `ensure_scene`; con false passare `nullptr` e non toccare
`depthValid`. Qualsiasi draw successiva con `depthTest` sullo stesso target deve comunque
aprire una scena con depth: l'invariante si garantisce perché questi percorsi chiudono la
scena (`end_scene`) prima di tornare al chiamante, o controllando in `draw()` che una pipeline
con depth non venga mai sottomessa in una scena aperta senza depth (in tal caso `end_scene`
e riapertura).

**Rischio:** basso. Validare con il probe esistente (copie display con crop/letterbox).

### 🔴 G3 — Il clear del depth non evita il load

**Dove:** `Renderer::clear` (`:896-915`), chiamato da `DrawSink::copy_tex(clear=true)` e dai
comandi `Clear`.

**Oggi:** il clear è un triangolo a schermo intero con `depthFunc=Always` disegnato *dopo*
`ensure_scene`, quindi la scena ha già il force-load attivo: prima si caricano 2 MiB e poi
vengono sovrascritti tutti.

**Proposta:** in `clear()`, se `writeDepth` è vero e la scena del target corrente **non è ancora
iniziata** (`!inScene`) — il clear copre sempre l'intero target, `:902` — impostare
`depthValid=false` (o `texture.depthValid=false` per l'offscreen) prima di `ensure_scene()`.
Il triangolo di clear scrive comunque il valore richiesto. Stesso ragionamento per il colore:
un clear RGB+A opaco occlude già il background object via HSR, quindi non serve altro
(libgxm Ov. §6.6 "Background Object").

**Rischio:** molto basso (risultato per pixel identico). È il cambio che rende G1 efficace.

### 🟡 G4 — Parameter buffer da 4 MiB

**Dove:** `gxm_renderer.hpp:14` (`parameterBufferBytes = 4 MiB`); default SDK
`SCE_GXM_DEFAULT_PARAMETER_BUFFER_SIZE` = 16 MiB.

**Perché costa:** se una scena non entra nel parameter buffer il firmware fa *partial render*:
tile processati più volte, depth/colore salvati e ricaricati (GPU UG §14.1: "il tempo di
rendering aumenta drasticamente"; libgxm Ov. §4.4). Le scene GX con molte draw piccole e
varying `float3` (S5) consumano più parameter buffer del necessario. Su homebrew non c'è
Razor per vederlo.

**Proposta:** renderlo configurabile da `BackendConfig` (se non lo è già dalla facade) e fare
A/B 4 / 8 / 16 MiB sulla scena più pesante misurando frame time GPU (tempo tra `EndScene` e
la notifica fragment). Verificare anche i warning TTY di libgxm sui ring buffer
("high-water mark... scene is being split"): se compare quello del *fragment* ring è una
scena extra per frame (impatto alto, libgxm Ov. §6.2 Tabella 5) e va aumentato quel ring.

---

## S — Shader generati (`gxm_shader_gen.cpp`): costo GPU per pixel

### 🔴 S1 — `discard` dello scissor in (quasi) ogni fragment program

**Dove:** `gxm_shader_gen.cpp:355-356`, `:541-542`; `PipelineDesc::fragmentScissor = true`
di default (`gfx/vita_gfx_types.hpp:212`); solo il clear lo disattiva (`gxm_renderer.cpp:902`).

**Oggi:** ogni pipeline GX genera
`if(window_position...) discard;` più l'input `WPOS`, anche quando lo scissor della draw
copre l'intero target (caso di gran lunga più frequente). L'audit di regressione ha
correttamente ripristinato il controllo esatto (il region clip è a tile), ma nessuna variante
senza discard viene selezionata quando non serve (era la priorità 3 rimasta aperta).

**Perché costa:** un fragment program con `discard` è pass type *Discard*:
- l'ISP trattiene al minimo 63 primitive prima dell'"HW limit flush" invece di centinaia
  (GPU UG §14.8);
- una primitiva discard non può occludere nell'HSR ciò che è sotto finché non è shaded;
  passare da discard a opaque provoca "Flush All" con stallo dell'ISP (GPU UG §14.19-14.20);
- l'ordine ideale SGX è opachi prima dei discard (libgxm Ov. §13.1): qui *tutto* è discard.
In pratica l'HSR, cioè il vantaggio principale del TBDR, è quasi annullato su tutta la scena.

**Proposta (fase 1, semantica esatta):** varianti lazy. In `Renderer::draw`, se la pipeline ha
`fragmentScissor` e lo scissor hardware calcolato copre tutto il target, usare la pipeline
gemella con `fragmentScissor=false` (chiave diversa, puntatore memorizzato in `Pipeline` per
evitare lookup). Il controllo già presente a `:1148-1151` garantisce che la variante senza
discard non riceva mai scissor parziali. La variante va registrata nel manifest hot per il
prewarm, altrimenti la prima occorrenza costa una compilazione.
Estensione da validare: scissor allineato alla griglia dei tile (32 px) o ai bordi del target
→ il region clip è esatto; va provato con il probe "scissor non allineati" già esistente prima
di abilitarlo.

**Proposta (fase 2, scissor parziali esatti senza discard):** usare il **mask bit** on-chip
(libgxm Ov. §6.7 "Mask Update", §6.8): alla prima draw con un nuovo scissor parziale, due
triangoli con il programma di `sceGxmShaderPatcherCreateMaskUpdateFragmentProgram()`
(stencil func NEVER sul target intero → mask 0; ALWAYS sul rettangolo → mask 1), e
il test mask (sempre attivo on-chip) per le draw successive; al ritorno a uno scissor pieno
si rimette mask=1 ovunque. Il test mask avviene prima dello shading, a granularità pixel,
quindi nessuna pipeline ha più bisogno di `discard` per lo scissor. Perché la maschera
sopravviva a un eventuale partial render la surface deve essere `DF32M` (stessi 4 byte di
`DF32`, GPU UG §11) invece di `DF32`; con D16 non c'è equivalente a pari costo. Servono test
hardware accurati: è un lavoro separato.

**Rischio:** fase 1 basso (stesso output, meno lavoro). Fase 2 medio.

### 🔴 S2 — Il wrap hardware non viene mai usato dallo shader

**Dove:** `PipelineDesc::nativeTextureWrapMask` (`vita_gfx_types.hpp:213`) non è assegnato da
nessun file (verificato con grep: solo default 0, hash in `vita_pipeline_key.cpp:14` e lettura
in `gxm_renderer.cpp:1160`). Quindi `gxm_shader_gen.cpp:552-556` emette sempre
`gx_wrap_uv(...)`.

**Oggi:** ogni `tex2D` è preceduto da `gx_wrap_coord` (confronti, `frac`, `abs`) per asse, e
**nello stesso tempo** `bind_texture` programma già `SCE_GXM_TEXTURE_ADDR_REPEAT/MIRROR` in
hardware sulle texture swizzled (`:940-943`): il lavoro è doppio. Inoltre con filtro bilineare
il wrap software sulla coordinata produce giunzioni diverse da quelle hardware.

**Perché costa:** ALU per pixel e, soprattutto, coordinata modificata nel fragment shader =
**dependent texture read** (GPU UG §14.14, USSE Tut. §2.4): niente prefetch del PDS, thread in
attesa della TCU, più registri.

**Proposta:** il wrap software serve solo per texture **linear** (non swizzled: NPOT e target
EFB) con repeat/mirror. Calcolare il bit per unità al momento della risoluzione del binding
(DrawSink/facade conoscono il sampler; la GXM sa se la texture è swizzled — esporre
`texture_is_swizzled(handle)`):
`nativeWrap[i] = swizzled || (wrapS==Clamp && wrapT==Clamp)`. Il bit entra nella chiave
pipeline (già hashato). `bind_texture(..., requireNativeWrap)` esiste già per validarlo.

**Rischio:** medio-basso: cambia il filtraggio ai bordi di texture repeat (lo avvicina
all'hardware GX). Validare con checker asimmetrico + repeat/mirror, come chiede la wiki.

### 🟠 S3 — Trasformazione UV nel fragment shader

**Dove:** `gxm_shader_gen.cpp:533` (`gx_sample_uv`), `:555`, `:143`; uniform
`u_tex_transform` caricata in `bind_pipeline` (`gxm_renderer.cpp:1046-1051`, `:1085-1087`).

**Oggi:** `tex2D(u_texN, gx_sample_uv(tev_uv, u_tex_transform[N]))` per ogni fetch: scala/bias
(crop, flip EFB) applicati per pixel. Anche con S2 risolto la coordinata resta modificata →
dependent read.

**Proposta:** applicare `uv*t.xy+t.zw` nel **vertex shader** ed emettere un varying per ogni
coppia (texCoord, texture) usata da uno stage non indiretto; nel fragment
`tex2D(u_texN, v_uvK.xy)` diventa non-dependent (USSE Tut. §5.1: "spostare i calcoli delle
coordinate nel vertex program"). Gli stage indiretti restano dependent per natura.
Con coordinate proiettive (`Matrix3x4`) usare il layout `xyw` e `tex2Dproj` (unica forma
proiettiva non-dependent, USSE Tut. Tabella 4) oppure dividere nel VS quando la
prospettiva della coordinata non conta.

**Rischio:** medio. Aumenta il numero di varying solo quando la stessa texcoord campiona
texture diverse; verificare il limite di 10 TEXCOORD e il costo ITR.

### 🟠 S4 — Branch su uniform in ogni fetch

**Dove:** `gxm_shader_gen.cpp:557-561`.

**Oggi:** dopo ogni `tex2D`: `if(u_tex_copy_mode[i]>1.5){...} else if(...>0.5){...}` e
`raw_tex.a=lerp(raw_tex.a,1.0,u_tex_force_opaque[i])`, anche per le normali texture della cache
(dove i valori sono sempre 0).

**Perché costa:** flow control nello shader: se il compilatore lo tratta come dinamico il
programma gira in **per-instance mode** (1 istanza per thread invece di 4, GPU UG §4.4);
anche statico, la guida chiede di preferire varianti separate (USSE Tut. §5.5). Più
istruzioni per ogni sample.

**Proposta:** spostare `sampleFormat` (2 bit) e `forceOpaque` (1 bit) per unità nella
`PipelineDesc` (solo le texture EFB li hanno ≠0; DrawSink li conosce quando risolve i binding)
e generare il codice solo per quelle unità. Le pipeline "normali" perdono branch e `lerp`.

**Rischio:** basso sulla correttezza; aumenta un po' le varianti solo per draw che campionano
copie EFB R4/A8/RGB565.

### 🟡 S5 — Varying `TEXCOORD` sempre `float3`

**Dove:** `gxm_shader_gen.cpp:400-403`, layout CPU `gpu_vertex_layout` (`vita_vertex_decode.cpp:264-279`,
12 byte per texcoord).

**Oggi:** la `.z` serve solo per texgen `Matrix3x4` (`coordinate()`, `:122-127`), ma ogni
texcoord è `float3` sia come attributo sia come varying.

**Perché costa:** il parameter buffer salva TEXCOORD da 2-4 componenti e "le componenti non
usate sprecano spazio" (Shader UG, *Vertex Output Semantics*); trasferimento ITR
proporzionale alla dimensione (GPU UG §14.11); 4 byte/vertice in più per texcoord nello
streaming CPU.

**Proposta:** `float2` (attributo e varying) quando la texcoord non è proiettiva; il bit è già
deducibile dalla `PipelineDesc`. Valutare in seguito `TEXCOORDn_HALF` solo per coordinate in
range piccolo (precisione da validare). I `COLOR0/1` sono già salvati come `unsigned char4`
nel parameter buffer: nessun intervento.

**Rischio:** basso (nessun cambio numerico se `.z` non è letta).

### ⚪ S6 — Fog range con ternari annidati

**Dove:** `fog_range()` (`:177-181`) chiamata due volte (`:606-607`).
Sostituire con indicizzazione di array uniform (`u_fog_range_k[int(lo)]`) o, meglio,
valutare la correzione per vertice. Solo per pipeline con `fogRangeEnabled`.

---

## C — Overhead CPU per draw

### 🟡 C1 — Cache di stato azzerate a ogni `BeginScene`

**Dove:** `gxm_renderer.cpp:440-442`.

**Oggi:** a ogni nuova scena si invalidano pipeline, viewport, scissor, vertex stream, texture
e uniform: la prima draw di ogni scena rifà tutti i `sceGxmSet*`.

**Cosa dice l'SDK:** programmi, stream, texture, uniform buffer e tutto lo stato persistono
"indefinitamente, anche tra scene" (libgxm Ov. §6.1); si perdono solo le reservation di
default uniform a fine scena (§6.3) e il region clip viene resettato alla valid region
da `BeginScene` (§6.8).

**Proposta:** in `ensure_scene` invalidare solo `vertexUniformState`, `fragmentUniformState` e
`scissorValid`. Tenere il resto. Guadagno modesto ma gratuito sui frame con molte scene.

**Rischio:** basso; se il cambio di render target alterasse qualche stato lo si vede subito
nel probe. In alternativa, mantenere l'invalidazione solo del viewport al cambio target.

### 🟡 C2 — Lookup e validazioni prima dei fast path

**Dove:** `draw()` (`:1116-1199`), `bind_texture()` (`:917-960`).

**Oggi, per draw:** 3 `FlatHashMap::find` (pipeline, VB, IB) + ricerca del pipeline in
`bind_pipeline` (evitata con l'hint) + per ogni texture: `textures.find`, 10 controlli
`isfinite`/range e una seconda `ensure_scene`, **prima** di scoprire che il binding è identico
al precedente (`:928-932`).

**Proposta:** controllare prima il fast path (unità valida + stesso handle + stesso sampler),
spostare la validazione del sampler dove il sampler viene costruito (una volta per stato),
memorizzare nel `DrawPacket`/comando i puntatori risolti di pipeline e buffer (validi fino a
`finish()`/distruzione, che già invalidano). `ensure_scene` una sola volta per draw.

### 🟡 C3 — Upload uniform generico

**Dove:** lambdas `uploadVertex`/`upload` (`:998-1002`, `:1074-1078`).

**Oggi:** per ogni parametro: `sceGxmProgramParameterGetComponentCount` ×
`GetArraySize` + `sceGxmSetUniformDataF` (chiamata di libreria con i propri controlli).

**Proposta:** alla creazione della pipeline precalcolare per ogni parametro l'offset
(`sceGxmProgramParameterGetResourceIndex`, in word dentro la default uniform buffer) e il
numero massimo di float; in `bind_pipeline` scrivere con `memcpy` sequenziali nella
reservation. Le reservation stanno nel ring del contesto (memoria uncached): scritture
sequenziali e contigue sono il caso migliore.

### 🟠 C4 — `FixedVertexUniforms` da ~3 KiB per ogni draw GPU

**Dove:** `aurora_vita_draw_sink.cpp:838` (`fixed_vertex_uniforms` + `push_back` nel deque),
`gxm_renderer.cpp:993-995` (`memcmp` completo), `:1031` (copia nella cache), `vita_fixed_vertex.hpp:132-165`.

**Oggi:** la struttura (12+12+3×120+2×8×12+32+160+4 float ≈ 772 float ≈ 3,1 KiB) viene
costruita da zero, copiata nel deque, confrontata per intero con l'ultima e, se diversa,
copiata di nuovo: ~12 KiB di traffico memoria CPU per draw prima ancora dell'upload,
indipendentemente da quali blocchi lo shader usa (le palette da 120 float sono copiate anche
per pipeline che non le leggono).

**Proposta:** revisioni per blocco lato stato GX (palette posizione/normale/texture, luci,
materiali) incrementate solo quando il frontend cambia quei registri; il packet porta le
revisioni + puntatore allo stato immutabile del chunk; il renderer confronta interi e fa
upload solo dei blocchi usati dalla pipeline (`p.gx*` non nulli). Costruire solo i blocchi
richiesti dalla pipeline.

**Rischio:** medio (invalidazione): coprire con i test host esistenti di fixed vertex.

### 🟡 C5 — Confronto uniform fragment

**Dove:** `bind_pipeline` `:1035-1070`.

**Oggi:** per ogni draw si costruiscono `textureFlags` e `textureTransform` (8 elementi) e si
esegue una catena di 12 `memcmp` (incluse matrici indirette 6×vec4) anche quando nulla è
cambiato.

**Proposta:** come C4: generazione/revisione degli uniform fragment calcolata in DrawSink
quando `g_gxState` cambia (esiste già `pipelineStateGeneration`/`stateDirty`), confronto di
un intero nel renderer. Tenere il `memcmp` solo come fallback di debug.

### 🟡 C6 — Draw streamed mai unite

**Dove:** `enqueue_streamed_draw` (`vita_draw_adapter.cpp:449-461`) vs `enqueue_draw`
(`:438-443`, che unisce draw compatibili con `batch_compatible`).

**Oggi:** il percorso streamed — quello normale — emette una `sceGxmDraw` per ogni draw GX,
anche quando pipeline, texture, uniform, viewport e scissor coincidono con la precedente.

**Proposta:** scrivere indici assoluti rispetto alla base del VBO dello slot (come fa già il
percorso non-streamed con `upload_rebased_indices`) ed estendere il tail-merge alle draw
streamed contigue. Meno chiamate GXM e meno confronti uniform; vincolo: indici < 64000
per stream a 16 bit (già verificato altrove).

### 🟡 C7 — `pipeline_key` ricalcolato

**Dove:** `vita_pipeline_key.cpp:5-27`; chiamate in `aurora_vita_draw_sink.cpp:456`, `:594`
(**ogni draw GPU**), `PipelineCache::get_or_create` (`gxm_facade.cpp:248`), di nuovo in
`gxm::Renderer::create_pipeline` (`gxm_renderer.cpp:691`); più l'aggiornamento della mappa
`hot_` a ogni `get_or_create`.

**Oggi:** una chiave completa = ~600 `hash_combine(fnv1a64(campo))` su campi di 1-4 byte.

**Proposta:** (a) memoizzare la chiave GPU per `(stateGeneration, flag sprite/texmtx)`;
(b) passare la chiave già calcolata dalla facade al renderer nativo; (c) in un secondo
momento serializzare la `PipelineDesc` canonica in un blob compatto e fare un solo hash.

### ⚪ C8 — Copie di statistiche per draw

**Dove:** `aurora_vita_draw_sink.cpp:767` (`statsBeforeTexture`, dentro il loop texture) e
`:813` (`statsBeforeEnqueue`), eseguite anche con `telemetry_ == nullptr`;
`gxm_facade.cpp:551-555` copia 9 campi di statistiche a ogni draw.
Proteggere con `if(telemetry_)` e aggiornare le statistiche della facade una volta per flush.

---

## M — Sincronizzazione, memoria e upload

### 🔴 M1 — `finish()` come unico meccanismo di ritiro risorse

**Dove:** `finish()` (`:1278-1292`), `update_buffer` (`:1300`), `update_texture` (`:1337`),
`destroy_buffer` (`:1350`), `destroy_pipeline` (`:1359`), `destroy_texture` (`:1374`),
`upload_target` (`:1470`); chiamanti: eviction di `StaticGeometryCache`
(`vita_static_geometry.hpp:191-192`), `TextureCache::erase`, `PipelineCache::evict_one`,
`EfbManager::destroy`, comando `Barrier`.

**Oggi:** una risorsa usata in qualunque momento dall'ultimo `finish()` ha `inFlight=true`;
distruggerla o riscriverla chiama `sceGxmFinish` (CPU ferma finché la GPU ha finito *tutto*),
poi `finish()` scorre **tutte** le mappe per azzerare i flag. Un'eviction di geometria o
texture durante il gameplay = stallo completo del pipelining CPU/GPU.

**Proposta:** seriale di sottomissione: `uint64 submitSerial` incrementato a ogni
`EndScene`, `lastUseSerial` per risorsa, `completedSerial` aggiornato con le notifiche
fragment di `sceGxmEndScene` (`SceGxmNotification`, libgxm Ov. §6.11) o, più semplice, dal
giro della display queue (dopo `displayBuffers` frame). Le distruzioni vanno in una coda
`pending_free` rilasciata quando `completedSerial >= lastUseSerial`; `finish()` resta solo
per readback e shutdown.

**Rischio:** medio (ciclo di vita): serve un test host che simuli le notifiche, come quello
già scritto per i worker.

### 🔴 M2 — Display copy: stallo e VBO condiviso

**Dove:** `copy_display_region` `:1759-1763`, `blit_to_default` `:1657-1663`.

**Oggi:** quando cambia il rettangolo sorgente, `copy_display_region` riscrive `blitVertices`
con `update_buffer(..., storageRetired=false)`: il buffer è quasi sempre in volo (usato nel
frame precedente) → `finish()`. Inoltre `blitVertices` è **lo stesso buffer** creato da
`blit_to_default` con UV diverse (`0..2` invertite): dopo una display copy con crop, un
successivo `blit_to_default` (comando `CopyEfb` → `blit_efb`, o present con `mainEfb_`)
disegna con le UV della display copy — e viceversa `copy_display_region` crede che il buffer
contenga ancora il suo rettangolo (`displayCopySourceValid`). È un bug di correttezza oltre
che di prestazioni.

**Proposta:** vertici statici (triangolo a schermo intero con UV base, come `copyVertices`) e
crop/flip tramite `TextureBinding::uvScale/uvBias` → `u_tex_transform`, esattamente come fa
già il percorso GPU di `copy_current_to_target`. Nessuna scrittura di buffer, nessun
`finish()`, nessuna condivisione. (Con S3 la trasformazione finisce comunque nel VS.)

**Rischio:** basso; il probe copre crop e flip.

### 🟡 M3 — Un memblock per buffer

**Dove:** `create_buffer` (`:793-807`) → `MemoryBlock::allocate(CpuGpu)` →
`sceKernelAllocMemBlock(USER_RW_UNCACHE, round4K)` + `sceGxmMapMemory` per ogni buffer.
`StaticGeometryCache` crea 2 buffer per mesh (`vita_static_geometry.hpp:174-176`).

**Perché costa:** due syscall per allocazione e altrettante in rilascio, minimo 4 KiB per
buffer (una mesh da 48 vertici occupa 8 KiB), frammentazione del numero limitato di memblock.

**Proposta:** sub-allocare da un heap `USER_RW_UNCACHE` mappato una volta (stesso schema di
`CdramPool` con `sceClibMspace`), un'unica allocazione VB+IB per mesh statica.

### 🟡 M4 — Streaming con doppia scrittura

**Dove:** `vita_streaming_arena.cpp:137-150` (scrittura in `vertexStage_` cached),
`:206-219` (`flush` → `update_buffer` → `memcpy` verso memoria GXM uncached).

**Oggi:** vertici e indici vengono scritti in staging cached e poi copiati interamente in
memoria uncached. Il commento motiva la scelta con l'assenza di writeback per memoria
*cached* mappata; ma il buffer di destinazione è già **uncached** (`USER_RW_UNCACHE`), quindi
scriverci direttamente non ha problemi di coerenza (libgxm Ov. §10: la CPU non passa dalla
cache, la GPU legge dalla memoria).

**Proposta:** A/B "direct uncached write" per GXM: `reserve()` restituisce il puntatore
nella memoria GXM dello slot; il pack dei vertici scrive in modo sequenziale (write buffer
ARM) e sparisce la `memcpy` finale. Da misurare: le scritture sparse/parziali su uncached
sono lente, quindi il pack deve scrivere ogni vertice per intero e in ordine
(`pack_gpu_vertex` lo fa già). Mai leggere da quella memoria (niente read-modify-write).

**Rischio:** basso sulla correttezza, esito prestazionale da misurare.

### 🟡 M5 — Upload texture a tre passaggi

**Dove:** `gxm_texture_layout.cpp:19-66` e `gxm_renderer.cpp:190-208`, `:848-861`.

**Oggi:** decode GX → vettore RGBA lineare → nuovo vettore swizzled con un `memcpy` di 4 byte
per texel → `memcpy` in CDRAM; vettori nuovi per ogni livello mip; mip generati con un loop
per canale e `std::min` per campione.

**Proposta (in ordine):** riusare buffer scratch; swizzle a blocchi (righe di 2/4 texel con
tabelle `spread` precalcolate, o decode diretto dei tile GX 4×4/8×4 in ordine Morton);
alternativa hardware: upload lineare e `sceGxmTransferCopy` linear→swizzled sul PTLA
(libgxm Ov. §7 Tabella 6), asincrono. Riduce gli hitch sui cache miss durante il gameplay.

### ⚪ M6 — Strutture O(N) nelle cache

- `TextureCache::pre_evict` (`gxm_facade.cpp:53-63`): scansione completa per ogni vittima →
  O(N²) sotto pressione. Usare una lista LRU intrusiva.
- `TextureCache::invalidate_source_range` (`:156-168`): scansione di tutte le texture e
  `std::vector` allocato a ogni invalidazione. Indice ordinato per `sourceId`.
- `PipelineCache::evict_one` (`:174-181`): scansione completa.
- `Renderer::destroy_texture` (`gxm_renderer.cpp:1377-1378`): scansione di `textureCache` per
  trovare la chiave; memorizzare la chiave nella `Texture`.

---

## Piano di lavoro proposto sul branch

Ogni fase = commit separati, build GXM + test host verdi, poi confronto su hardware con il
probe GX (`AURGXGM01`) e con la stessa sequenza Strikers (≥300 frame warm, mediana/p95/p99,
screenshot, hash eboot), come da REGRESSION_AUDIT.

1. **Fase 0 – misura.** Telemetria per scena: load/store depth, scene per frame, tempo GPU per
   scena (notifiche fragment), `finish()` chiamati per frame con causa. Senza questo non si
   possono attribuire i guadagni.
2. **Fase 1 – semantica identica, banda GPU:** G3, G2, G1 (telemetria), S1 fase 1, M2.
3. **Fase 2 – CPU per draw a semantica identica:** C1, C2, C3, C8, C7(a,b), M6.
4. **Fase 3 – sincronizzazione:** M1 (seriali + coda di rilascio), M3.
5. **Fase 4 – shader (validazione visiva):** S2, S4, S5, poi S3.
6. **Fase 5 – lavori più ampi:** C4/C5 (revisioni di stato), C6 (merge streamed), M4, M5,
   S1 fase 2 (mask bit), G4 (A/B parameter buffer).

## Indice rapido per file

| File | Voci |
|---|---|
| `gxm/gxm_renderer.cpp` | G1, G2, G3, S1, C1, C2, C3, C4, C5, M1, M2, M3, M5, M6 |
| `gxm/gxm_renderer.hpp` | G4 |
| `gxm/gxm_shader_gen.cpp` | S1, S2, S3, S4, S5, S6 |
| `gxm/gxm_texture_layout.cpp` | M5 |
| `gxm/gxm_facade.cpp` | C7, C8, M6 |
| `gxm/gxm_memory.cpp` | M3 |
| `gx/aurora_vita_draw_sink.cpp` | S2, S4, C4, C7, C8 |
| `gfx/vita_draw_adapter.cpp` | C6 |
| `gfx/vita_streaming_arena.cpp` | M4 |
| `gfx/vita_static_geometry.hpp` | M1, M3 |
| `gfx/vita_pipeline_key.cpp` | C7 |
| `gfx/vita_gfx_types.hpp` | S1, S2, S4 |
