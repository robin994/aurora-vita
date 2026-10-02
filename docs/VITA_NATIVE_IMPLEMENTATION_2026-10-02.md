# Aurora Vita — Implementazione del piano Vita native

Data: 2026-10-02. Base del lavoro:
`bb5147e1e1aebc959a56dd85dc4010c7c9d17afd`, branch `vita-experiment`.
Le verifiche e gli artefatti sotto riportati sono stati prodotti prima del
commit di consegna: non attribuirli al solo commit base. I manifest sotto
`build/<profilo>/` identificano anche il diff e i file nuovi della verifica.
Il commit che aggiunge questo documento consegna l'implementazione.
Questo documento aggiorna il [piano AS-IS](VITA_NATIVE_AS_IS_2026-10-02.md),
che resta una fotografia della revisione iniziale.

## Esito e perimetro

AVN-00–08 sono implementate e verificate localmente. AVN-09–11 consegnano le
analisi/specifiche richieste dal piano. Batching, retirement selettivo e servizi
completi non erano riscritture autorizzate dalle rispettive schede: le nuove
implementazioni future richiedono le misure e i contratti lì indicati.

Nessuna nuova cattura hardware del gioco o prova visiva/FPS è stata eseguita.
La stabilità dichiarata dall'utente riguarda le tre opzioni già accettate nella
configurazione provata; non certifica i binari appena ricompilati. Le nuove
ottimizzazioni di dispatch/vista sono candidate con controllo separato.

## Handoff delle schede

| ID | Consegna concreta | Prova locale / controllo |
| --- | --- | --- |
| AVN-00 | Preset stable/control/candidate, directory distinte, descrizione async corretta a CPU2; README/build/wiki aggiornati | Preset elencati e configurati; cache effettive e build controllate. Texture native restano OFF. |
| AVN-01 | `tools/vita_build_manifest.py`: commit/diff/file nuovi, cache reale, toolchain, ELF/SELF/VPK/eboot, metadati/campioni opzionali | Fixture determinismo, OFF/ON/missing, dirty content, file mancanti, eboot diverso, installed hash e FRAME log; 5 test Python passati. Assenze hardware = unknown. |
| AVN-02 | `gfx/vita_cpu_dispatch_plan.hpp`: cap prima del filtro, CPU distinti, ranghi separati da callback ID, fallback seriale su CPU sconosciuta | Tabelle CPU0/CPU2, cap0..5, piccoli intervalli, core duplicati/assenti e size_t massimo; copertura disgiunta/esatta. |
| AVN-03 | Il pool attraversa helper scelti e raccoglie le loro completion; failed wake esegue il range localmente e riporta errore | 9000 job legacy + 1000 CPU2 non contigui, errori lane2/3, wake fallito, nested/concurrent fallback, CPU3 create/physical-ID rifiutata, shutdown. `cpu_distinct_core_dispatch=false` mantiene il dispatch originale. |
| AVN-04 | Statistiche native composte alla lettura; `cache_counters()` per delta di cinque campi; nessuna replica nativa nel facade draw/present/capture | Test di composizione e cumulativi; probe per clear/copy/execute parziale/present/errore preparato e compilato, da eseguire su Vita; ispezione ARM in questa verifica. |
| AVN-05 | `gfx/vita_fixed_snapshots.hpp`: scratch riutilizzabile, confronto esatto, append soltanto per nuovo payload | 10000 duplicati: stesso puntatore, zero nuove allocazioni del contenitore host dopo primo publish. Matrici/PN/normal/light/material/texture, sprite distinti, controllo senza riuso e clear/revisioni coperti. Nessuna misura allocator Vita. |
| AVN-06 | `DrawSubmissionView` con riferimenti const, adapter packet inline/shared e rvalue owner vietati; cache GPU tipata | Test alias/reference, proprietà della coda dopo copy/move, payload CPU modificato, revisioni nuove/zero e invalidazione. La vista non è un tipo accodabile al CommandStream. |
| AVN-07 | Direct streamed opzionale usa la vista; packet e vista entrano nella stessa funzione GXM | Flush dei comandi vecchi e dell'arena, errore, submit mark e percorso queued invariati; quattro build GXM. `gxm_immediate_draw_view=false` mantiene il packet completo. Sequenze/image equivalence del probe richiedono Vita. |
| AVN-08 | `CompletedMemorySnapshot` / `completed_memory_snapshot()`, pubblicazione owner-side con un'unica cattura memoria per pubblicazione performance+memory | Prima del frame, blocco del worker senza fence del lettore, frame successivo, shutdown/reinit/failed init, copie concorrenti coerenti. Il probe verifica failed-frame retention su GXM, non ancora eseguito. |
| AVN-09 | [Protocollo batching](VITA_NATIVE_BATCHING_DECISION.md) e casi sintetici di padding/stato già eseguiti | Direct conservato. Decisione prestazionale aperta fino alle catture dello stesso gioco. |
| AVN-10 | [Contratto retirement](VITA_NATIVE_RETIREMENT_CONTRACT.md): risorsa/ultimo uso/prova, seriali/token/epoche, errori e matrice dei test | Finish globale conservato; nessuna notifica o reclamation nuova introdotta. |
| AVN-11 | [Contratto servizi Vita](VITA_PLATFORM_SERVICES_CONTRACT.md): target futuro, dipendenze, ownership, lifecycle, probe e cinque schede separate | Nessun target platform/launcher/core SDL completo aggiunto. Renderer e callback del gioco restano gli owner. |

