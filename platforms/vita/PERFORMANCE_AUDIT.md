# Audit performance — Aurora Vita (backend GXM + vitaGL)

> Documento di analisi statica del codice, non di misura. Nessun numero di questo
> documento è stato verificato su hardware. Le stime di guadagno sono ragionamenti
> sull'architettura di SGX543MP4+ e della Vita, non risultati. Vale la stessa regola
> di `PERFORMANCE_LAB.md`: una build che compila e un overlay a 60 FPS non sono prove
> di 60 FPS sostenuti in gameplay.

**Indice**

- [0. Punto di partenza e metodo](#0-punto-di-partenza-e-metodo)
- [A. Percorso GPU / GXM](#a-percorso-gpu--gxm)
- [B. Percorso CPU per-vertex](#b-percorso-cpu-per-vertex)
- [C. Banda di memoria](#c-banda-di-memoria)
- [D. Overhead per-draw](#d-overhead-per-draw)
- [E. Flag di compilazione](#e-flag-di-compilazione)
- [F. Parallelismo CPU](#f-parallelismo-cpu)
- [G. Piano operativo](#g-piano-operativo)
- [H. Protocollo di misura](#h-protocollo-di-misura)
- [Indice rapido per file](#indice-rapido-per-file)

---

## 0. Punto di partenza e metodo

I dati registrati in `PERFORMANCE_LAB.md` (scena introduttiva Strikers, misure hardware):

| Fase | Mediana |
|---|---|
| Transform CPU | 69,0 ms (dopo ottimizzazione; 95,6 ms prima) |
| Frame intero | 426,6 ms (prima: 453,1 ms) |
| Target | 16,67 ms |

**Il transform CPU è circa il 16% del frame.** Anche azzerandolo completamente si resterebbe
lontanissimi dal target. Il grosso del tempo sta altrove, e questo audit parte da lì.

### Perché le regole sono diverse qui

SGX543MP4+ è un **TBDR** (tile-based deferred renderer). Le conseguenze pratiche, tutte
rilevanti per questo codice:

1. **Il numero di *scene* conta più del numero di draw.** Ogni `sceGxmBeginScene`/`EndScene`
   implica load e store dei tile verso la RAM.
2. **`discard` / alpha-kill disattivano l'HSR** (hidden surface removal). Uno shader
   punch-through costringe il tiler a eseguire il fragment per ogni frammento di ogni
   triangolo, in ordine, invece di scartare gli occlusi prima dello shading.
3. **Il depth buffer non dovrebbe mai toccare la RAM.** Vive nella tile memory. Forzarne
   lo store annulla il vantaggio strutturale del tiler.
4. **Le coordinate texture dipendenti** (calcolate nel fragment shader invece di
   interpolate) perdono il prefetch della TMU.

Tre delle quattro sono violate dal codice attuale.

### Come leggere le priorità

| Simbolo | Significato |
|---|---|
| 🔴 | Guadagno atteso alto, rischio basso. Fatelo per primo. |
| 🟠 | Guadagno alto, richiede validazione visiva. |
| 🟡 | Guadagno medio, poco rischio. |
| ⚪ | Pulizia / abilitante per altri interventi. |

---

## A. Percorso GPU / GXM

### 🔴 A1 — Lo scissor via `discard` disattiva l'HSR

**Dove:** `platforms/vita/gxm/gxm_shader_gen.cpp:451-452`, default a `platforms/vita/gfx/vita_gfx_types.hpp:212`

```cpp
if (d.fragmentScissor)
  fs << "if(window_position.x<u_clip_rect.x||window_position.y<u_clip_rect.y||"
        "window_position.x>=u_clip_rect.z||window_position.y>=u_clip_rect.w) discard;\n";
```

`fragmentScissor` è `true` di default e viene disattivato solo per il clear
(`gxm_renderer.cpp:636`). Quindi **ogni fragment program generato contiene un `discard`**.

**Perché costa:** su SGX un fragment program con `discard` diventa punch-through. Il
tiler non può più determinare la visibilità prima dello shading e deve eseguire lo shader
per ogni frammento di ogni triangolo, rispettando l'ordine di sottomissione. Con
l'overdraw tipico di una scena GC/Wii il costo di fragment può moltiplicarsi di 2-4x.
In più `window_position` (WPOS) è un input aggiuntivo che va interpolato e passato.

**Cosa fare:** GXM ha lo scissor hardware, a costo zero.

```cpp
// in Renderer::draw(), accanto al viewport caching di gxm_renderer.cpp:838-844
if(!d.scissorValid || !same_scissor(d.cachedScissor, packet.scissor)) {
  sceGxmSetRegionClip(d.context, SCE_GXM_REGION_CLIP_OUTSIDE,
                      packet.scissor.x, packet.scissor.y,
                      packet.scissor.x + packet.scissor.width  - 1,
                      packet.scissor.y + packet.scissor.height - 1);
  d.cachedScissor = packet.scissor; d.scissorValid = true;
}
```

Poi `fragmentScissor` diventa `false` su tutto il backend GXM: sparisce il `discard`,
sparisce WPOS, e `pipeline_key()` genera meno varianti da compilare (beneficio collaterale
sui tempi di caricamento).

**Da verificare:** il region clip è applicato dal raster a granularità di pixel, quindi
la semantica GX resta esatta. Il caso da osservare è lo scissor che taglia dentro un tile
su geometria con alpha-test, dove il clip resta comunque pre-fragment.

**Nota:** l'alpha-test GX (`alpha_test()`, `gxm_shader_gen.cpp:68-88`) genera comunque un
`discard` quando è realmente attivo. Quello è inevitabile e corretto — ma va limitato alle
pipeline che lo usano davvero, cosa che il codice già fa.

---

### 🔴 A2 — Depth force-store sempre attivo, formato DF32

**Dove:** `platforms/vita/gxm/gxm_renderer.cpp:295-298` e `:434`

```cpp
sceGxmDepthStencilSurfaceSetForceLoadMode(ds, loadDepth ? ..._ENABLED : ..._DISABLED);
sceGxmDepthStencilSurfaceSetForceStoreMode(ds, SCE_GXM_DEPTH_STENCIL_FORCE_STORE_ENABLED);
```
```cpp
sceGxmDepthStencilSurfaceInit(&d.depthSurface, SCE_GXM_DEPTH_STENCIL_FORMAT_DF32, ...)
```

**Perché costa:** 960 × 544 × 4 byte = **2 MB di depth scritti in RAM alla fine di ogni
scena**, più altrettanti riletti se `loadDepth`. Lo store è incondizionato, anche
sull'ultima scena del frame, il cui depth viene subito buttato via.

Il motivo per cui è stato messo (preservare Z attraverso gli split di scena causati da
`GXCopyTex`) è legittimo. Il problema è che è sempre attivo.

**Cosa fare, in ordine di semplicità:**

1. **Niente force-store sull'ultima scena.** In `end_frame()` nessuno leggerà più quel
   depth: rimettere `FORCE_STORE_DISABLED` prima dell'`end_scene()` finale. 2 MB per frame
   risparmiati, cambio di poche righe, rischio nullo.
2. **Store condizionale.** Un flag `depthNeededAfterScene`, settato dai path
   `copy_tex` / `bind_target` che sanno che seguirà un'altra scena. Copre il caso reale
   senza penalizzare i frame a scena singola.
3. **`SCE_GXM_DEPTH_STENCIL_FORMAT_D16`** al posto di DF32: dimezza load e store
   (2 MB → 1 MB) e raddoppia il throughput del depth test nella tile memory.
   GX usa 24 bit; D16 può introdurre z-fighting su scene con range di profondità ampio.
   **Va misurato per gioco** — se si vedono artefatti, restate su DF32 con lo store
   condizionale del punto 2.

---

### 🟡 A3 — Numero di scene per frame

**Dove:** `gxm_renderer.cpp:415` e `:1000` (`rt.scenesPerFrame = 1`), logica in
`ensure_scene()` / `end_scene()` (`gxm_renderer.cpp:275-305`)

Due problemi distinti.

**(a) `scenesPerFrame` dichiara 1 ma ne fate molte di più.** Ogni `GXCopyTex` e ogni
`bind_target` chiude e riapre una scena. `scenesPerFrame` dimensiona la memoria driver del
render target: mettetelo al valore realistico osservato (es. 8). Non è un bug funzionale
ma influenza l'allocazione interna e può causare stalli.

**(b) Ogni scena costa un load + store completo di color e depth:**

| Voce | Byte a 960×544 |
|---|---|
| Color load | 2,0 MB |
| Color store | 2,0 MB |
| Depth load (se `loadDepth`) | 2,0 MB |
| Depth store (con A2 attuale) | 2,0 MB |
| **Totale per scena** | **fino a 8 MB** |

Con 5 scene per frame a 30 FPS sono ~1,2 GB/s di solo traffico tile — una frazione
enorme della banda disponibile.

**Cosa fare:** prima di tutto **misurare**: un contatore `stats.sceneCount` incrementato in
`ensure_scene()` costa 5 righe e dice se questo item conta o no. Se il numero è > 2-3, il
lavoro più redditizio è raggruppare i `GXCopyTex`: `DrawSink::copy_tex`
(`platforms/vita/gx/aurora_vita_draw_sink.cpp:307`) chiama `flush()` incondizionatamente,
ma se due copy_tex arrivano senza draw in mezzo la seconda non richiede una nuova scena.

---

### 🟠 A4 — Risoluzione interna a 960×544

**Dove:** `platforms/vita/aurora_vita_backend.hpp:21` (`width=960, height=544`)

Un gioco GameCube renderizza a 640×480 o 640×448. Renderizzare a 960×544 significa
**1,74x i pixel** rispetto al contenuto, con tutto il costo di fragment, depth test,
tile load/store ed EFB copy che scala linearmente.

**Cosa fare:** separare la risoluzione di rendering da quella di display. Renderizzare a
640×448 (o 720×408 in 4:3 con pillarbox) e fare l'upscale finale nel path
`blit_to_default` / `copy_display_region` che esiste già.

> È probabilmente **la singola manopola col miglior rapporto effort/FPS** dell'intero
> documento, e non richiede di toccare alcuna logica GX.

Serve anche come **strumento diagnostico**: vedi [H](#h-protocollo-di-misura).

---

### 🟠 A5 — Tutte le texture espanse a RGBA8888

**Dove:** `gxm_renderer.cpp:562-600` (`create_texture` usa sempre
`SCE_GXM_TEXTURE_FORMAT_U8U8U8U8_ABGR`); gating a
`platforms/vita/gfx/vita_texture_cache.cpp:16-21`:

```cpp
#define AURORA_VITA_NATIVE_CMPR 0
#define AURORA_VITA_NATIVE_GX_TEXTURES 0
```

`transcode_texture_native()` esiste già (`vita_texture_decode.cpp:127+`) ma è codice morto
per il backend GXM.

**Perché costa:**

| Formato GX | Espansione a RGBA8 | Esempio 256×256 |
|---|---|---|
| CMPR | **8x** | 32 KB → 256 KB |
| I4 | 8x | 32 KB → 256 KB |
| I8 / IA4 | 4x | 64 KB → 256 KB |
| IA8 / RGB565 | 2x | 128 KB → 256 KB |

Con un budget texture di 24 MB (`texture_cache_budget`), si sta facendo thrashing della
cache con fino a 8x i byte necessari — più il costo CPU del decode per texel
(`cmpr_block()`, `vita_texture_decode.cpp:73-88`) e la banda di sampling sprecata a
runtime.

**Cosa fare:** mappare i formati GX su quelli nativi GXM.

| Formato GX | Formato GXM | Lavoro necessario |
|---|---|---|
| CMPR | `UBC1_ABGR` | transcode per blocco: byte-swap dei due colori + inversione delle coppie di selettori (`reverse_cmpr_selector_pairs()` esiste già a `vita_texture_decode.cpp:70`) |
| I8 | `U8_RRRR` | copia diretta |
| IA8 | `U8U8_GGGR` o equivalente | copia + swap |
| RGB565 | `U5U6U5_RGB` | byte-swap BE→LE |
| I4 / IA4 | espandere a I8 / IA8 | 2x invece di 8x |
| RGB5A3, RGBA8, C4/C8/C14X2 | RGBA8 (invariato) | nessuno |

**Sulla fedeltà di CMPR→BC1:** GX interpola i colori intermedi a 3/8 e 5/8, DXT1 a 1/3 e
2/3. È un errore di colore sub-percettivo nella modalità a 4 colori; la modalità a 3 colori
con trasparenza è compatibile. È esattamente ciò che fa Dolphin quando decodifica in
hardware.

---

### 🟠 A6 — Emulazione software del wrap UV

**Dove:** `gxm_shader_gen.cpp:444-466`

```cpp
"float gx_wrap_coord(float v,float mode){if(mode<0.5)return clamp(v,0.0,1.0);"
"if(mode<1.5)return frac(v);float f=frac(v*0.5)*2.0;return 1.0-abs(f-1.0);}\n"
```

Serve perché `gxm_renderer.cpp:551-553` marca una texture come `swizzled` solo se
POT **e `mipCount <= 1` e `!generateMipmaps`**, e su GXM il wrap hardware richiede
il layout swizzled.

**Perché costa:** ALU per-texel (branch + `frac` + `abs`), ma soprattutto la coordinata
diventa **dipendente**: su SGX il sample perde il prefetch della TMU e diventa una
dependent texture read, molto più costosa.

**Cosa fare:** `sceGxmTextureInitSwizzled` **supporta le mipmap**. Estendere
`prepare_swizzled_texture()` (`platforms/vita/gxm/gxm_texture_layout.cpp:6-34`) a swizzlare
l'intera catena mip livello per livello. Con questo ogni texture POT — la stragrande
maggioranza del contenuto GC — usa il wrap hardware e `gx_wrap_uv` sparisce dagli shader
generati.

---

## B. Percorso CPU per-vertex

### 🟡 B1 — `CanonicalVertex` è da 168 byte

**Dove:** `platforms/vita/gfx/vita_vertex_decode.hpp:99-108`

```cpp
struct CanonicalVertex {
  float position[4];                    //  16
  float normal[3], binormal[3], tangent[3];  // 36
  uint8_t color0[4], color1[4];         //   8
  float texcoord[8][3];                 //  96  <-- sempre allocati
  uint8_t pnMatrixIndex;
  uint8_t texMatrixIndex[8];
};                                      // ~168 byte
```

**Perché costa:** un draw tipico usa 1-2 texcoord e nessun binormale/tangente, ma ogni
vertice consuma comunque 168 byte, di cui ~120 sono zeri. La L1 dati della Cortex-A9 è
32 KB: **195 vertici saturano la L1**. Il transform diventa memory-bound, non ALU-bound.

Questo spiega anche perché il parallelismo su tre core rende meno del previsto: tre core
che macinano 168 byte/vertice competono per la stessa L2 da 512 KB.

Il commento a `vita_vertex_decode.hpp:93` riconosce già il problema per lo *stream GPU*
(120 byte invece di 168), ma l'array intermedio CPU resta a 168.

**Cosa fare, in ordine:**

1. **Rendere `prepare_streamed_draw_into()` il percorso di default.** Esiste già
   (`platforms/vita/gfx/vita_draw_adapter.cpp:400+`) e fonde decode → transform → pack in
   un solo passaggio, con `CanonicalVertex v{}` **sullo stack**
   (`decode_transform_pack_range`): il vertice non lascia mai la L1. Attualmente il path
   principale è `prepare_draw_into`, che materializza l'intero array.
   Il path streamed rifiuta linee e punti — quelli restano sul percorso vecchio.
   **È il cambio col miglior rapporto costo/beneficio della sezione B, perché il codice
   c'è già ed è testato.**
2. **Inizializzazione selettiva.** `CanonicalVertex v{}` azzera 168 byte per scriverne poi
   ~24. Con `decodeSemantics` già noto (`decode_semantics_for_pipeline()`), azzerare solo
   i campi che il decode non tocca ma il pack legge.
3. **A tendere:** layout variabile guidato da `decodeSemantics`, o decoder specializzato
   (vedi B2).

---

### 🟡 B2 — Il decoder è un interprete, per attributo e per vertice

**Dove:** `platforms/vita/gfx/vita_vertex_decode.cpp:89-160`

`decode_vertex()` itera su `layout.count` attributi e per ognuno esegue:

- `resolve()` — branch su `VertexSource`, bound check, lookup indicizzato
- `numeric()` — switch su `VertexComponent` dentro un loop su `components`
- uno `switch` su 23 valori di `VertexSemantic`

Sono ~15-25 branch difficilmente predicibili per vertice, su una CPU in-order con
predittore modesto.

**Cosa fare — decoder specializzato per layout.** Il layout cambia raramente ed è già
cachato in `translatedLayout_` (`aurora_vita_draw_sink.cpp:415`). Alla prima comparsa di
un layout, compilare una tabella di micro-op:

```cpp
struct DecodeOp {
  const uint8_t* base;   // array sorgente o stream
  uint16_t srcStride;
  uint8_t  converter;    // enum piccolo: F32x3_BE, S16x3_BE_scaled, U8x2, RGBA8, ...
  uint16_t dstOffset;
  float    scale;
};
```

Il loop per-vertice esegue una sequenza piatta con uno switch su 6-8 converter comuni,
eliminando `resolve()` e la gran parte dei branch.

**Bonus:** i casi comuni (posizione S16 big-endian ×3, texcoord S16 ×2) si vettorizzano
bene in NEON: `vrev16`/`vrev32` + `vmovl` + `vcvt` + `vmul` per 4 vertici alla volta.

---

### 🟡 B3 — Nessuna vettorizzazione NEON nel transform

**Dove:** `platforms/vita/gfx/vita_vertex_pipeline.cpp:37-42`

```cpp
V3 transform(const Matrix3x4& m, V4 p) noexcept {
  return {p.x*m.v[0]+p.y*m.v[1]+p.z*m.v[2]+p.w*m.v[3], ...};
}
```

Struct `V3`/`V4` scalari, ritornate per valore.

**Perché costa:** GCC su ARMv7 **non auto-vettorizza codice floating point senza
`-ffast-math`**, perché NEON non è IEEE-conforme sui denormali. Il backend GXM non ha
`-ffast-math` (vedi [E1](#-e1---ffast-math-manca-sul-backend-gxm)), quindi questo è tutto
VFP **scalare**: ~12 `vmla` per la posizione, altrettanti per ogni fra normale, binormale
e tangente.

Questo probabilmente spiega perché il transform del path GXM è più lento di quello vitaGL
a parità di sorgente.

**Cosa fare:**
1. Prima di tutto sistemare i flag ([E1](#-e1---ffast-math-manca-sul-backend-gxm)) — è
   gratis e abilita l'auto-vettorizzazione.
2. Poi una `transform4()` esplicita in NEON intrinsics (`<arm_neon.h>`, disponibile in
   VitaSDK) che processa 4 vertici alla volta con la matrice tenuta in registri:

```c
x = vaddq_f32(vmlaq_n_f32(vmlaq_n_f32(vmulq_n_f32(px, m00), py, m01), pz, m02),
              vdupq_n_f32(m03));
```

`mathneon` è già linkato (`cmake/aurora_vita_gxm.cmake:29`): verificare se `transform_dir()`
e `norm()` possono usarlo direttamente.

---

### 🟠 B4 — Il path GPU-vertex è disattivato di default

**Dove:** `platforms/vita/aurora_vita_backend.hpp:57` (`static_geometry_budget = 0`)

È **la soluzione strutturale** al problema della sezione B: far fare il transform alla GPU
invece che alla CPU. Il codice esiste:

- `platforms/vita/gfx/vita_static_geometry.hpp` — cache di geometria immutabile verificata
- `platforms/vita/gfx/vita_fixed_vertex.hpp` — eleggibilità e uniform
- `gxm_shader_gen.cpp:358-372` — shader con `u_gx_position_palette` / `u_gx_light`

`PERFORMANCE_LAB.md` documenta che non è mai stato validato su hardware perché la console
si è bloccata a metà test.

> **Massimo potenziale, massimo rischio.** Fare prima A1 + A2 + A4 (basso rischio, guadagno
> certo), poi tornare qui con una console sbloccata e il protocollo di acceptance già
> scritto in `PERFORMANCE_LAB.md`.

**Da sistemare prima di riattivarlo:** `bind_pipeline` carica ~500 float di uniform per
draw in questo path — vedi [D1](#-d1--upload-uniform-incondizionato-a-ogni-draw) — e c'è
un puntatore potenzialmente invalidato in
[D4](#-d4--fixedvertexuniforms_--26-kb-per-draw-e-un-puntatore-a-rischio).

---

## C. Banda di memoria

### 🟠 C1 — Doppia copia dello streaming arena su GXM

**Dove:** `cmake/aurora_vita_gxm.cmake:22` (`AURORA_VITA_DIRECT_STREAM_WRITE=0`),
`platforms/vita/gfx/vita_streaming_arena.cpp:150-155` e `:205-215`

Il flusso attuale per ogni byte di vertice/indice:

```
CPU scrive -> vertexStage_ (vector su heap, cached)
           -> flush() -> pool_.update() -> Renderer::update_buffer
           -> memcpy nella memoria GXM (gxm_renderer.cpp:933)
```

**Ogni byte viene scritto due volte per frame.** Con 4 MB di arena vertici sono fino a
8 MB di traffico aggiuntivo.

**Attenzione prima di "ottimizzare ingenuamente":** i buffer GXM sono allocati
`MemoryKind::CpuGpu` → `kCpuPreferred` → `SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE`
(`platforms/vita/gxm/gxm_memory.cpp:88-93`). Scrivere *direttamente* campo per campo in
memoria uncached sarebbe **peggio** della doppia copia: ogni store va in RAM senza
write-combining. La struttura attuale (staging cached + memcpy bulk verso uncached) è
difendibile.

**Le due vie corrette, in ordine di preferenza:**

1. **Allocare lo streaming buffer in memoria *cached*** (`SCE_KERNEL_MEMBLOCK_TYPE_USER_RW`),
   mapparla con `sceGxmMapMemory`, e fare un writeback esplicito della D-cache prima della
   submit. Con questo `AURORA_VITA_DIRECT_STREAM_WRITE=1` diventa corretto e sicuro per
   GXM: si scrive una volta sola, in memoria cached, con tutta la località che serve.
   Richiede un nuovo `MemoryKind::StreamingCpuWrite` in `gxm_memory.cpp` e la chiamata di
   writeback in `StreamingArena::flush()`.
2. Se (1) non è praticabile: tenere la doppia copia ma garantire che il `memcpy` verso
   uncached sia allineato a 16 byte e multiplo di 16 (già vero grazie a
   `cfg_.alignment = 16`). Il `memcpy` di newlib VitaSDK usa già NEON per blocchi grandi.

---

### 🟠 C2 — Fixup CPU su memoria uncached dopo l'EFB copy

**Dove:** `gxm_renderer.cpp`, in coda a `copy_current_to_target()`

```cpp
auto* pixels = static_cast<uint32_t*>(destination.memory.data());
if(format==EfbCopyFormat::RGB565) { ... row[x] |= 0xff000000u; }
if(flipX) { ... std::swap(row[x], row[destination.width-1u-x]); }
if(flipY) { ... std::swap(top[x], bottom[x]); }
```

Sono read-modify-write pixel per pixel su memoria **uncached**. Su una destinazione
480×272 sono ~130k accessi uncached; il costo per accesso uncached è tipicamente 10-50x
quello cached.

Il commento nel codice osserva che "tocca solo la destinazione 480×272" — vero, ma è
esattamente il caso in cui il fattore uncached domina.

**Cosa fare:** i commit `d8b1464` / `97b90a8` hanno già spostato R4 e lo scaled copy sul
transfer engine. Portare lì anche gli altri due:

- **`forceOpaque`** → si esprime come `colorMask` senza alpha nel fragment program, costo zero.
- **`flipX` / `flipY`** → `sceGxmTransferCopy` non ha flag di mirror, ma un draw
  full-screen con UV invertite su un render target costa quasi nulla su un tiler, ed è già
  quello che fa il path `blit_to_default`.

---

### ⚪ C3 — Partizionamento CDRAM vs RAM di sistema

**Dove:** `gxm_memory.cpp:79-95`, `gxm_renderer.hpp:11-18`

Situazione attuale:

| Risorsa | `MemoryKind` | Destinazione |
|---|---|---|
| Display surface (3 × 2 MB) | `ColorSurface` | CDRAM |
| Texture | `GpuResource` | CDRAM (pool `sceClibMspace`, 64 MB) |
| Depth | `GpuResource` | CDRAM |
| **Vertex/index buffer** | `CpuGpu` | **RAM di sistema, uncached** |

Il layout è ragionevole: se il fragment shading legge texture da CDRAM mentre il vertex
fetch legge da RAM di sistema, i due percorsi non si contendono lo stesso bus.

> **Non cambiare questo senza misurare.** È controintuitivo e dipende fortemente dal gioco.

---

## D. Overhead per-draw

### 🔴 D1 — Upload uniform incondizionato a ogni draw

**Dove:** `gxm_renderer.cpp:694-788`

Il caching copre solo programma, depth e cull:

```cpp
if(!d.pipelineStateValid || d.boundPipelineKey != key) {
  sceGxmSetVertexProgram(...); sceGxmSetFragmentProgram(...);
  sceGxmSetFrontDepthFunc(...); ... sceGxmSetCullMode(...);
  d.pipelineStateValid = true; d.boundPipelineKey = key;
}
```

**Tutto il resto viene ricaricato a ogni singola draw:**
`sceGxmReserveVertexDefaultUniformBuffer` + `sceGxmReserveFragmentDefaultUniformBuffer` +
~15 `sceGxmSetUniformDataF`.

Nel path fixed-vertex ([B4](#-b4--il-path-gpu-vertex-è-disattivato-di-default)) il conto è:

| Uniform | Float |
|---|---|
| `u_gx_position_palette` | 120 |
| `u_gx_normal_palette` | 120 |
| `u_gx_material` + `u_gx_ambient` | 32 |
| `u_gx_light` (fino a 8 luci × 20) | fino a 160 |
| `u_gx_texture[8]` + `u_gx_post[8]` | 192 |
| **Totale** | **~600 float ≈ 2,4 KB per draw** |

Ogni `reserve` consuma spazio dal parameter buffer da 4 MB
(`gxm_renderer.hpp:15`): **overflow dopo ~1700 draw**.

**Cosa fare:** dirty-tracking degli uniform. Tenere l'ultimo `GpuDrawUniforms` caricato
(e l'ultimo puntatore `FixedVertexUniforms`) e saltare reserve + upload se identici e la
pipeline non è cambiata. GXM mantiene il default uniform buffer legato finché non se ne
riserva uno nuovo, quindi è semanticamente valido. Un `memcmp` di 684 byte costa
enormemente meno di 15 chiamate API più 2,4 KB nel parameter buffer.

**Raffinamento:** dividere gli uniform per frequenza di aggiornamento. `u_mvp` cambia
spesso; `u_fog_*`, `u_ind_mtx`, `u_texcoord_scale`, `u_texture_size_bias` quasi mai.
Caricare i gruppi separatamente in base a un dirty-mask elimina la gran parte del traffico.

---

### 🟡 D2 — Lookup ripetuti su `unordered_map` nel path caldo

**Dove:** `gxm_renderer.cpp:791-856`

```cpp
const auto pi = d.pipelines.find(packet.pipelineKey);
const auto vi = d.buffers.find(packet.vertices.buffer);
const auto ii = d.buffers.find(packet.indices.buffer);
// ...
bind_pipeline(packet.pipelineKey, ...);          // rifà find() sulla stessa chiave
// ...
for(...) bind_texture(packet.textures[i].texture, ...);   // find() per unità
// ...
for(...) d.textures.at(packet.textures[i].texture)->inFlight = true;  // di nuovo!
```

Sono ~8-12 hash lookup per draw. `unordered_map` è una lista concatenata di nodi allocati
separatamente: ogni lookup è un hash più un pointer chase in memoria sparsa, pessimo per
la cache. Le texture vengono cercate **due volte**.

**Cosa fare:**

1. Passare a `bind_pipeline` il puntatore `Pipeline*` già risolto invece della chiave.
2. Il flag `inFlight` delle texture è già settato dentro `bind_texture`
   (`gxm_renderer.cpp:689`): **il loop a riga 856 è puramente ridondante, va eliminato.**
3. A tendere: sostituire `unordered_map<Handle,T>` con un vettore denso indicizzato
   dall'handle (gli handle sono già sequenziali, `nextHandle++`) o una tabella
   open-addressing. Elimina hashing e indirezione insieme.

---

### 🟡 D3 — `DrawPacket` da ~950 byte copiata per draw

**Dove:** `platforms/vita/gfx/vita_gfx_types.hpp:459-478`

| Campo | Byte |
|---|---|
| `GpuDrawUniforms uniforms` | ~684 |
| `array<TextureBinding,8> textures` | ~192 |
| resto | ~70 |
| **Totale** | **~950** |

Ogni `stream_.draw(packet)` copia l'intera struttura, e `batch_compatible()`
(`vita_draw_adapter.cpp:139`) fa `memcmp` di 684 byte per ogni tentativo di coalescenza.
Con ~1000 draw per frame sono ~1 MB di copie e ~700 KB di memcmp.

Non è il collo di bottiglia principale, ma è puro spreco.

**Cosa fare:** interning degli uniform. Un pool di `GpuDrawUniforms` per frame con un
indice a 16 bit nel packet; il confronto per il batching diventa un confronto di indici.
Stessa cosa per l'array di `TextureBinding`. Il `DrawPacket` scende a ~60 byte e ne stanno
quattro in una cache line.

---

### 🔴 D4 — `fixedVertexUniforms_` — 2,6 KB per draw e un puntatore a rischio

**Dove:** `platforms/vita/gx/aurora_vita_draw_sink.cpp:705-712`

```cpp
fixedVertexUniforms_.push_back(gfx::fixed_vertex_uniforms(translatedGpuPipeline_, vertexState));
// ...
packet.fixedVertexUniforms = &fixedVertexUniforms_.back();
stream_.draw(packet);
```

`FixedVertexUniforms` (`vita_gfx_types.hpp:403-415`) è ~2,6 KB, di cui 960 byte per le due
palette da 10 matrici 3x4.

**Due problemi:**

1. **Volume:** con il path GPU attivo e centinaia di draw, sono centinaia di KB allocati e
   copiati per frame.
2. **Correttezza — puntatore potenzialmente invalidato.** Si prende l'indirizzo di un
   elemento di uno `std::vector` che continua a crescere: la `push_back` successiva può
   riallocare e **invalidare tutti i puntatori già inseriti nei packet dello stream**. Il
   commento a `vita_gfx_types.hpp:473-474` afferma che l'owner mantiene lo snapshot valido
   fino al ritorno di `execute()`, ma la riallocazione del vector rompe proprio questa
   garanzia.

   Questo è un bug latente, non solo una questione di performance, e potrebbe essere
   collegato ai comportamenti non riproducibili descritti in `PERFORMANCE_LAB.md`.

**Cosa fare:** un `std::deque`, un arena allocator a blocchi, o una `reserve()` con
capacità garantita e un controllo esplicito che non venga superata. La deque è la
correzione minima.

---

## E. Flag di compilazione

**Dove:** `cmake/aurora_vita_common.cmake:17-23`, `cmake/aurora_vita_gxm.cmake:24-26`

```cmake
target_compile_options(... PRIVATE
    -O3 -ffunction-sections -fdata-sections -fno-exceptions -fno-rtti
    -mtune=cortex-a9 -mfpu=neon -fsigned-char)
if (AURORA_VITA_RENDERER STREQUAL "VITAGL")
    target_compile_options(aurora_vita_common PRIVATE -ffast-math)
endif ()
```

### 🔴 E1 — `-ffast-math` manca sul backend GXM

`-ffast-math` è abilitato solo per `VITAGL`. Su ARMv7 senza di esso **GCC non
auto-vettorizza nulla in floating point**, perché NEON non è IEEE-conforme sui denormali.
L'intera sezione B gira quindi in VFP scalare sul backend GXM.

Se `-ffast-math` completo preoccupa — cambia la gestione di NaN/inf, e
`vita_draw_adapter.cpp` usa `std::isfinite()` per la validazione — **usare il sottoinsieme
sicuro**:

```cmake
-fno-math-errno -funsafe-math-optimizations -fno-signed-zeros -ffp-contract=fast
```

Abilita la vettorizzazione NEON **senza** `-ffinite-math-only`, quindi `isfinite()`
continua a funzionare correttamente. È la combinazione giusta per questo codice.

### 🟡 E2 — `-mtune` invece di `-mcpu`

`-mtune=cortex-a9` influenza **solo lo scheduling**; non abilita le istruzioni dell'A9.
Serve:

```cmake
-mcpu=cortex-a9 -mfpu=neon-vfpv3 -mfloat-abi=hard
```

`-mfpu=neon` da solo implica `neon-vfpv3` sulla maggior parte delle toolchain, ma
esplicitarlo è più sicuro.

**`-marm` vs Thumb-2:** merita un test A/B. Thumb-2 è più denso (meglio per la I-cache da
32 KB), ARM ha scheduling migliore sui loop numerici. Il verdetto dipende dal codice:
misurare entrambi.

### 🟡 E3 — Nessun LTO

Aggiungere `-flto=auto -ffat-lto-objects` (o `INTERPROCEDURAL_OPTIMIZATION ON`) sui target
`aurora_vita_common` e `aurora_vita_gxm_backend`. Molte funzioni calde sono piccole e
distribuite su `.cpp` diversi — `transform_vertex()` ↔ `decode_vertex_into()` ↔
`pack_gpu_vertex()` — e l'inlining cross-TU qui è concreto.

Attenzione ai tempi di link con `-Wl,-q`.

### ⚪ E4 — Ottimizzazione mirata invece di `-O3` globale

`-O3` globale fa crescere il codice e peggiora l'I-cache da 32 KB. Un'alternativa: `-Os`
globale più `__attribute__((optimize("O3","unroll-loops")))` sui 5-6 loop per-vertice e
per-texel realmente caldi.

Vale la pena solo dopo aver misurato la miss rate della I-cache — non speculativamente.

---

## F. Parallelismo CPU

**Dove:** `platforms/vita/gfx/vita_cpu_workers.cpp`

Configurazione attuale: 2 worker più il chiamante, affinità sui core 1 e 2,
`minItems = 512`, priorità `0x10000110`.

### 🟡 F1 — Sincronizzazione a semafori per dispatch

```cpp
lane.state.store(1, std::memory_order_release);
dispatched[i] = sceKernelSignalSema(lane.wake, 1) >= 0;
// ...
while(sceKernelWaitSema(lane.done, 1, nullptr) < 0) sceKernelDelayThread(100);
```

`sceKernelSignalSema` / `WaitSema` sono syscall: ~5-20 µs a coppia. Con `minItems = 512` e
draw da 500-2000 vertici si fanno ~2 syscall per draw; su centinaia di draw sono
millisecondi persi in kernel. Il commento a `vita_cpu_workers.cpp:133-141` riconosce già
la classe di problema e ha mitigato il caso peggiore, ma il costo base resta.

**Alternative:**
- **Work queue persistente:** il main thread accoda tutte le draw del frame, i worker fanno
  spin-then-sleep su un atomico. Un solo wake per frame invece di due per draw.
- **Parallelismo a granularità di draw** invece che di vertice: mandare la draw N+1 al
  worker mentre il main prepara la N. Elimina la barriera per-draw.

Abbassato il costo di sincronizzazione, si può abbassare anche `minItems` e parallelizzare
draw più piccole.

### ⚪ F2 — Note sul dimensionamento

- **Core:** sulla Vita i core 0-2 sono per l'applicazione (il 3 è riservato al sistema).
  Usare 0 (chiamante) + 1 + 2 è corretto.
- **Priorità:** `0x10000110` è più bassa del default (`0x10000100`). Va bene per non
  affamare il thread di gioco, purché il render thread non finisca ad aspettare i worker.
- **Il vero limite è B1.** Ridotto il working set per vertice, il parallelismo rende molto
  di più. Lavorare su F1 prima di B1 dà meno di quanto sembri.

---

## G. Piano operativo

### Fase 1 — basso rischio, guadagno immediato

Nessun cambio di semantica GX. Ordine consigliato:

| # | Item | File principale |
|---|---|---|
| 1 | [E1](#-e1---ffast-math-manca-sul-backend-gxm) + [E2](#-e2---mtune-invece-di--mcpu) — flag matematici e `-mcpu` | `cmake/aurora_vita_common.cmake` |
| 2 | [A1](#-a1--lo-scissor-via-discard-disattiva-lhsr) — region clip hardware | `gxm_shader_gen.cpp`, `gxm_renderer.cpp` |
| 3 | [A2.1](#-a2--depth-force-store-sempre-attivo-formato-df32) — niente force-store sull'ultima scena | `gxm_renderer.cpp:295-298` |
| 4 | [D1](#-d1--upload-uniform-incondizionato-a-ogni-draw) — dirty-tracking uniform | `gxm_renderer.cpp:694-788` |
| 5 | [D4](#-d4--fixedvertexuniforms_--26-kb-per-draw-e-un-puntatore-a-rischio) — puntatore dangling (**correttezza**) | `aurora_vita_draw_sink.cpp:705` |
| 6 | [D2](#-d2--lookup-ripetuti-su-unordered_map-nel-path-caldo) — lookup ridondanti | `gxm_renderer.cpp:791-856` |

Il punto 1 va fatto per primo perché amplifica tutto il resto. Il punto 5 è una correzione
di correttezza, non di performance, e va fatto comunque.

### Fase 2 — richiede validazione visiva

| # | Item |
|---|---|
| 7 | [A4](#-a4--risoluzione-interna-a-960544) — risoluzione interna disaccoppiata |
| 8 | [A5](#-a5--tutte-le-texture-espanse-a-rgba8888) — formati texture nativi, partendo da CMPR→UBC1 |
| 9 | [A6](#-a6--emulazione-software-del-wrap-uv) — swizzle delle catene mip |
| 10 | [C1](#-c1--doppia-copia-dello-streaming-arena-su-gxm) — arena in memoria cached con writeback |
| 11 | [C2](#-c2--fixup-cpu-su-memoria-uncached-dopo-lefb-copy) — eliminare i fixup CPU su uncached |

### Fase 3 — strutturale

| # | Item |
|---|---|
| 12 | [B1](#-b1--canonicalvertex-è-da-168-byte) — `prepare_streamed_draw_into` come default |
| 13 | [B2](#-b2--il-decoder-è-un-interprete-per-attributo-e-per-vertice) + [B3](#-b3--nessuna-vettorizzazione-neon-nel-transform) — decoder specializzato e transform NEON |
| 14 | [A2.3](#-a2--depth-force-store-sempre-attivo-formato-df32) + [A3](#-a3--numero-di-scene-per-frame) — D16 e riduzione delle scene |
| 15 | [F1](#-f1--sincronizzazione-a-semafori-per-dispatch) — work queue persistente |
| 16 | [B4](#-b4--il-path-gpu-vertex-è-disattivato-di-default) — riabilitare il path GPU-vertex con il protocollo di `PERFORMANCE_LAB.md` |

---

## H. Protocollo di misura

Senza questi strumenti l'audit resta speculativo. Tre aggiunte che oggi mancano.

### H1 — Il test decisivo: CPU-bound o GPU-bound?

Abbassare `config.width/height` a 480×272 **senza toccare nient'altro** e confrontare il
frame time sulla stessa scena deterministica.

| Risultato | Conclusione | Priorità |
|---|---|---|
| Frame time crolla | GPU / fill-bound | Sezione A |
| Frame time invariato | CPU-bound | Sezioni B e D |
| Riduzione parziale | Misto | A1 + D1 per primi |

Costa mezz'ora e determina quale metà di questo documento conta davvero.

### H2 — Contatore di scene per frame

`stats.sceneCount` incrementato in `ensure_scene()` (`gxm_renderer.cpp:275`).
Cinque righe, e dice se [A3](#-a3--numero-di-scene-per-frame) è un problema reale o no.

### H3 — Flag per quantificare A1

Un `AURORA_VITA_NO_DISCARD=1` che forza `fragmentScissor = false` globalmente, anche se
visivamente scorretto, serve solo a misurare in un run quanto pesa il punch-through.
Da non lasciare nel codice di produzione.

### H4 — Attenzione a cosa misurano le statistiche attuali

`stats.nativeDrawUs`, `nativePipelineUs`, `nativeTextureUs` (`gxm_renderer.cpp:835-853`)
misurano il tempo **CPU di sottomissione**, non l'esecuzione GPU. `submit_us` in
`PERFORMANCE_LAB.md` ha lo stesso caveat, già documentato.

Restano valide le regole di acceptance già scritte in `PERFORMANCE_LAB.md`: stesso binario
compilato per i confronti on/off, configurazione registrata, verifica dell'eboot installato
con hash di readback, e controllo sia dei menu sia del gameplay, non solo della scena
introduttiva.

---

## Indice rapido per file

| File | Item |
|---|---|
| `cmake/aurora_vita_common.cmake` | [E1](#-e1---ffast-math-manca-sul-backend-gxm), [E2](#-e2---mtune-invece-di--mcpu), [E3](#-e3--nessun-lto), [E4](#-e4--ottimizzazione-mirata-invece-di--o3-globale) |
| `cmake/aurora_vita_gxm.cmake` | [E1](#-e1---ffast-math-manca-sul-backend-gxm), [C1](#-c1--doppia-copia-dello-streaming-arena-su-gxm) |
| `platforms/vita/aurora_vita_backend.hpp` | [A4](#-a4--risoluzione-interna-a-960544), [B4](#-b4--il-path-gpu-vertex-è-disattivato-di-default) |
| `platforms/vita/gxm/gxm_shader_gen.cpp` | [A1](#-a1--lo-scissor-via-discard-disattiva-lhsr), [A6](#-a6--emulazione-software-del-wrap-uv) |
| `platforms/vita/gxm/gxm_renderer.cpp` | [A1](#-a1--lo-scissor-via-discard-disattiva-lhsr), [A2](#-a2--depth-force-store-sempre-attivo-formato-df32), [A3](#-a3--numero-di-scene-per-frame), [A5](#-a5--tutte-le-texture-espanse-a-rgba8888), [C2](#-c2--fixup-cpu-su-memoria-uncached-dopo-lefb-copy), [D1](#-d1--upload-uniform-incondizionato-a-ogni-draw), [D2](#-d2--lookup-ripetuti-su-unordered_map-nel-path-caldo) |
| `platforms/vita/gxm/gxm_memory.cpp` | [C1](#-c1--doppia-copia-dello-streaming-arena-su-gxm), [C3](#-c3--partizionamento-cdram-vs-ram-di-sistema) |
| `platforms/vita/gxm/gxm_texture_layout.cpp` | [A6](#-a6--emulazione-software-del-wrap-uv) |
| `platforms/vita/gfx/vita_texture_cache.cpp` | [A5](#-a5--tutte-le-texture-espanse-a-rgba8888) |
| `platforms/vita/gfx/vita_texture_decode.cpp` | [A5](#-a5--tutte-le-texture-espanse-a-rgba8888) |
| `platforms/vita/gfx/vita_vertex_decode.hpp/.cpp` | [B1](#-b1--canonicalvertex-è-da-168-byte), [B2](#-b2--il-decoder-è-un-interprete-per-attributo-e-per-vertice) |
| `platforms/vita/gfx/vita_vertex_pipeline.cpp` | [B3](#-b3--nessuna-vettorizzazione-neon-nel-transform) |
| `platforms/vita/gfx/vita_draw_adapter.cpp` | [B1](#-b1--canonicalvertex-è-da-168-byte), [D3](#-d3--drawpacket-da-950-byte-copiata-per-draw) |
| `platforms/vita/gfx/vita_streaming_arena.cpp` | [C1](#-c1--doppia-copia-dello-streaming-arena-su-gxm) |
| `platforms/vita/gfx/vita_cpu_workers.cpp` | [F1](#-f1--sincronizzazione-a-semafori-per-dispatch), [F2](#-f2--note-sul-dimensionamento) |
| `platforms/vita/gfx/vita_gfx_types.hpp` | [A1](#-a1--lo-scissor-via-discard-disattiva-lhsr), [D3](#-d3--drawpacket-da-950-byte-copiata-per-draw), [D4](#-d4--fixedvertexuniforms_--26-kb-per-draw-e-un-puntatore-a-rischio) |
| `platforms/vita/gfx/vita_static_geometry.hpp` | [B4](#-b4--il-path-gpu-vertex-è-disattivato-di-default) |
| `platforms/vita/gx/aurora_vita_draw_sink.cpp` | [A3](#-a3--numero-di-scene-per-frame), [D4](#-d4--fixedvertexuniforms_--26-kb-per-draw-e-un-puntatore-a-rischio) |
