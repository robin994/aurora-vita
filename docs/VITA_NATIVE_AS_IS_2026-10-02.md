# Aurora Vita: analisi AS-IS e piano esecutivo Vita native

Data: **2026-10-02**. Revisione analizzata: **`bb5147e1e1aebc959a56dd85dc4010c7c9d17afd`**, branch `vita-experiment`.
Implementazione successiva: [stato AVN-00–11 e verifiche](VITA_NATIVE_IMPLEMENTATION_2026-10-02.md).
Le osservazioni AS-IS sotto restano riferite alla revisione iniziale.

Il checkout era pulito all'inizio dell'audit. Questo documento propone attività: **non certifica l'implementazione delle modifiche proposte**.

## 1. Come assegnare il lavoro senza ricostruire tutta la conversazione

Assegnare **una scheda AVN alla volta**. Il modello legge [AGENTS.md](../AGENTS.md), le sezioni 2, 6 e 7 di questo report, la scheda assegnata e soltanto i file indicati. Gli ID Sxx si risolvono nella mappa della sezione 3; leggere solo le righe pertinenti. Le sezioni 3–5 sono approfondimenti da consultare per il finding citato nella scheda. Le sezioni 9–10 contengono verifiche e prove.

Le dipendenze sono esplicite. Una scheda conclusa produce un riepilogo con revisione iniziale, file modificati, test realmente eseguiti, risultato e verifiche hardware ancora necessarie. Il successore riceve quel riepilogo e il diff pertinente, senza l'intera cronologia della chat.

**Ordine iniziale raccomandato:** AVN-00 → AVN-01; poi AVN-02 → AVN-03, AVN-04 e AVN-05, in modifiche separate. AVN-06/07 vengono dopo questi interventi. AVN-09/10 sono analisi subordinate alle misure, non autorizzazioni a riscrivere batching o sincronizzazione.

| Scheda | Consegna | Dipendenza principale | Classe modello |
| --- | --- | --- | --- |
| AVN-00 | Preset stabile/controllo e documentazione | Nessuna | L |
| AVN-01 | Manifest dei binari e delle catture | AVN-00 per i nuovi preset | L/S |
| AVN-02 | Selettore puro delle lane e test | Nessuna | S + review A |
| AVN-03 | Integrazione del dispatch reale | AVN-02 | A |
| AVN-04 | Statistiche fuori dal singolo draw | Contratto lettori; AVN-01 per misura | S + review A |
| AVN-05 | Scratch per snapshot fixed duplicati | Nessuna | S |
| AVN-06 | Contratto/adapter della vista immediata | AVN-04 | A design, S test |
| AVN-07 | Vista collegata al percorso GXM | AVN-06 | A |
| AVN-08 | Budget memoria pubblicato per frame | AVN-04 se cambia gli snapshot | S + review A |
| AVN-09 | Decisione sul batching, con misure | AVN-01 e cattura | S |
| AVN-10 | Protocollo di retirement, con prove | AVN-01 e finish significativi | A |
| AVN-11 | Contratto dei servizi Vita | Nessuna | S + review A |

## 2. Baseline e limiti dell'evidenza

- Il proprietario del progetto dichiara test su Vita senza regressioni per `AURORA_VITA_ASYNC_GX`, `AURORA_VITA_GXM_DIRECT_STREAM_WRITE` e `AURORA_VITA_GXM_DIRECT_DRAW_SUBMIT`. **Le tre opzioni sono considerate stabili per la configurazione verificata dall'utente** e costituiscono la baseline raccomandata del piano.
- Titolo, hash installato, scena, clock e misure prima/dopo di quei test non sono allegati. Non attribuire loro FPS, percentuali di miglioramento o copertura di altri giochi.
- Nel sorgente le tre opzioni sono ancora `OFF` per default. Il preset `vita-gxm` non le attiva. La directory locale `build/vita-gxm-fast` le ha tutte `ON`; **non esiste ancora un preset chiamato `vita-gxm-fast` o `vita-gxm-stable`**.
- Il renderer generico predefinito è VitaGL; i preset scelgono il backend esplicitamente. GXM è già nativo e selezionato alla compilazione, senza Dawn/vitaGL/vita2d nel binario verificato.
- Audit corrente: configurazione/build host riuscite, **11/11 CTest passati**, build dei due profili GXM riuscite, **4/4 audit ELF/map passati**, quattro eboot nei VPK identici ai SELF corrispondenti. Dettagli nella sezione 10.
- Nessun nuovo benchmark o confronto visivo su console è stato acquisito durante questo audit. I colli di bottiglia in millisecondi restano da attribuire nel port consumatore.

Legenda: **S** = verificato nel sorgente; **B** = verificato nel codice ARM generato/build; **H** = test host; **U** = evidenza dichiarata dall'utente; **M** = effetto prestazionale da misurare. Un costo S/B non è automaticamente il maggiore costo del gioco.

## 3. Architettura effettiva

```text
Callback e ciclo del gioco
  → entry point Dolphin GX/VI
  → FIFO produttore, registri e display list
  → consumer GX [thread dedicato se ASYNC_GX]
  → command_processor → DrawSink::submit
      → revisioni GX, pipeline/layout, texture, uniform
      → geometria statica GPU oppure decode/transform/pack degli stream
      → GXM direct: flush dei comandi precedenti → stream flush → draw immediato
      → altri casi: CommandStream → execute/execute_range → draw
  → gfx::Renderer [implementazione GXM selezionata al link]
  → gxm::Renderer → sceGxm* → present / EFB / readback
```

| Confine | Funzione attuale e scelta progettuale |
| --- | --- |
| GX/FIFO → command processor | Interpreta il contratto del gioco e conserva ordine, token e dipendenze. Non eliminabile solo perché aggiunge chiamate. |
| DrawSink → preparazione | Traduce lo stato e sceglie il percorso GPU/CPU; usa già revisioni per dominio e ricette di draw. |
| Stream → draw immediato | La flag direct salta già CommandStream per gli stream eleggibili. Geometria statica, fallback e batching continuano a usare la coda. |
| Facade `gfx::Renderer` → native | Chiamate C++ concrete, non una catena di dispatch virtuale. I costi residui interessanti sono i dati copiati e la gestione delle risorse. |
| Scene e risorse GXM | Un proprietario del contesto; helper solo per preparazione CPU. BeginScene ristabilisce sempre lo stato. |
| Backend → servizi Vita | Lo standalone include un runtime minimo di configurazione/log. SDL3/core/PAD/CARD seguono integrazioni distinte; il probe SDL3 esistente è VitaGL. |

