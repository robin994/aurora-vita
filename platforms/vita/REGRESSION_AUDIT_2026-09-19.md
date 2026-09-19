# Audit regressioni e prestazioni Aurora / sceGxm — 19 settembre 2026

## Ambito e conclusione

Baseline indicata dall'utente: `85505768070821171682fca6eeed8fbd4f149abc`.
Revisione analizzata: `f4a2af4` su `vita-experiment`, 13 commit successivi,
43 file modificati. Checkout iniziale pulito. L'utente segnala perdita delle
ombre e immagini specchiate; il campione Strikers mostra anche elementi
sovrapposti/duplicati e illuminazione piatta, con overlay a 2 FPS.

Sono corretti difetti verificabili nel codice e ripristinati default compatibili
con la baseline. Non e' dimostrato che tutti i difetti del campione abbiano una
sola causa, ne' che siano risolti visivamente: in questa sessione non e' stato
eseguito un nuovo test su Vita reale. L'immagine, il nome del file e il contenuto
attuale di una directory di build non identificano da soli l'eboot installato.

Il renderer resta nativo sceGxm. Le prove host riguardano contratti CPU, memoria
e sincronizzazione simulata; non eseguono la GPU. I VPK prodotti sono probe di
Aurora, non un nuovo pacchetto del gioco Strikers.

## Commit esaminati

| Commit | Modifica | Esito dell'audit |
|---|---|---|
| `aba1a30` | Prima fase audit: scissor, depth, uniform, geometria GPU | Scissor preciso e conservazione depth ripristinati; geometria torna opt-in. Uniform cache mantenuta. |
| `46b3ac0` | Texture native e swizzle mip | Formati packed/CMPR resi opt-in; upload swizzled valida puntatore e dimensione del buffer. |
| `55a803d` | Fixup EFB su GPU e padding arena | Ripristino sorgente prima del fallback CPU; controllo capacita' arena coerente con il padding; probe di crop/flip. |
| `fe70d62` | Decode/transform/pack diretto | Mantenuto; modalita' diagnostica split torna ad avere fasi distinte. |
| `6cdfdd1` | Shader compiler / discard costante | Workaround mantenuto; test delle combinazioni alpha gia' presenti. |
| `3dc3881` | LTO, NEON, D16 | D16 resta opt-in; flag ARM coerenti con VitaSDK, eliminato conflitto mcpu/march. |
| `7e25fd0`, `218a15a` | Risoluzione interna separata | Default segue display; scala destinazione GXCopyTex usa la stessa risoluzione della sorgente. |
| `236ca6d` | Micro-op decoder | Corretto overwrite dei campi del vertice; validazione conteggi; confronto con decoder di riferimento. |
| `1c65ca4` | Worker spin/sleep e comandi separati | Corretto blocco worker; storage comandi riutilizzato dopo reset. |
| `a73141f` | XXH3 e scansione cache su miss | Hash mantenuto con confronto esatto; rimossa scansione lineare aggiuntiva. |
| `7cd76cc`, `f4a2af4` | Robin Hood e alias mappe | Mantenuti; cache che espongono puntatori continuano a usare nodi o oggetti indiretti stabili. |

## Difetti e correzioni

### P1 — Scissor pixel sostituito da region clip

La prima fase forzava `fragmentScissor=false` sia nella facade sia nella creazione
del programma nativo. `sceGxmSetRegionClip` e' un clip a tile, non la prova di uno
scissor esatto al pixel. Un rettangolo non allineato puo' quindi modificare pixel
esterni, incluso un atlante usato dalle ombre. Rimane il clip hardware grossolano,
ma il programma conserva il controllo preciso richiesto. Una pipeline senza
fragment scissor rifiuta una draw con scissor parziale.

