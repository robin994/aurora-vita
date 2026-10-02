# AVN-10 — Contratto proposto per retirement selettivo

Stato: specifica e matrice di prove consegnate. Il runtime conserva il finish
attuale; notifiche e distruzione differita non sono implementate in questa scheda.
Base: `bb5147e1e1aebc959a56dd85dc4010c7c9d17afd` più il diff 2026-10-02.

## Gate di misura

Acquisire due snapshot completati dello stesso processo/configurazione e fare
il delta di `nativeFinishReasonCalls` e `nativeFinishReasonWaitUs`. I contatori
sono cumulativi e comprendono anche operazioni fra frame; scartare reset/sessioni
diverse. Attribuire Explicit, BufferMutation, TextureMutation, ResourceDestroy,
Readback, TargetMutation, StreamReuse e FrameDiscard. Il tempo è un'attesa CPU:
non attribuirlo automaticamente a un solo draw o sommarlo a fasi che lo includono.

Procedere con una futura implementazione solo se il costo interessa il port e
una causa identificata può usare una prova più stretta. Se manca la misura,
conservare il percorso attuale. Non aggiungere frame presunti sicuri e non
considerare `completed_memory_snapshot()` una prova di completamento GPU.

## Mappa di ownership e completamento

Ogni risorsa candidata ha un'identità `(kind, handle, generation)`; il riuso di
un handle non eredita l'ultimo uso della precedente allocazione.

| Risorsa | Ultimo uso da tracciare | Prova richiesta prima di riuso/distruzione |
| --- | --- | --- |
| VBO/IBO e pagine streaming | Ogni draw che consuma la slice, anche dopo un flush nello stesso frame | Completamento della scena che contiene l'ultimo draw; usare entrambi gli stadi finché il solo vertex non è dimostrato sufficiente |
| Texture e sampler descriptor | Ogni campionamento fragment, anche varianti scissor-free | Fragment completo e ogni transfer che legge/scrive la stessa allocazione |
| EFB color/depth e target | Ogni scena/clear, feedback, resize e copy | Vertex/fragment e transfer rilevanti; nessun target bound può essere rimosso |
| Pipeline/programmi condivisi | Ogni draw/clear/blit che usa owner o variante | Vertex/fragment; nessun programma condiviso liberato prima dell'ultimo consumer |
| Risorse della copia EFB | Sorgente letta e destinazione scritta, incluse copie asincrone | Evento del transfer e dipendenze fra motori; CPU fixup/readback richiedono il completamento della scrittura |
| Superfici di display/sync object | Scene e richieste della display queue | Completamento rendering e rilascio da parte della coda display; la notifica fragment da sola non basta |
| Payload CPU della vista | Consumo di setter/upload prima del ritorno dal draw | Contratto sincrono già implementato; non usarlo per inferire retirement dei buffer GPU |
| Ring uniform/parameter gestiti da GXM | Reserve/upload e scena | Conservare le regole del SDK; nessun riuso manuale anticipato |

Non confondere seriale di frame, slot dell'arena e seriale di submission: un
frame può avere più scene, cambiare target, esaurire l'arena o non presentare.

## Protocollo della futura implementazione

1. Unico proprietario del contesto GXM. Helper CPU non allocano token GPU, non
   cambiano last-use e non liberano risorse. Gli usi della scena aperta sono
   registrati prima di ogni draw e aggiornati per clear/blit/copy interni.
2. Seriali CPU monotoni a 64 bit, separati per dominio vertex/fragment/transfer;
   ogni scena/transfer realmente accettato riceve un record. Un errore di
   submission non viene marcato completato e non sblocca risorse.
3. La scena aperta deve essere chiusa prima di attendere il suo ultimo uso.
   Nessun wait su un token che la GPU non potrà ancora emettere. Nei cambi target
   e nell'overflow nello stesso frame, sigillare il record corretto.
4. Il SDK locale dichiara `sceGxmGetNotificationRegion`, `SceGxmNotification`,
   `sceGxmNotificationWait`, notifiche in EndScene e nelle API transfer. La
   dichiarazione non dimostra visibilità/coerenza o ordine fra motori: verificare
   quei contratti e l'integrazione hardware prima di sostituire finish.
5. Pool limitato di slot notification con token a 32 bit e generazione CPU a
   64 bit. Slot non riutilizzabile finché esistono submission o risorse che
   dipendono dal token precedente. Vietati confronti approssimativi su frame,
   valori `>=` senza gestione wrap e seqlock su dati ordinari.
6. Esaurimento pool: chiudere la scena e attendere un record realmente pendente;
   se non dimostrabile, usare finish globale. Prima del wrap del token: finish
   di rendering/transfer, drain display e reset controllato del pool/epoca.
   Mai riscrivere una notification ancora posseduta dalla GPU.
7. Una prova aggiorna soltanto il dominio corrispondente. Vertex completato non
   implica fragment o transfer; un motore non fornisce una sequenza globale
   degli altri. L'ARM/cache visibility deve seguire il contratto del SDK.
8. Deferred destroy mantiene memoria e descriptor fino alla prova completa;
   la risorsa diventa subito non risolvibile per nuove submission. Limite di
   memoria della coda: attendere o fallback finish, mai crescita senza limite.
9. Mutazioni verificano tutti gli ultimi usi prima di scrivere. Readback e CPU
   fixup restano globali finché le dipendenze non sono provate. Un fallimento
   nel wait/transfer porta a errore conservativo, senza reclamation speculativa.
10. Suspend/shutdown: bloccare nuovi ingressi, drenare FIFO, chiudere scene,
    completare transfer/render/display, reclamare, poi distruggere. Resume
    ristabilisce stato su ogni BeginScene; nessuna cache di contesto oltre scena.

Il finish globale rimane selezionabile come controllo. Prima del merge la futura
scheda deve definire l'opzione di confronto e la gestione di ogni errore.

## Matrice obbligatoria per la futura scheda A

| Transizione | Assertion di correttezza / prova Vita |
| --- | --- |
| Due draw sulla stessa pagina, EFB flush interposto | Last-use è il secondo draw, mai quello del primo flush |
| Più scene/target nello stesso frame | Ogni allocazione attende la propria scena e gli stadi necessari |
| Overflow arena prima del present | Chiusura scena precede wait; nessun token non inviato |
| Upload/mutazione texture dopo transfer | Lettura e scrittura del transfer concluse prima della CPU mutation |
| EFB feedback/copy/resize, orientamento e alpha | Nessun accesso a color/depth prematuramente riusato; immagini equivalenti |
| Distruzione pipeline/variante e handle riutilizzato | Generazioni separate e programmi condivisi ancora vivi |
| Present scartato o callback display lento | Nessuna reclamation basata sul solo frameIndex |
| Pool pieno / token vicino al wrap | Limiti rispettati, fallback completo, epoca nuova priva di riferimenti vecchi |
| Errore EndScene/transfer/wait | Nessuna completion inventata, errore propagato, teardown conservativo |
| PS button / suspend/resume | Drain completo e stato ricostruito; nessuna cache fra scene |
| Shutdown con risorse pending | Tutte le dipendenze completate prima di free/context destroy |

Servono test host di ledger/generazioni con errori iniettati e probe Vita che
producano log di token, hash e immagini. Tali test appartengono alla futura
implementazione del ledger; questa specifica non ne simula una prova acquisita.