### Mappa del contesto minimo

I numeri di riga valgono per la revisione analizzata; verificare prima il simbolo se il file è cambiato.

| ID | File e simboli da trovare |
| --- | --- |
| S01 | [FIFO](../lib/gx/fifo.cpp): `enqueue_job` (~150), `start_worker` (~210), `run_async` (~277), `write_stable_data_from` (~348) |
| S02 | [Command processor](../lib/gx/command_processor.cpp): `process`, `DrawSink::submit` (~2024, ~2047) |
| S03 | [DrawSink](../platforms/vita/gx/aurora_vita_draw_sink.cpp): `flush` (~172), `submit` (~477), `buildFixedUniforms` (~1023), direct path (~1065) |
| S04 | [DrawSink header](../platforms/vita/gx/aurora_vita_draw_sink.hpp): `fixedVertexUniforms_`, `reset_commands`, cache di stato |
| S05 | [Facade GXM](../platforms/vita/gxm/gxm_facade.cpp): `BufferPool`, `TextureCache`, `Renderer::draw` (~581), `execute_range` |
| S06 | [Renderer GXM](../platforms/vita/gxm/gxm_renderer.cpp): `ensure_scene` (~495), `bind_texture` (~1119), `bind_pipeline` (~1174), `draw` (~1359), `finish` (~1548) |
| S07 | [Worker CPU](../platforms/vita/gfx/vita_cpu_workers.cpp): `WorkerAffinities`, `cpu_parallel_for_min_lanes`; [contratto](../platforms/vita/gfx/vita_cpu_workers.hpp) |
| S08 | [Streaming arena](../platforms/vita/gfx/vita_streaming_arena.cpp): `acquire_slot`, `begin_frame`, `reserve`, `flush`, `mark_current_submitted` |
| S09 | [Tipi](../platforms/vita/gfx/vita_gfx_types.hpp): `DrawPacket`, `GpuDrawUniforms`, `FixedVertexUniforms`, `FrameStats`; [snapshot fixed](../platforms/vita/gfx/vita_fixed_vertex.hpp): `fixed_vertex_uniforms_into` |
| S10 | [CommandStream](../platforms/vita/gfx/vita_command_stream.hpp): `share_draw_state`, copia/move, `reset`; [batch predicate](../platforms/vita/gfx/vita_draw_batch.hpp) |
| S11 | [Backend](../platforms/vita/aurora_vita_backend.cpp): `end_frame_now`, `completed_performance_snapshot`, `performance_snapshot`, `memory_budget`; [API](../platforms/vita/aurora_vita_backend.hpp) |
| S12 | [Build GXM](../cmake/aurora_vita_gxm.cmake), [preset](../CMakePresets.json), [frontend comune](../cmake/aurora_vita_frontend.cmake), [test](../cmake/aurora_vita.cmake) |
| S13 | [SDL3 provider](../cmake/AuroraSDL3Provider.cmake), [runtime minimo](../lib/vita/runtime.cpp), [window](../lib/window.cpp), [PAD](../cmake/aurora_pad.cmake), [CARD I/O](../lib/card/FileIO.cpp) |
| S14 | [Traduzione GX](../platforms/vita/gx/aurora_gx_bridge.cpp): `translate_current_pipeline_and_layout`; [revisioni](../lib/gx/state_revisions.hpp) |

### Funzioni già ottimizzate: non riproporle da zero

- Revisioni vertex/fragment/texture/raster, cache del layout e ricette immutabili.
- Lookup delle capacità uniform risolto alla creazione della pipeline.
- Confronto programmi/depth/cull e riuso uniform **entro la scena**.
- Fast path delle texture già bindate, che evita il lookup della mappa quando valido.
- `resolvedPipelineHint` evita una seconda ricerca della stessa pipeline dentro `bind_pipeline`.
- Scritture dirette degli stream e puntatori alle pagine risolti per slot.
- CommandStream conserva il proprio high-water storage; gli snapshot condivisi restano immutabili.
- Copie EFB native, policy depth e shader senza fragment-scissor per rettangoli realmente a target pieno.
- Cache GXP persistente per titolo, manifest delle pipeline e prewarm.
- Probe CPU3, quattro lane e cap separati per renderer/gioco; nessun budget adattivo CPU3 ancora implementato.
- Snapshot completato senza drenare GX, contatori finish per causa, comparatore FRAME e CI host con sanitizer.

## 4. Finding: costi, complessità e priorità

### F01 — Il consumer GX e il primo helper condividono CPU2 [S, M; priorità P1]

`start_worker` usa `SCE_KERNEL_CPU_MASK_USER_2`. Il primo elemento di `WorkerAffinities` usa la stessa maschera. La scelta delle lane considera quantità di lavoro e cap, ma non elimina gli helper sul core del chiamante. Durante una preparazione avviata dal consumer GX, caller e primo helper non possono lavorare contemporaneamente su due core distinti.

Obiettivo: scegliere helper su core fisici diversi dal chiamante. Non spostare indiscriminatamente il consumer su CPU1: va considerata l'integrazione audio del port. La stringa CMake che descrive async GX su “core 1” è obsoleta.

Vincolo essenziale: un cap di 3 oggi lascia CPU3 fuori dal dispatch. Saltare l'helper CPU2 **non autorizza a sostituirlo con CPU3** oltre il prefisso di helper consentito. Separare rango di partizione del lavoro, indice fisico dell'helper e ID lane passato al callback. Schede AVN-02/03.

### F02 — Copie di statistiche dopo ogni draw nativo [S, B, M; P1]

`gfx::Renderer::draw` chiama il renderer nativo e ricopia **192 byte di campi** nelle proprie statistiche a ogni draw riuscito: includono due array da 64 byte di contatori finish cumulativi. Queste copie avvengono anche con diagnostica DrawSink disabilitata. Il disassemblato ARM del GX probe fast conferma istruzioni di load/store e copie `ldmia/stmia` dopo la chiamata al draw nativo: LTO non le ha eliminate.