Evidenza primaria: [VitaSDK, SceGxmRegionClipMode](https://github.com/vitasdk/vita-headers/blob/master/include/psp2/gxm.h)
descrive i tile; [vitaGL, update_scissor_test](https://github.com/Rinnegatamante/vitaGL/blob/master/source/tests.c)
usa anche un pass dedicato. La precedente proposta A1, che assumeva granularita'
pixel e costo nullo, non era una base valida per rimuovere il controllo.

### P1 — Worker: notifica persa e token di completamento residuo

I flag separati `sleeping`, `doneWaiter` e `state` non formavano un protocollo
atomico. Il produttore poteva saltare una wait dopo aver osservato `state=done`
mentre il worker stava ancora pubblicando il token. Quel token poteva essere
consumato dal lavoro successivo, anticipandone la fine. La transizione al sonno
consentiva inoltre una notifica persa nel modello di memoria ARM.

Ripristinato un token wake e un token done per ogni lavoro. Il contesto e'
riutilizzato solo dopo l'acknowledgement. Resta la soglia minima di lavoro per
lane: le draw piccole continuano sul thread chiamante. Il test host compila il
vero ramo Vita con uno shim dei semafori; prima della correzione riproduce un
timeout di 3 secondi, dopo passa 8.000 lavori con dimensioni/lane alternate,
copertura esatta, errori delle callback e shutdown. Non misura lo scheduler Vita.

### P1 — Risoluzione interna e copia ombre con scale diverse

Il default era diventato 640x448, ma `DrawSink::copy_tex` calcolava le dimensioni
destinazione usando il backing di display 960x544. La sorgente usava invece
`map_logical_scissor` e quindi la risoluzione interna. Anche la successiva
normalizzazione 1:1/2:1 poteva allargare il rettangolo ai pixel sbagliati.

Ora sorgente e destinazione usano lo stesso render extent. I default
`render_width=0`, `render_height=0` seguono il display come nella baseline.
Ridurre la risoluzione rimane possibile esplicitamente e richiede confronti
di GXCopyTex, GXCopyDisp, UI e ombre sul titolo reale.

### P1 — Geometria GPU attivata globalmente e fallback incompleto

Il budget statico era passato da 0 a 8 MiB, attivando anche trasformazioni,
illuminazione e texgen GPU senza confronto hardware valido per tutti i titoli.
Ripristinato il default 0. Se un candidato GPU fallisce per budget, cache o
shader, il fallback CPU ricarica ora lo stato completo, invece di consumare
quello ridotto preparato per il percorso GPU.

Attenzione all'integrazione: la copia locale di Strikers letta durante l'audit
imposta autonomamente `static_geometry_budget=8 MiB` per GXM in `src/Game/main.cpp`.
Il default di Aurora non annulla quella scelta. Per il controllo CPU usare
`STRIKERS_STATIC_GEOMETRY_MB=0` e verificare il log `gpu_fixed_vertex=0`.
La sua build configurata usa inoltre `extern/aurora-vita`, una copia separata:
aggiornare quel riferimento o configurare `STRIKERS_AURORA_VITA_ROOT` con questo
checkout prima di ricostruire il gioco. Quel worktree non e' stato modificato.

### P1/P2 — Decoder precompilato non equivalente

Il decoder di riferimento copia al massimo XYZ/STQ; le micro-op scrivevano
tutte le componenti. Con quattro componenti potevano sovrascrivere W, il
vettore successivo, i colori o i selettori di matrice. Mancavano anche controlli
per componenti zero/>4 e conteggio micro-op fuori limite. Ora i limiti sono
espliciti e il risultato coincide con il riferimento su sorgenti dirette e
indicizzate, endian, semantiche e formati numerici diversi.

Il nuovo test collegato alle librerie originali registra 361 fallimenti prima
dell'arresto su input malformato; la versione corretta passa 3.013 controlli
complessivi. Sono casi di correttezza e robustezza: non dimostrano che i vertici
del campione Strikers abbiano proprio quel formato errato.

### P2 — Errori EFB, depth e descrittori riutilizzati

- Un fallimento dopo il cambio di target nel fixup GPU poteva far leggere il
  target destinazione al fallback CPU. Ora viene ripristinata la sorgente.
- La politica store depth torna a essere stabilita prima di BeginScene;
  EndFrame non rende automaticamente morto il depth di un target offscreen.
- Ricreando il descrittore della texture alias di display si invalida anche la
  cache del sampler. Le uniform UV senza binding espliciti partono dall'identita'.
- Il probe GXM con frontend aggiunge 24 confronti pixel con il riferimento CPU:
  crop asimmetrico, copie 1:1/2:1, tutte le combinazioni flip X/Y, alpha e A8.
  Il probe nativo mantiene la verifica degli scissor non allineati ai tile.

### P2 — Arena e misure fuorvianti

`can_reserve` ignorava il padding aggiunto da `reserve`, quindi prometteva spazio
che la scrittura poteva rifiutare. I due controlli ora coincidono e l'allineamento
gestisce l'overflow aritmetico. Sono presenti test delle capacita' al limite.

Il percorso streamed bypassava `profile_split_vertex_phases`: ora una richiesta
split usa il percorso che misura separatamente decode/transform. La riga
RENDERER include `frame`, `sampled` e `split_vertex_phases`; `sampled=0` significa
che i tempi per draw non sono stati raccolti, non che il loro costo sia zero.
Il riepilogo nativo viene aggiornato dopo present, includendo le scene interne.

## Audit prestazioni: cosa migliora e cosa misurare

| Priorita' | Intervento | Evidenza e criterio |
|---|---|---|
| Applicato | Riutilizzare storage DrawPacket | Sul Mac: 2.048 draw x 20 frame, 2.540 allocazioni dopo warmup prima, 0 dopo. Packet host 1.072 byte. Contratto aggiunto ai test. Nessun guadagno FPS Vita dedotto. |
| Applicato | Eliminare scansione lineare della cache dopo un miss | Evita fino a 1.024 confronti aggiuntivi per miss a cache piena. Era giustificata con un problema di un altro progetto; dopo Robin Hood non aveva evidenza locale. |
| Applicato | Mantenere decode/transform/pack fuso e cache uniform | Rimossi errori senza disattivare queste ottimizzazioni. Le misure split sono diagnostiche, non comparabili direttamente al throughput fused. |
| 1 | Attribuire frame time e attese GPU/EFB | Registrare frame completi, scene, EFB, wait, upload, compilazioni e present separando intervalli inclusivi. Non sommare `draw_frontend`, decode, geometry cache e submit come tempi esclusivi. |
| 2 | Ridurre interruzioni di scena e copie realmente superflue | Priorita' solo se misure mostrano EFB/sincronizzazioni dominanti. Preservare ordine GXCopyTex, alias, depth e dipendenze; non rimuovere finish senza prova del ciclo di vita. |
| 3 | Varianti senza fragment scissor per copertura completa | Selezionarle solo quando l'intero target e' coperto. Per rettangoli parziali mantenere semantica esatta, oppure implementare una maschera verificata. Nessuna rimozione globale. |
| 4 | Geometria GPU per categorie validate | Prima unlit/fixed PN, poi indexed PN e luci. Confrontare framebuffer CPU/GPU, matrici cambiate e cache piena. Usare hit/miss e byte residenti per decidere il budget. |
| 5 | Risoluzione 640x448 e 480x272 | Rispetto a 960x544: 54,9% e 25% dei pixel. Sono rapporti di area, non previsioni FPS; non riducono tutto il lavoro CPU o tutte le allocazioni del backing. |
| 6 | Texture native, D16, LTO, memoria | Un'opzione alla volta. Formati/BC1/mip richiedono checker asimmetrici e alpha; D16 richiede test z-fighting. LTO va confrontato per tempo/size; nessun fast-math globale aggiunto. |

I numeri di 69 ms transform e 426,6 ms/frame nel precedente PERFORMANCE_LAB
sono dati storici di un'altra configurazione, non misure della patch presente.
Il solo overlay a 2 FPS non attribuisce il collo di bottiglia alla CPU o GPU.
`submit_*_us` misura tempo CPU nelle API, non timestamp di esecuzione della GPU.

## Protezioni e riproduzione

```sh
cmake --preset vita-host-tests
cmake --build --preset vita-host-tests --parallel 8
ctest --preset vita-host-tests

export VITASDK=/usr/local/vitasdk
cmake --preset vita-gxm
cmake --build --preset vita-gxm --parallel 8
python3 tools/check_vita_gxm_binary.py build/vita-gxm/aurora_vita_gx_probe \
  --map build/vita-gxm/aurora_vita_gx_probe.map \
  --nm "$VITASDK/bin/arm-vita-eabi-nm"
```

CI aggiunta per i contratti host in Release e con AddressSanitizer/UBSan.
La CI non equivale a un test su dispositivo. Native packed GX textures e CMPR
usano ora le opzioni CMake `AURORA_VITA_NATIVE_GX_TEXTURES` e
`AURORA_VITA_NATIVE_CMPR`, entrambe OFF per una nuova configurazione GXM.
Una cache CMake esistente con valori ON conserva l'opt-in: verificarla.

Per chiudere la verifica hardware: conservare baseline e relativo eboot/hash;
eseguire i due probe (marker frontend `build=regression-20260919`); poi ripetere
la stessa introduzione Strikers e una partita con il controllo CPU. Verificare
ombre, orientamento, luci, UI, pausa/ripresa e cambi scena. Raccogliere almeno
300 frame warm e piu' run a clock/config identici, mediana/p95/p99, screenshot
comparabili, log di un singolo boot e hash del binario realmente installato.
Solo allora confrontare una singola ottimizzazione e accettarne il default.

Nessuna suite puo' garantire assenza futura di regressioni: questi controlli
rendono riproducibili i difetti trovati e impediscono di considerare una build
riuscita come prova visiva o prestazionale.