## Profili e controlli

| Profilo | Async GX | Direct stream | Direct draw | Distinct CPU default | Immediate view default |
| --- | --- | --- | --- | --- | --- |
| `vita-gxm` | OFF nella cache verificata | OFF | OFF | OFF | OFF |
| `vita-gxm-control` | OFF | OFF | OFF | OFF | OFF |
| `vita-gxm-stable` | ON | ON | ON | OFF | OFF |
| `vita-gxm-candidate` | ON | ON | ON | ON | ON |

Nella cache generica `vita-gxm`, runtime logging era OFF; nei tre profili nuovi
è ON. Uniformare anche questa opzione prima di confrontare il profilo generico
con quelli nuovi; stable/control/candidate condividono gli altri default Aurora.

Le ultime due colonne scelgono il default dei campi BackendConfig: il port può
forzarli false indipendentemente. Il cap limita il prefisso degli helper, non
obbliga a usare quel numero di core. Con caller CPU2 e cap3 la nuova scelta è
caller + helper CPU1; CPU3 resta esclusa. Cap2 su quel caller diventa seriale.
Affinità/priorità, probe CPU3 e protocollo wake/done rimangono invariati.

Le statistiche e lo scratch sono presenti in tutti i profili. Per misurare il
loro beneficio separato serve il build del commit base con la stessa scena e
configurazione; il nuovo preset control non è un checkout del codice storico.
`GxmDisableFixedSnapshot` conserva il controllo senza riuso dei payload.

## Contratti conservati e costi rimossi

- Draw facade GXM: chiamata nativa ed errore, senza ricopiare i contatori dopo
  ogni draw. I valori osservati con una nuova lettura comprendono clear/copy e
  scena finale; i contatori cache restano quelli della facade. I finish cumulativi
  vengono conservati anche al reset per il nuovo frame, non azzerati fino al
  successivo finish/present.
- Direct candidato: geometria/metadati brevi e riferimenti agli uniform GPU
  tipati e ai binding. La conversione GPU avviene al cambio di revisione, non
  per draw identico. Nessun cast al prefisso CPU; la coda conserva proprietà.
- Fixed snapshot: costruzione del payload ancora eseguita; il contenitore non
  riceve emplace/pop per duplicati. Per payload nuovi resta una copia dallo
  scratch: il vantaggio del carico con molti snapshot unici va misurato.
- Budget: lettura di un valore pubblicato senza run_sync, non accesso alle mappe
  vive. Failed frame conserva il valore precedente; init/shutdown azzerano
  validità. Non è una completion GPU. Le due API pubblicate non sono una
  transazione atomica unica: confrontare frameIndex fra letture.
- FIFO ordine, snapshot fino a execute, buffer in-flight, shader reservation,
  CPU fallback, scissor pixel-exact e reset a ogni BeginScene restano i vincoli
  del percorso originale. Nessun riordino di draw e nessuna modifica al FIFO.

L'ABI C++ di BackendConfig cambia: ricompilare insieme port e libreria. I VPK
prodotti sono probe, non includono asset o il nuovo eseguibile del gioco.

## Verifiche eseguite

- Configure/build `vita-host-tests`: riusciti; **12/12 CTest passati**.
- Configure/build host separato con `-fsanitize=address,undefined` in Release:
  **12/12 CTest passati**, nessun errore sanitizer riportato.