`present` trasferisce nuovamente le statistiche native. Inoltre, con telemetria attiva, DrawSink acquisisce copie di `FrameStats` prima/dopo texture e pipeline, pur consumando solo pochi delta. Qui il compilatore può restringere le copie: non confondere il `sizeof` con traffico fisico misurato.

Obiettivo: comporre le statistiche native alla lettura/boundary appropriata e usare delta piccoli per la telemetria. Preservare letture intermedie, conteggi di clear/copy/present, contatori cumulativi e contratto di `stats()`. Non limitarsi a cancellare assegnazioni. Scheda AVN-04.

### F03 — Stato copiato dentro il DrawPacket immediato [S, B per dimensioni, M; P2]

Con direct submit, DrawSink costruisce comunque `DrawPacket packet{}` e assegna `packet.textures=bindings` e `packet.uniforms=uniforms`. Le dimensioni ARM rilevate sono **352 + 620 = 972 byte di payload logico**, oltre ai metadati. Il packet completo occupa 1.080 byte. La flag ha già eliminato la permanenza nella coda; resta da evitare la materializzazione del payload per una chiamata sincrona.

Direzione: una vista immutabile per il solo consumo immediato. I packet accodati mantengono proprietà e durata del proprio stato. Il native renderer deve consumare i riferimenti prima del ritorno; la memoria degli stream GPU continua invece a rispettare il retirement. Mai usare un cast fra `DrawUniforms` e `GpuDrawUniforms` per risparmiare la conversione: il layout a prefisso non concede automaticamente aliasing C++.

Il numero di byte sopra descrive gli oggetti/assegnazioni, non un benchmark di banda. Richiede misura separata dopo F02. Schede AVN-06/07.

### F04 — Snapshot fixed: costruzione prima della verifica di uguaglianza [S, M; P1]

`buildFixedUniforms` esegue `deque.emplace_back`, costruisce `FixedVertexUniforms` e confronta con il precedente; se uguale esegue `pop_back`. Il payload occupa **3.096 byte ARM**. La costruzione include inizializzazione e copie di matrici/luci; l'emplace/pop può inoltre causare allocazioni secondo la politica della deque. Non è stato acquisito un conteggio di allocazioni Vita per questo percorso.

Primo intervento limitato: costruire in uno scratch riutilizzabile, mantenere il confronto byte-exact, inserire nella deque soltanto quando serve un nuovo snapshot. Riduce l'attività del contenitore sui duplicati; non pretende di eliminare il costo della costruzione.

Il commento del codice documenta una regressione precedente con riuso basato solo su versioni/chiavi. **Non sostituire il confronto esatto con `vertexStateVersion_` o pipeline key.** Un successivo pool persistente per gli snapshot unici richiederebbe un compito distinto, limiti di memoria e prova della durata dei puntatori. Scheda AVN-05.

### F05 — Lookup residui e controlli ripetuti [S, M; P3 condizionale]

Il draw nativo ricerca una pipeline e due buffer nelle mappe. Sono presenti ulteriori controlli di scena/validità, ma `ensure_scene` termina rapidamente quando la scena è aperta e la seconda ricerca della pipeline è già evitata. Il fast path texture è già presente.

Possibile studio successivo: risoluzione delle risorse una volta per run, con handle/generazioni e invalidazione alla distruzione. È un cambiamento di lifetime; non introdurre puntatori permanenti senza provare eviction, riuso degli handle, reset e alternanza con clear/blit. Va affrontato solo se un profilo attribuisce un costo rilevante a queste ricerche.

### F06 — Batching e direct submit sono percorsi alternativi [S, M; P2 condizionale]

Con `gxm_local_draw_batching=true`, il ramo diretto seleziona `queueStreamed`. Il batching richiede stato equivalente e slice contigue, e il padding dell'arena può impedire fusioni. Non aspettarsi automaticamente un guadagno attivandolo insieme alle tre flag stabili.

Prima misurare candidati, fusioni e rifiuti. Poi valutare un accumulo breve di soli draw adiacenti compatibili. Non cambiare globalmente l'allineamento per aumentare il merge rate; non riordinare draw per pipeline/texture. Scheda AVN-09.

### F07 — Retirement e mutazioni ricorrono ancora a finish globale [S, M; P2 condizionale, rischio alto]

`StreamingArena::acquire_slot` chiama `wait_idle` quando non trova pagine disponibili. Il backend nativo usa `finish` per mutazione/distruzione di risorse in-flight, readback e altre transizioni. Esiste già retirement delle pagine legato al frame effettivamente inviato e alla coda di presentazione; non partire dall'ipotesi che tutto venga sincronizzato a ogni draw.

Le notifiche vertex/fragment di `sceGxmEndScene` sono oggi passate come null. Una futura sequenza di completamento potrebbe ridurre alcune attese e posticipare le distruzioni. Vanno però coperti trasferimenti, più scene/target, overflow nello stesso frame e present scartati. Aumentare semplicemente il numero di frame presunti sicuri non è una soluzione.

