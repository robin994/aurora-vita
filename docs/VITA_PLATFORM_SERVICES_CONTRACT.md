# AVN-11 — Servizi Vita: contratto del target opzionale

Stato: design consegnato. `aurora::vita_platform` è un target futuro proposto,
non un target CMake o un servizio già disponibile. Base: `bb5147e` e sorgente
corrente del 2026-10-02. Input, audio, storage e probe sono implementazioni separate.

## Confini rispetto al progetto attuale

Il backend standalone fornisce il renderer/GX e `lib/vita/runtime.cpp` con
configurazione/log minimi. Il core include ancora window/input/eventi e SDL3;
PAD dipende da core/SI e absl; CARD FileIO usa SDL_IOStream e mantiene le proprie
semantiche di lettura/scrittura. Il probe SDL3 attuale crea una finestra e usa
vitaGL: non è il punto d'ingresso del futuro probe GXM.

Il target opzionale deve essere indipendente dal frontend desktop, da Dawn e
da GL. Il port continua a possedere callback originali, ciclo del gioco,
memoria guest, policy clock e root dati. Il modulo possiede soltanto i servizi
che il port gli affida esplicitamente. Nessun secondo present o context grafico.

## Dipendenze e API proposte

| Area | Contratto pubblico futuro | Ownership e dipendenza |
| --- | --- | --- |
| Lifecycle | initialize(config), poll_events(), suspend(), resume(), shutdown(), last_error() | Game/platform thread; nessuna chiamata GXM da callback di sistema |
| Input | Snapshot di PAD/touch con timestamp, mapping dichiarato e stato connesso | Provider nativo Vita o SDL3 servizi; adattare ABI PAD esistente senza importare tutto il core |
| Audio | Apertura/chiusura stream, formato, callback originale del port | Thread audio dedicato, priorità/affinità dichiarate; nessun lavoro renderer o allocation per campione |
| Storage | Root per titolo, operazioni bounded con risultato ed errore | Separare storage della cache da CARD/save; preservare offset, short I/O e callback CARD |
| Tempo/clock | Tempo monotono; eventuale richiesta clock esposta al port | Nessun incremento clock implicito o cap CPU3 derivato da CapUnlocker installato |
| Diagnostica | Ultimo errore e snapshot senza drain | Pubblicare valori; non leggere cache/renderer vivo da callback o game overlay |

Configurazione proposta: versione/size della struct, servizi richiesti, provider
per servizio, root dati, hook suspend/resume del port e contesto utente. Ogni
hook dichiara il thread di esecuzione. Errori sono restituiti, senza dialoghi o
uscite di processo imposte al gioco. Re-init dopo shutdown è idempotente.

GXM rimane posseduto dal backend. Il modulo chiama hook del proprietario per
fermare e riprendere il rendering: dipendenza iniettata, non dipendenza inversa
del backend verso un target SDL. I simboli di runtime/config/log hanno un solo
provider al link; non duplicare i globali di `lib/vita/runtime.cpp`.

Un provider SDL3 può inizializzare soltanto i sottosistemi necessari ai servizi.
Non creare window/video/GL context come effetto collaterale di input/audio.
L'assenza di GL nel risultato va dimostrata tramite ELF/map: non basta nominare
un subset di SDL o compilare con una flag. Se il pacchetto SDL introduce tali
dipendenze, scegliere un provider nativo o correggere il provider in una scheda
separata. Non allargare l'allowlist del controllo GXM.

## Sequenza del ciclo di vita

1. Port configura logging/root/clock e installa i callback originali.
2. Modulo valida provider e servizi richiesti; registra eventi di sistema.
3. Inizializza storage, input e audio richiesti, con rollback inverso in errore.
   Il port dichiara se l'audio può partire prima del renderer.
4. Port inizializza backend GXM e cache; il render thread resta unico
   proprietario. Dopo startup il port avvia FIFO e ciclo originale.
5. `poll_events()` raccoglie eventi sul platform thread una volta per iterazione
   utile; i callback di sistema accodano segnali bounded e non attendono GX/GPU.
6. Suspend diventa una transizione coordinata: sospendere nuovi ingressi,
   completare i callback del gioco, drenare FIFO, chiudere scene e completare
   render/transfer/display con il percorso attuale, poi sospendere audio e
   chiudere/sincronizzare le operazioni storage richieste dalla policy del port.
7. Resume ripristina servizi e risorse effettivamente perdute; il proprietario
   del renderer riapre scene ricostruendo sempre il contesto. Pubblicare gli
   aggiornamenti guest/TLUT/EFB/runtime che invalidano le cache pertinenti.
   Nessun riuso dello stato GXM attraverso BeginScene.
8. Shutdown blocca nuovi eventi/lavori, arresta FIFO e backend, ferma audio,
   rilascia input/storage/provider e deregistra eventi in ordine inverso.
   Nessun hook usa risorse già distrutte; shutdown anche dopo init parziale.

Suspend/resume ripetuti, eventi duplicati e shutdown durante una transizione
sono gestiti da stati espliciti: Uninitialized, Starting, Running, Suspending,
Suspended, Resuming, Stopping, Failed. L'errore non riapre il rendering da un
callback di sistema; decide il port come recuperare o uscire.

## Probe minimo proposto

Nuovo target separato: backend GXM + vita_platform + provider dei soli servizi
richiesti, senza core desktop e senza il probe SDL3/vitaGL esistente. Disegnare
una scena tramite i callback GX di test, osservare input reale, eseguire I/O
bounded su un file proprio del probe e, in un'ulteriore scheda audio, riprodurre
un buffer noto. Mostrare errori e identità del build nel log; niente launcher.

Verifiche: audit ELF/map, hash installato, clear/copy/scissor, input/PAD mapping,
I/O short/error, init failure e rollback, ripetuti PS button suspend/resume,
audio senza underrun nella scena scelta, shutdown/reinit e confronto con il
backend senza modulo. Host mock verifica il lifecycle; prove Vita verificano
provider e callback di sistema. Compilazione non equivale a prova audio/input.

## Attività assegnabili successive

- VPS-00: piccolo target opzionale, API lifecycle/versione e test di rollback;
  dipendenze minime e audit link obbligatorio.
- VPS-01: provider input/PAD e mapping Vita; test valori/edge e prove device.
- VPS-02: provider audio con callback del port; misura underrun/affinità e
  confronto del carico CPU1 con i worker Aurora.
- VPS-03: provider storage e adapter CARD, conservando semantiche esistenti.
- VPS-04: probe GXM + servizi e transizioni suspend/resume con log/hash.

Assegnare queste schede separatamente dopo aver scelto provider e hook nel port
consumatore. La consegna AVN-11 è questo contratto, senza implementare l'intero
core o sostituire il ciclo originale con una UI sintetica.