- 726 check frontend, 10114 check submission e 3017 check regression senza
  fallimenti. CommandStream: 2048 draw, 20 frame, warm_allocations=0. Il test
  fixed conta separatamente le allocazioni dello storage dei duplicati host.
- VitaSDK GCC 15.2.0: build GXM dei quattro profili, due probe per profilo.
  Configurazioni nuove hanno usato i sorgenti pinned delle dipendenze già
  presenti, tramite FETCHCONTENT_SOURCE_DIR_XXHASH/ROBIN_HOOD, per lavorare offline.
- Audit su otto ELF/map nativi e corrispondenza SELF/eboot nei VPK: registrati nei
  log/manifest indicati sotto. Host/shim/build non provano scheduling o immagini
  reali su console. Il probe aggiunge wait diagnostici per ispezionare lo stato
  vivo senza sovrapporsi al worker: non usarli come pattern dell'overlay del gioco.

Log locali: `build/vita-native-validation-2026-10-02/host-tests.txt`,
`sanitizer-tests.txt`, i rispettivi `*-details.txt`, `binary-audits.txt` e
`draw-disassembly.txt`. I manifest sono `build/<profilo>/<nome_probe>-manifest.json`.
Sono output locali ignorati da Git; il codice dello strumento e i test sono
parte della modifica sorgente.

Il draw facade del GX probe candidato, Release/LTO, è stato ispezionato a
`0x81009878` (dimensione `182` byte). Dopo la chiamata al draw nativo
tramite vista, il codice verifica il risultato e aggiorna soltanto il flag
failed: le precedenti copie native di FrameStats non sono presenti. La
conversione del packet accodato copia ancora metadati brevi nella vista;
questa osservazione non misura uno speedup e non implica assenza di tutte le
copie nel frontend. Il disassemblato completo è nel log indicato sopra.

## Prove hardware ancora necessarie

Installare il probe candidato e quello stable, verificare hash read-back, log
contratti e immagini. Ripetere sul port del gioco stream→static→stream, EFB fra
draw, rollover, uniform/texture/TLUT/guest update, runtime feature change,
shader miss/fallback, present scartato e PS button suspend/resume. Registrare
stessa scena/input, clock, risoluzione e cache; confrontare separatamente ciascun
campo nuovo. Nel candidato il probe grafico tiene i worker CPU a zero: il pool
ha test shim, ma la sua misura hardware richiede il port o un job CPU dedicato.

Non dichiarare automaticamente stabili dispatch/vista né attivare il batching
sulla base di host pass o minor numero di chiamate. I protocolli AVN-09/10 e le
schede VPS del contratto servizi indicano il successivo sviluppo assegnabile.

## Artefatti

Gli hash sotto identificano SELF ed eboot.bin corrispondente. Titolo/scena/clock/
risoluzione/cache/hash installato restano `unknown` nei manifest senza cattura.

| Profilo | Probe | SELF / eboot SHA256 |
| --- | --- | --- |
| `vita-gxm` | `aurora_vita_gxm_probe` | `4a4f5f316144b76064151d0c33f11a08645ad103f4a2936865e7d08533849ad0` |
| `vita-gxm` | `aurora_vita_gx_probe` | `c91cf74772674ae196b997d62c90d91a55f2899bc3e76b99fee5159f344e7209` |
| `vita-gxm-stable` | `aurora_vita_gxm_probe` | `3bff8ccc6e59c4e27811ad71df6e68e110bce7dd2f7f50d996358d98a1c1bd3e` |
| `vita-gxm-stable` | `aurora_vita_gx_probe` | `15fc04b91ff841a1a18eed566019fd3ff46df46fc6407c5d0a6e584b42bbc11a` |
| `vita-gxm-control` | `aurora_vita_gxm_probe` | `3bff8ccc6e59c4e27811ad71df6e68e110bce7dd2f7f50d996358d98a1c1bd3e` |
| `vita-gxm-control` | `aurora_vita_gx_probe` | `48121abe977347fef4ba60a56ce9719f901a133f26f6f16acb81f23acbcbc7ce` |
| `vita-gxm-candidate` | `aurora_vita_gxm_probe` | `3bff8ccc6e59c4e27811ad71df6e68e110bce7dd2f7f50d996358d98a1c1bd3e` |
| `vita-gxm-candidate` | `aurora_vita_gx_probe` | `fd3441b8e480a23a25c0b70546672aa961ee820cabbb60b9ef2df71c10281969` |