Prima calcolare delta di `nativeFinishReasonCalls/WaitUs` e attribuire il costo reale. Le API di notifiche/transfer esistono in [VitaSDK GXM](https://github.com/vitasdk/vita-headers/blob/master/include/psp2/gxm.h); la strategia di ownership è da progettare e validare nel progetto. Scheda AVN-10.

### F08 — Osservazione pubblica non uniforme fra thread [S, M; P1 di integrazione]

`completed_performance_snapshot()` legge un valore pubblicato senza drenare GX. `performance_snapshot()` usa invece `run_sync` quando async GX è attivo. `memory_budget()` legge direttamente DrawSink e cache; non applica lo stesso protocollo di pubblicazione.

Un port che legge statistiche sincrone ogni frame può reintrodurre attese. Una lettura delle cache dal game thread mentre il worker le modifica richiede un contratto esplicito; il sorgente non fornisce sincronizzazione per `memory_budget()`. È una condizione di rischio, non una race riprodotta in questo audit.

Definire le API osservabili dal game thread e pubblicare anche il budget memoria coerente a fine frame. Mantenere gli accessori a renderer/DrawSink come strumenti del thread proprietario, documentandolo. Il mutex breve di `PublishedSnapshot` è già il meccanismo adatto; evitare seqlock su campi ordinari. Scheda AVN-08.

### F09 — Copia FIFO: costo visibile, ownership necessaria [S, M; P3]

`enqueue_job` copia i byte FIFO in uno slot posseduto dal consumer e copia il contesto dei callback asincroni. La coda è limitata a quattro job; le display list vengono aggregate fino a 256 KiB o al limite degli stable span. Non esiste più necessariamente un job per ogni display list.

Una futura rotazione di buffer produttore/consumer può rimuovere una copia, ma deve mantenere ownership dei byte, identità degli stable span, endianness, aggiornamenti delle sorgenti indicizzate, backpressure e shutdown. Non trasformare `assign` in un puntatore a memoria riutilizzata dal gioco. Affrontare solo se `dlCopyUs`, volume copiato e attese del produttore lo giustificano.

### F10 — Wrapper vuoti e ottimizzazioni apparenti [S, B; bassa priorità]

Le tre invalidazioni resource/texture/buffer della facade GXM sono vuote. Nel GX probe fast con LTO la funzione `invalidate_resource_bindings` non compare tra i simboli, mentre i due livelli di `Renderer::draw` sono presenti. Le copie F02 sono visibili nel disassemblato.

Quindi: pulire commenti e chiamate GL-specifiche può migliorare leggibilità; togliere una funzione già eliminata dal compilatore non dimostra un guadagno runtime. Lo stesso vale per getter inline, condizioni constexpr di selezione backend e wrapper senza dati copiati. Non sostituire ogni facade con un backend duplicato.

## 5. Cosa significa Vita native per questo progetto

Il risultato desiderato è una libreria Vita con un solo proprietario GXM, dati di draw pronti per GXM, code e memoria limitate, confini di sincronizzazione espliciti e integrazione ripetibile nei giochi. La semantica GX resta il contratto di ingresso.

| Area | AS-IS | Passo successivo |
| --- | --- | --- |
| Build | Backend nativo già separato; LTO Vita già ON per default; frontend comune | Profilo stabile e controllo espliciti; audit anche nei port consumatori |
| Submission | Direct per stream, queue per gli altri casi, due oggetti renderer con statistiche replicate | F02/F03, mantenendo un solo percorso nativo di validazione e draw |
| CPU | Helper persistenti, soglie per lavoro, CPU3 verificato all'avvio | F01; in seguito budget CPU3 misurato, senza assumere che il quarto core sia sempre disponibile |
| Memoria | Pool nativo e budget/cache già presenti, fallback di allocazione, ring stream | Budget coordinato col gioco e dati coerenti; niente nuove cache prive di invalidazione |
| Shader | Cache persistente e prewarm | Manifest aderenti alle fasi del gioco; evitare compilazioni durante gameplay quando la copertura è nota |
| EFB/texture | Percorsi nativi più fallback CPU, packed GXM ancora opzionale | Ampliare solo i casi frequenti misurati; preservare alpha, flip, TLUT e feedback |
| Piattaforma | Runtime standalone minimo; SDL3/core/PAD/CARD distinti; probe SDL3 solo VitaGL | Prima specificare un target opzionale di servizi Vita e un probe GXM + servizi, poi lifecycle/input/audio/storage per schede separate |
| Qualità | CI host normale e sanitizer | Estendere a build VitaSDK, ABI SDK/GAMECUBE e audit ELF; regressioni visive hardware con artefatti identificati |

Il modulo servizi dovrebbe coordinare sospensione/ripresa, input, audio e percorsi dati lasciando al gioco i suoi callback e la simulazione. SDL3 può fornire servizi di piattaforma mantenendo disattivato il proprio renderer. L'integrazione va provata con GXM: non riutilizzare il probe VitaGL che il target GXM rifiuta. Il driver SDL supporta touch anteriore/posteriore; gestire gli eventi touch esplicitamente evita la conversione predefinita in eventi mouse. [SDL3 Vita](https://wiki.libsdl.org/SDL3/README-vita).

Questa espansione di prodotto è una fase distinta dall'ottimizzazione del renderer: AVN-11 produce un contratto circoscritto, non introduce contemporaneamente audio, PAD, CARD e lifecycle.

## 6. Contratto comune per ogni implementazione

1. Leggere `AGENTS.md`; verificare HEAD e modifiche locali. Non resettare il checkout né sovrascrivere lavoro preesistente. Se HEAD è diverso, rivalidare i simboli della scheda, non tutto il repository.
2. Conservare callback, FIFO, clear/copy/target/barrier, semantica EFB e CPU fallback. Un draw GPU fallito deve conservare il ripristino completo dello stato necessario al fallback CPU.
3. **Ogni BeginScene ristabilisce i binding.** Nessuna cache del contesto sopravvive implicitamente alla scena. Nessuna sostituzione dello scissor pixel-exact con il solo region clip a tile.
4. Conservare reserve/rebind degli uniform quando cambiano programmi o scena. Gli snapshot accodati restano immutabili fino al ritorno di execute; la memoria letta dalla GPU resta valida fino al completamento richiesto.
5. Aggiornamenti guest, TLUT, EFB, invalidazioni di source range, reset e cambi runtime invalidano le cache pertinenti. Il percorso conservativo per scritture legacy non tracciate resta attivo.
6. Le tre flag dichiarate stabili sono il riferimento. Le nuove ottimizzazioni con rischio su scheduling, stato o lifetime mantengono un controllo selezionabile e vengono promosse dopo confronto hardware.
7. Non introdurre un frontend GX alternativo, API OpenAI nel runtime, dipendenze desktop nel target standalone o allocazioni cached GPU senza un protocollo di visibilità verificato.
8. Separare tre risultati: correttezza host, build/link Vita, prestazioni e immagini su dispositivo. Un test host non certifica GXM, priorità reali o visibilità cache.
9. Non sommare fasi annidate; tempi di submit CPU non sono tempi GPU. I finish diagnostici alterano l'overlap. Percentili di campioni radi non sono una misura del pacing di tutti i frame.
10. Se emerge una modifica a ownership/ABI/FIFO fuori dal contratto della scheda, lasciare un diff coerente e un handoff con il punto non risolto; non aggiungere una seconda riscrittura per far passare i test.

## 7. Strategia di assegnazione e risparmio del contesto

Le indicazioni OpenAI raccomandano obiettivo, contesto, vincoli e criteri di completamento espliciti. Qui vengono concretizzati in schede e verifiche del repository. La documentazione corrente indica GPT-6.1 Sol come punto di partenza quando disponibile e High per GPT-6 Luna; la regola locale richiede **GPT-6 Astra High per modifiche trasversali al renderer**. [OpenAI: best practices](https://learn.chatgpt.com/guides/best-practices).

| Classe | Assegnazione suggerita | Tipo di lavoro |
| --- | --- | --- |
| L | GPT-6 Luna High, se disponibile | Documentazione, preset già specificati, manifest, report di misure; niente decisioni di lifetime |
| S | GPT-6.1 Sol Medium/High | Compito locale con interfaccia definita, algoritmo puro e test, piccole modifiche sotto contratto |
| A | GPT-6 Astra High | Design/review di scheduling, ownership, viste immediate, retirement e passaggi fra scene; implementazione trasversale |

La classificazione è una scelta operativa per contenere esplorazione e rework, non una misura di prezzo o consumo garantito. La revisione costosa si concentra sui contratti A; test puri e adattamenti locali possono poi essere estratti in schede S. Non assegnare a L il renderer completo chiedendo genericamente di ottimizzarlo.

Regole per ridurre i token senza perdere prove:

- All'inizio leggere la scheda e cercare i simboli, evitando il dump di tutti gli audit storici.
- Usare un solo obiettivo osservabile per modifica e un elenco di file consentiti. Un nuovo file richiede una motivazione nel handoff, non una scansione indiscriminata del progetto.
- Riutilizzare i test elencati; aggiungere casi che distinguono comportamento corretto e scorretto. Non creare test che ripetono semplicemente le assegnazioni della patch.
- Riportare il riepilogo dei comandi e gli errori rilevanti, senza incollare log completi. Non ripetere una suite passata se nessun cambiamento o dubbio nuovo lo richiede.
- Chiudere ogni scheda con massimo una pagina di handoff. Stato ammesso: `implementato/verificato localmente`, `validato su Vita`, `bloccato con evidenza`, `scartato per misura`; non confondere i primi due.

### Prompt riutilizzabile

```text
Implementa soltanto AVN-XX da docs/VITA_NATIVE_AS_IS_2026-10-02.md.
Leggi AGENTS.md, le sezioni 2, 6 e 7, la scheda e i relativi file.
Verifica HEAD e preserva le modifiche locali. Controlla il finding nel sorgente corrente.
Segui le dipendenze e il contratto della scheda. Non ampliare la modifica.
Esegui i test mirati, poi i controlli di integrazione applicabili della sezione 9.
Non dichiarare risultati hardware senza una cattura. Se manca una decisione su
ownership/ABI/FIFO, documenta il punto preciso e termina con un handoff coerente.
Consegna: obiettivo raggiunto, file, test e risultati, controllo di riferimento,
limiti residui e prossima scheda sbloccata. Commit/push seguono la richiesta del task.
```

Per estrarre il contesto iniziale di una scheda senza caricare tutto il report, dalla radice del repository:

```sh
python3 - AVN-05 <<'PY'
import re, sys
from pathlib import Path
text = Path('docs/VITA_NATIVE_AS_IS_2026-10-02.md').read_text()
task = re.search(r'(?ms)^### ' + re.escape(sys.argv[1]) + r' —.*?(?=^### AVN-|^## |\Z)', text)
if task is None:
    raise SystemExit('Scheda non trovata')
for section in (2, 6, 7):
    block = re.search(r'(?ms)^## ' + str(section) + r'\..*?(?=^## |\Z)', text)
    # Esclude prompt ed estrattore dalla sezione 7, evitando contesto ricorsivo.
    print(block.group().split('### Prompt riutilizzabile')[0])
ids = set(re.findall(r'\bS\d\d\b', task.group()))
for line in text.splitlines():
    if any(line.startswith('| ' + key + ' |') for key in ids):
        print(line)
print(task.group())
PY
```

Leggere poi il finding citato e la sezione 9 quando necessari. L'estratto non sostituisce `AGENTS.md` né il controllo del sorgente corrente.

## 8. Schede esecutive

Tutte le schede partono come **proposte, non implementate**. File “nuovo” significa da introdurre nell'attività futura. I test nominati sono target CTest esistenti salvo indicazione esplicita.

### AVN-00 — Formalizzare il profilo stabile [L, P0]

**Dipendenze:** nessuna. **Contesto:** S12, README, wiki Experimental-Flags/Configuration-Recipes.

**Risultato:** preset configure/build `vita-gxm-stable` con le tre flag ON e un preset di controllo con tutte OFF; directory distinte. Correggere il testo CMake “core 1”. Documentare separatamente stato di stabilità dichiarato dall'utente e default generici effettivi.

**File consentiti:** `CMakePresets.json`, `cmake/aurora_vita_gxm.cmake` (descrizioni), `README.md`, `docs/building.md`, le due pagine wiki citate. Non cambiare codice runtime, altri default o le flag texture.

**Verifica:** `cmake --list-presets`, configurare entrambi i nuovi preset, leggere le tre variabili dalle cache, build GXM e audit se si producono artefatti. **Completato quando:** una configurazione pulita seleziona esattamente le tre opzioni dichiarate. Nessuna promessa di FPS nella documentazione.

### AVN-01 — Manifest riproducibile e raccolta della baseline [L/S, P0]

**Dipendenze:** AVN-00 per i nuovi nomi; i profili attuali possono già essere acquisiti. **Finding:** F01–F09 richiedono attribuzione.

**Risultato:** un piccolo strumento offline, ad esempio `tools/vita_build_manifest.py` (nuovo), che legge commit/diff, cache CMake effettiva, toolchain e hash ELF/SELF/eboot. Titolo, scena, clock, risoluzione, cache calda/fredda e hash installato sono campi forniti dalla cattura: se assenti risultano `unknown`, mai dedotti dal VPK.

**File consentiti:** nuovo strumento, relativo test Python e `docs/wiki/Diagnostics-and-Validation.md`. Riutilizzare il comparatore FRAME esistente senza cambiarne il significato.

**Verifica:** fixture con chiavi mancanti, tre flag OFF/ON, SELF diverso dall'eboot, percorso inesistente, dirty tree. **Completato quando:** manifest deterministico a parità di input, errori espliciti, una cattura può essere collegata al suo binario. Il pacchetto hardware resta incompleto finché non esistono log della stessa scena.

### AVN-02 — Specificare e testare la scelta delle lane [S; review A, P1]

**Dipendenze:** nessuna. **Finding:** F01. **File:** nuovo helper puro `platforms/vita/gfx/vita_cpu_dispatch_plan.hpp`, `tests/vita_cpu_workers_test.cpp`; lettura di S07 e degli array per lane in `vita_draw_adapter.cpp`/`vita_vertex_decode.cpp`.

**Contratto:** filtrare prima il prefisso di helper consentito dal cap, poi escludere i core uguali al caller o duplicati. Cap0 significa tutti gli helper configurati. Limitare inoltre le lane utili con la granularità `minItems` già prevista, dopo il filtro: non svegliare thread per intervalli vuoti. Il rango nel piano divide gli intervalli; il callback conserva l'ID `helperIndex+1`, caller sempre lane 0. Core del chiamante sconosciuto: fallback seriale conservativo. Nessuna modifica alle affinità reali in questa scheda.

**Casi minimi:** workers `[CPU2, CPU1, CPU3]`; caller CPU0/cap3 → helper 0,1; caller CPU2/cap3 → helper 1; caller CPU2/cap4 → helper 1,2. Includere zero worker, pochi elementi, CPU3 assente, core duplicati e cap0.

**Completato quando:** copertura esatta/disgiunta degli intervalli, ID lane entro 0..3 e CPU3 mai ammessa da cap3. Il contratto viene revisionato prima di AVN-03.

### AVN-03 — Applicare il piano ai worker reali [A, P1]

**Dipendenze:** AVN-02. **File:** S07, test worker, stub Vita usati da quel test; configurazione backend solo se necessaria al controllo selezionabile.

**Risultato:** dispatch e raccolta risultati attraversano gli helper scelti, non `0..activeWorkers-1`. Conservare una coppia wake/done per job, fallback per chiamate annidate/concorrenti, gestione degli errori e shutdown. Non aumentare priorità e non introdurre work stealing o budget CPU3 nello stesso intervento.

**Verifica:** test esistente degli 8.000 job, chiamante simulato su CPU2, helper non contigui, errori del callback su lane 2/3, CPU3 rifiutata, signal fallito e shutdown. Eseguire suite host, build GXM e confronto hardware contro il dispatch originale.

**Completato quando:** risultati equivalenti, nessuna lane fuori cap, log della topologia effettiva e misura delle attese. Senza prova hardware: implementato/verificato localmente, controllo originale disponibile.

### AVN-04 — Togliere le copie di statistiche dal singolo draw [S; review A sul contratto, P1]

**Dipendenze:** nessuna; AVN-01 per misurare. **File:** S05, `vita_renderer.hpp`, S03 per delta stretti; test/probe pertinenti.

**Risultato:** togliere la replica nativa F02 dal draw, componendo i campi quando letti o pubblicati. Conservare i contatori di pipeline/texture posseduti dalla facade. Definire e documentare la validità del riferimento restituito da `stats()`; una lettura a metà frame deve essere aggiornata. Nessun puntatore al backend letto dal game thread.

**Verifica:** conteggi dopo draw, clear/copy interni, execute parziale, errore e present; contatori finish cumulativi distinti dai contatori per frame. Controllare tutti i lettori di `stats()`; il probe GXM verifica i casi non eseguibili sul backend host. Ispezionare il disassemblato del draw Release/LTO.

**Completato quando:** stessi valori osservabili ai confini previsti e copie ripetute assenti nel codice generato. Non dichiarare speedup sulla base dei soli byte rimossi.

### AVN-05 — Evitare emplace/pop per snapshot fixed duplicati [S, P1]

**Dipendenze:** nessuna. **Finding:** F04. **File:** S03/S04 e un test mirato in `tests/vita_submission_test.cpp` o helper/test dedicato se serve eseguire il percorso.

**Risultato:** uno scratch riutilizzabile, costruzione con la stessa `fixed_vertex_uniforms_into`, identico confronto fino a `offsetof(FixedVertexUniforms,revision)`, append solo per snapshot nuovo. Conservare allocazione distinta per sprite e controllo `GxmDisableFixedSnapshot`; non introdurre caching per versione.

**Verifica:** due snapshot identici condividono il puntatore; una modifica a matrici, PN, luce, materiale o texgen non altera quello precedente; sprite non condivisi; flag di riferimento; flush/reset. Contare le allocazioni del contenitore dopo warmup per una sequenza di duplicati, senza assumere la stessa politica deque fra host e Vita.

**Completato quando:** valori e durata degli snapshot identici al riferimento, nessun inserimento/rimozione sui duplicati. Il costo di costruzione del payload può restare: non includere una seconda ottimizzazione nello stesso task.

### AVN-06 — Definire la vista di draw immediato [A per design, S per test puri, P2]

**Dipendenze:** AVN-04 stabilizza l'osservazione. **Finding:** F03. **File:** S09 e test submission/command stream; lettura dei due header Renderer.

**Risultato:** specifica e adapter di una vista con geometria/metadati e riferimenti const a `GpuDrawUniforms`, texture e snapshot fixed. Una vista può essere consumata solo sincronicamente. La conversione da packet rispetta `sharedState`; nessuna conversione tramite reinterpret_cast dal payload CPU.

**Verifica:** packet inline/condiviso produce la stessa vista; copy/move del CommandStream conserva lo stato proprio; nessun riferimento in una coda oltre la vita dell'owner. Non modificare il ramo nativo in questa scheda.

**Completato quando:** interfaccia e regole di lifetime sono revisionabili, test distinguono i due tipi di ownership. Non considerarla un'ottimizzazione completata: AVN-07 deve collegarla al percorso reale.

### AVN-07 — Usare la vista nel percorso GXM diretto [A, P2]

**Dipendenze:** AVN-06. **File:** S03, S05, S06 e relativi header, test/probe submission.

**Risultato:** direct submit usa la vista; il percorso a packet resta adapter allo stesso codice nativo. Mantenere flush dei comandi precedenti, flush degli stream, controlli di errore e `mark_current_submitted`. Il caching di un payload GPU compatto richiede una revisione valida oppure una conversione esplicita; non riferirsi a campi CPU con tipo incompatibile.

**Verifica:** sequenze stream→static→stream, copy EFB fra draw, rollover arena, shader fallito e fallback; uniform cambiati fra due draw; batching ON continua ad avere proprietà del payload. Suite host, GXM stabile/controllo e confronti di immagini/stato su Vita.

**Completato quando:** il packet completo non è più materializzato nel percorso diretto eleggibile, il ramo accodato conserva gli snapshot e il codice nativo non è duplicato. Stop se servono modifiche al FIFO o al retirement GPU.

### AVN-08 — Pubblicare osservazione e memoria coerenti [S; review A, P1]

**Dipendenze:** AVN-04 se cambia la composizione delle statistiche. **Finding:** F08. **File:** S11, `vita_published_snapshot.hpp`, `vita_memory_budget.hpp`, test submission e Runtime-Tuning.

**Risultato:** aggiungere un accessor esplicito del budget memoria completato, con identità frame/validità, pubblicato dal thread proprietario. Documentare gli accessor sincroni e quelli owner-thread. Conservare le API esistenti salvo migrazione dichiarata; lo snapshot nuovo non attende la GPU e non significa “GPU completata”.

**Verifica:** prima del primo frame, dopo pubblicazione, frame fallito, shutdown/reinit, lettore e scrittore concorrenti senza valori incoerenti. Nessun accessor pubblicato legge mappe/cache vive dal game thread. Validare che non accodi callback GX.

**Completato quando:** il port può aggiornare diagnostica e overlay dai dati pubblicati senza `run_sync` e con frame di provenienza noto.

### AVN-09 — Decidere il batching sulla baseline direct [S, P2; analisi]

**Dipendenze:** AVN-01 e cattura rappresentativa. **Finding:** F06. **File:** lettura S03/S08/S10, `vita_draw_adapter.cpp`, test submission; output un report breve sotto `docs/`.

**Risultato:** confronto direct vs batching sugli stessi frame/scena, conteggi merge/reject e casi sintetici che separano padding da incompatibilità dello stato. Distinguere percentuale di draw fusi e frame time.

**Completato quando:** esiste una decisione motivata: mantenere direct, sviluppare un accumulo limitato oppure correggere un vincolo specifico. Se manca la cattura, consegnare protocollo e casi riproducibili, lasciando la decisione aperta. Qualsiasi modifica all'indicizzazione o all'ordine richiede una nuova scheda A.

### AVN-10 — Progettare retirement selettivo solo dove serve [A, P2; analisi]

**Dipendenze:** AVN-01, delta dei finish e prova che le attese incidano. **Finding:** F07. **File:** lettura S05/S06/S08, `gxm_memory.*`, `vita_static_geometry.hpp`; output specifica del protocollo e matrice dei test.

**Risultato:** mappa risorsa→ultimo uso→evento di completamento; includere vertex/fragment/transfer e più scene nello stesso frame. Definire seriali, esaurimento/riuso delle notifiche, reclamation e comportamento in errore. Mantenere il finish globale come controllo e per i casi non dimostrati.

**Completato quando:** ogni riuso/distruzione ha una prova di completamento, con test per overflow, present scartato, EFB/feedback, suspend/resume e shutdown. Non implementare tutto il protocollo in questa scheda. Se i finish sono trascurabili, registrare il risultato e posticipare.

### AVN-11 — Definire il modulo di servizi Vita [S; review A, P3; analisi]

**Dipendenze:** nessuna, ma separata dalle ottimizzazioni. **File:** lettura S12/S13, `cmake/aurora_core.cmake`, probe SDL3/VitaGL e GXM, documentazione building.

**Risultato:** contratto del futuro target opzionale `aurora::vita_platform` (nome proposto), ordine init/shutdown, ownership degli eventi e hook di sospensione/ripresa. Identificare servizi forniti dal port e quelli riutilizzabili da SDL3, PAD e CARD. Proporre un probe minimo GXM + servizi senza secondo renderer.

**Completato quando:** dipendenze, API e sequenza di lifecycle sono definite; input, audio, storage e probe diventano attività separate. Non collegare l'intero core desktop o implementare un launcher per completare questa scheda.

## 9. Verifica richiesta e soglie di accettazione

### Controlli locali

Per modifiche runtime eseguire i test mirati durante lo sviluppo e l'intera suite prima del handoff. Per sola documentazione/preset verificare link, sintassi e opzioni effettive; evitare test nuovi che rispecchiano soltanto il testo.

```sh
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests --output-on-failure
VITASDK=/usr/local/vitasdk cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8
```

Prima di AVN-00, il profilo con le tre flag ON si riproduce così:

```sh
VITASDK=/usr/local/vitasdk cmake --preset vita-gxm -B build/vita-gxm-fast \
  -DAURORA_VITA_ASYNC_GX=ON \
  -DAURORA_VITA_GXM_DIRECT_STREAM_WRITE=ON \
  -DAURORA_VITA_GXM_DIRECT_DRAW_SUBMIT=ON
cmake --build build/vita-gxm-fast --parallel 8
python3 tools/check_vita_gxm_binary.py build/vita-gxm-fast/aurora_vita_gx_probe \
  --map build/vita-gxm-fast/aurora_vita_gx_probe.map \
  --nm /usr/local/vitasdk/bin/arm-vita-eabi-nm
```

Ripetere l'audit per il probe GXM e il profilo di controllo. Per il controllo impostare esplicitamente le tre flag OFF: una cache preesistente può conservare valori precedenti. Il binario del gioco che incorpora Aurora deve essere ricompilato e identificato separatamente.

| Modifica | Test mirati esistenti da estendere/verificare |
| --- | --- |
| Lane e semafori | `vita_cpu_workers`, `vita_regression` |
| Snapshot, packet, viste, batching | `vita_submission`, `vita_command_stream`, `vita_regression` |
| Revisioni, traduzione, fallback | `vita_frontend_translation`, `vita_backend_contract` |
| Packing/indici | `vita_vertex_pack`, `vita_regression` |
| Build/strumenti | `vita_renderer_selection`, `vita_binary_audit_contract`, `vita_performance_compare` |

Per stato, indici o lifetime usare inoltre ASan/UBSan in una directory host distinta. La CI [vita-contracts.yml](../.github/workflows/vita-contracts.yml) contiene già i flag sanitizer. Il test host non compila il renderer GXM reale: verificare anche il probe nativo per ogni modifica ai suoi rami.

### Confronto hardware

- Baseline: tre flag stabili ON; candidata: la stessa configurazione più **un solo intervento**. Il profilo OFF è un controllo di correttezza/bisezione separato.
- Stesso titolo, scena, input ripetibile, clock, risoluzione, asset e stato delle cache; almeno tre ripetizioni confrontabili.
- Registrare hash installato, revisione e configurazione del port, frame time mediano/p95/p99, attese producer/consumer, finish per causa, draw nativi, upload, memoria e compilazioni shader. Non interpretare automaticamente la latenza della display queue come puro tempo GPU.
- Verificare immagini, ombre, alpha/orientamento EFB, scissor parziali, TLUT/texture dinamiche, indexed PN, fallback e suspend/resume.
- Accettare una modifica di prestazioni quando la riduzione del costo mirato supera il rumore fra ripetizioni e non peggiora code di latenza o memoria oltre il budget concordato. Se le prove sono inconcludenti, tenere la nuova strada opzionale; non inventare una percentuale minima universale.

```sh
python3 tools/compare_vita_performance.py reference.log candidate.log \
  --reference-id 'hash installato; titolo/scena; opzioni baseline' \
  --candidate-id 'hash installato; stessa scena; singola modifica' \
  --warmup 60 --output comparison.json
```

`--warmup` conta campioni registrati. FRAME radi danno una distribuzione campionata; le medie di 120 frame di `phase_profile.log` non producono p95/p99 di frame individuali. I contatori finish cumulativi richiedono differenze fra snapshot della stessa sessione.

## 10. Evidenze riprodotte durante questo audit

### Build, test e binari

- `cmake --preset vita-host-tests`: riuscito; warning di deprecazione CMake nella dipendenza robin_hood.
- Build host: riuscita. CTest: **11/11**, tempo riportato **1,59 s**; è durata dei test host, non prestazione Vita.
- Build `vita-gxm` e `build/vita-gxm-fast`: riuscite per entrambi i probe. I controlli non sono nuove sessioni di gameplay.
- ELF/map: **4/4 PASS**, simboli nativi draw/present presenti, nessuna libreria/entry point GL/vitaGL/vita2d rilevata.
- Per tutti e quattro i VPK, `eboot.bin` coincide byte per byte con il SELF locale.
- Toolchain della misura ABI: `arm-vita-eabi-g++ 15.2.0`; profili GXM Release, ABI SDK, LTO ON, frontend GX ON. Nel profilo fast le tre flag sono ON; texture native CMPR/GX OFF, runtime logging ON.
- Questo audit non ha rieseguito ASan/UBSan: il precedente report li documenta per il refactor. La nuova evidenza corrente è quella elencata sopra.

| Profilo / probe | SHA-256 SELF = eboot del VPK |
| --- | --- |
| `vita-gxm / aurora_vita_gx_probe` | `fea299368f506c1789618aac47f390970fc80079ce6c8528226845a13db2d97b` |
| `vita-gxm / aurora_vita_gxm_probe` | `aaf0ef94729b981c67782d2ca4daecd5f2ab8a6e83da9d2dcef3878e5fc05fac` |
| `vita-gxm-fast / aurora_vita_gx_probe` | `4778459f6ed57882b5aafd952d20ca3bf22c542795a3f5403cafe66c03e7ce06` |
| `vita-gxm-fast / aurora_vita_gxm_probe` | `70d280cdd0f4fcdb224a532172b8cea31d5c63b81221ddc612506e5d85b5ec73` |

### Dimensioni ARM, non stime host

Un translation unit temporaneo include `vita_gfx_types.hpp` ed emette array globali `char audit_Type[sizeof(Type)]`. Compilazione con `-std=c++20 -O3 -march=armv7-a -mfpu=neon -mfloat-abi=hard -DAURORA_VITA_RENDERER_GXM=1`; lettura delle dimensioni tramite `arm-vita-eabi-nm -S`. Nessuna esecuzione su console necessaria per questa misura di layout.

| Tipo | Byte |
| --- | ---: |
| `DrawPacket` | 1080 |
| `DrawUniforms` | 1516 |
| `GpuDrawUniforms` | 620 |
| `array<TextureBinding, MaxTextures>` | 352 |
| `FixedVertexUniforms` | 3096 |
| `FrameStats` | 320 |

Disassemblato ispezionato: `build/vita-gxm-fast/aurora_vita_gx_probe`, SHA-256 ELF **`3e14f509f20f6e7cf45a9b8021dff555f963da9aeb20285b581ff2d94a53bd4c`**. `gfx::Renderer::draw` è presente e chiama `gxm::Renderer::draw`, poi copia i campi F02. La funzione vuota `invalidate_resource_bindings` non compare nel simbolario di questo ELF. Questi dati motivano la priorità delle copie rispetto alla semplice riduzione dei nomi di funzione.

## 11. Proposte da non assegnare come piccoli cleanup

- Cache GXM attraverso BeginScene o eliminazione di reserve/rebind uniform.
- Riuso degli snapshot fixed tramite sole versioni, senza equivalenza completa degli input.
- Memo generico del descrittore GX: `aurora_gx_bridge.cpp` conserva la nota che il precedente hash/compare/copy costava più della traduzione. Quel valore storico non è un benchmark del checkout attuale.
- Eliminazione del FIFO, puntatori guest presunti immutabili, riordino dei draw o fusione attraverso EFB/clear/barrier.
- Riuso di memoria GPU deciso da un ritardo arbitrario, rimozione globale di finish o cambio di cacheability delle pagine.
- GPU fixed per ogni primitiva, D16/risoluzione ridotta o texture native abilitate globalmente come “ottimizzazione gratuita”.
- Riscrittura contemporanea di scheduler, batching, buffer e frontend. Impedirebbe di attribuire guadagni e regressioni.

Il lavoro iniziale può rimanere limitato a profili, evidenza, helper distinti, statistiche e scratch degli snapshot. Il refactor più ampio del percorso immediato viene dopo contratti e misure utilizzabili dai task successivi.
