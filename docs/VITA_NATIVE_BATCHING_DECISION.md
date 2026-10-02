# AVN-09 — Direct submit e batching

Stato: analisi consegnata; scelta prestazionale aperta in assenza di catture Vita.
Base: `bb5147e1e1aebc959a56dd85dc4010c7c9d17afd` più il diff di implementazione
2026-10-02 descritto in [handoff](VITA_NATIVE_IMPLEMENTATION_2026-10-02.md).

## Decisione attuale

Conservare direct submit come baseline. `gxm_local_draw_batching` resta `false`.
Non cambiare allineamento, indici, ordine o retirement per ottenere più fusioni.
Con batching ON i draw streamed eleggibili entrano in CommandStream e quindi
non usano la vista immediata: i due percorsi hanno costi diversi da confrontare.
La riduzione del numero di draw non dimostra una riduzione del frame time.

## Casi riproducibili già nel test vita_submission

`local_batching()` prepara triangoli nella vera StreamingArena ed esegue il
vero `enqueue_streamed_draw`, con il pool di buffer host. I casi distinguono:

| Caso | Risultato richiesto |
| --- | --- |
| Slice contigue, stesso stato, alignment=2 | Tre draw logici diventano uno; indici 0..8 esatti; due fusioni |
| Stesso stato, alignment=16 | Due draw; padding degli indici impedisce la fusione |
| Texture/UV crop, MVP, scissor o pipeline cambiati | Draw distinti, nessun rebase |
| Snapshot fixed diversi | Draw distinti, payload precedenti immutabili |
| Clear, target, copy EFB o barrier interposti | Nessuna fusione attraverso il confine |
| Indice locale fuori slice, buffer/offset errato | Nessuna fusione o scrittura su memoria estranea |
| Slice già flushata | Il rebase non riscrive gli indici già inviati |
| Totale vicino a 64000 vertici | Il limite nativo resta rispettato |

Questi sono risultati host di correttezza. L'allineamento di 2 nel test isola il
predicato; non è una proposta di configurazione del renderer Vita.
Il contatore `batchRejectedState` attuale include anche slice non contigue:
non interpretarlo come una misura esclusiva dei cambi di stato. Per attribuire
il padding di un gioco servirà una cattura diagnostica mirata dei confini,
separata dal benchmark a bassa perturbazione.

## Protocollo da eseguire nel port

1. Compilare il gioco con la baseline `vita-gxm-stable`, conservando commit/diff,
   opzioni CMake effettive, configurazione BackendConfig e hash installato.
   Lo strumento manifest identifica anche i probe, ma i loro FPS non sono quelli
   del gioco. Registrare titolo, scena, clock, risoluzione e stato della cache.
2. Preparare due varianti identiche salvo `gxm_local_draw_batching=false/true`.
   Per isolare il batching, lasciare `gxm_immediate_draw_view=false` e
   `cpu_distinct_core_dispatch=false` in entrambe. Solo dopo confrontare batching
   con la vista candidata. Nessuna modifica contemporanea a texture o depth.
3. Acquisire almeno tre ripetizioni per variante della stessa sequenza di gioco,
   alternando A/B. Separare training/cache fredda dalle catture calde; fissare
   durata e warmup prima di osservare i risultati. Registrare gli input o usare
   una sequenza del gioco ripetibile.
4. Salvare FRAME log distinti per sessione e un manifest per ciascun binario.
   Il comparatore esistente accetta campioni FRAME e segnala campionamento
   sparso; le medie a 120 frame non diventano campioni. Non sommare fasi annidate.
5. Confrontare mediana/p95/p99, draw logici, draw nativi, candidati/fusioni/reject,
   scene, delta finish per causa e compilation miss. Le grandezze mancanti vanno
   raccolte sul proprietario del renderer, pubblicando dati per frame, evitando
   getter sincroni sul game thread. Non usare finish diagnostici nel benchmark.
6. Confrontare immagini/ombre/copied texture/scissor e ripetere PS-button
   suspend/resume, rollover dell'arena e present scartato.

## Esito e prossima scheda

Scegliere prima la soglia d'interesse del port. Proposta iniziale: riduzione
ripetibile di almeno il 5% della mediana, p95/p99 senza peggioramento fuori dalla
variabilità delle ripetizioni e nessuna differenza funzionale/visiva. La soglia
è un criterio proposto, non un risultato misurato.

- Poche fusioni o nessun vantaggio temporale: mantenere direct.
- Fusioni utili ma costo della coda dominante: nuova scheda per un accumulo
  limitato di soli draw consecutivi, con flush obbligatorio a ogni confine.
- Padding dominante: nuova scheda per risolvere uno specifico vincolo di slice,
  con prove sugli indici e sul retirement; nessun cambio globale di alignment.

Non esiste una cattura nuova del gioco: nessuna di queste tre conclusioni
prestazionali viene dichiarata acquisita.
