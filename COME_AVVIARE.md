# KokakuNative: avvio e impostazioni

Aggiornamento dell’11 settembre 2026. Avvia **PLAY_KOKAKU.cmd**. La build include la modalita **60 FPS**, le voci ATRAC dal flusso del gioco, la correzione dei modelli neri nel briefing e le impostazioni PC nella schermata originale OPTION. Introduzione, briefing e ingresso nella prima missione sono raggiungibili; una missione completa e tutti i dialoghi della campagna non sono ancora stati verificati.

## Comandi e impostazioni

Aggiornamento del 12 settembre: le indicazioni dei comandi usano icone pixel art per tastiera, pulsanti del mouse e rotella. Il menu principale, OPTION, i menu con selezione, la pausa e il multiplayer mostrano la barra di navigazione; i salvataggi hanno suggerimenti dedicati. Le icone seguono le assegnazioni salvate, compresi i tasti alternativi quando manca il principale. L'anteprima Type A distingue i comandi di gioco da quelli di navigazione. Il pannello PC e i suoi messaggi sono ora interamente in inglese, anche su Windows in italiano.

Dal menu entra in **OPTION → KeyAssign** e premi **Z**. F2 non apre più una finestra esterna.

- **Tab** cambia pagina: tastiera/mouse, mouse, video/audio PC, **Gameplay**, **Network** (stanze online e nome del giocatore, vedi Multiplayer). Funziona anche l'azione menu Quadrato (**C** per impostazione predefinita).
- **Su/Giu** selezionano l'azione; **Sinistra/Destra** scelgono il tasto principale o alternativo. **Z** avvia la cattura: premi un tasto, un pulsante del mouse o gira la rotella. **Esc** annulla la cattura; **Canc** rimuove l'assegnazione selezionata.
- Nelle altre pagine **Sinistra/Destra** cambiano il valore. Il ripristino dei comandi richiede **Z**.
- **X** torna alla navigazione originale. Nel pannello PC funzionano anche Invio per confermare ed Esc per tornare indietro.

I comandi vengono salvati subito in `tools/PSPRecomp/build/bin/KokakuControls.ini`. Le assegnazioni in conflitto vengono scambiate. Sensibilita e inversione del mouse sono immediate. Le impostazioni video e il volume generale vengono salvati in `KokakuNative.ini` e applicati **al prossimo avvio**, come indicato nella schermata. La compilazione conserva la configurazione esistente.

Le colonne **PRIMARY** e **ALT** mostrano entrambe le assegnazioni. La casella evidenziata indica quella modificata; sotto compare il nome completo del tasto. **Enter**, **Esc**, **Tab**, frecce e **Delete** restano scorciatoie fisse nel pannello PC, utilizzabili anche dopo aver rimosso le assegnazioni di navigazione. Verifiche e catture: [diagnostics/PIXEL_CONTROLS.md](diagnostics/PIXEL_CONTROLS.md).

Profili selezionabili: **1920x1080 / MSAA 4**, **1920x1080 / MSAA 2**, **1280x720 / MSAA 2**, **480x272 / MSAA 1**. Sono presenti anche schermo intero, contatore FPS, volume generale e **Cadenza: 30/60 FPS**; il menu originale **Sound** conserva i controlli audio del gioco. Video, volume e cadenza richiedono il riavvio.

La configurazione corrente usa **1080p, MSAA 2 e 60 FPS**. Nella prova reale all'inizio della prima missione sono stati misurati **58,95 FPS medi**, con il 95% degli intervalli fra fotogrammi entro **20,58 ms**. Non sono 60 FPS costanti su tutta la campagna: il tempo simulato nel tratto misurato e circa il 98,2% del tempo reale. Il contatore mostra i fotogrammi prodotti, senza contare le presentazioni ripetute dello stesso buffer.

La patch viene riapplicata quando il gioco ricarica il modulo principale all'ingresso della missione. Le attese usano un timer Windows preciso; la cache delle texture considera soltanto la banca colori raggiungibile dall'immagine, evitando migliaia di decodifiche inutili. La modalita grafica asincrona resta disattivata: nella prova si e bloccata all'avvio. Misure e limiti: `diagnostics/FPS_VALIDATION.md` e `diagnostics/fps-validation-results.json`.

La modalita **30 FPS** e stata verificata separatamente: **30,01 FPS medi**, con tempo simulato al 100,02% del tempo reale nel tratto misurato. Il limite sui fotogrammi completati evita l'accelerazione dovuta alle attese VBlank di piu thread.

## Giocare

**Bilanciamento PC leggero (attivo di default):** in **OPTION → KeyAssign**, entra nel pannello con Z/Invio e premi **Tab tre volte** per raggiungere **Gameplay → PC balance**. Invio o Sinistra/Destra alternano **On/Off**. La scelta viene salvata subito in `[Gameplay] PCBalance=true/false` nel `KokakuNative.ini` accanto all'eseguibile (o nel percorso configurato con `PSPRECOMP_CONFIG`) e si applica al prossimo caricamento di missione/area, senza riavviare il programma.

Con On, gli HP massimi del personaggio in campagna sono il **40% del valore originale**, arrotondato all'intero piu vicino: **250 → 100 HP**. I danni degli avversari al giocatore aumentano del **60%**, mentre le pause fra gli attacchi dei soldati si accorciano del **30%**. Ogni personaggio conserva la propria base; le cure rispettano il massimo ridotto e la vita trasportata fra aree viene soltanto limitata al nuovo massimo. Il Tachikoma alleato infligge il **5% del danno originale quando non lo stai pilotando**; a bordo mantiene il danno originale. I soldati con AI di pattuglia e assalto si spostano lateralmente piu spesso durante il combattimento, anche mentre sparano. Con Off tornano HP, danni, pause e movimenti originali al prossimo caricamento. **In multiplayer tutti i modificatori sono sempre esclusi, anche se il toggle resta On per la campagna**. Verifiche e limiti: [diagnostics/PC_BALANCE.md](diagnostics/PC_BALANCE.md).

Al titolo premi **Invio**, naviga con le frecce e conferma con **Z**. Nei menu **X** torna indietro. **F1** cattura/libera il mouse per la visuale; **Esc** apre la pausa durante la partita. Alt+F4 chiude la finestra.

Predefiniti: WASD movimento, Shift cammina, Spazio/Q salto, mouse sinistro attacco, E/mouse destro interazione, rotella cambio arma. La visuale usa gli spostamenti relativi del mouse. Le azioni corrispondono allo schema PSP Type A. Il controller XInput resta supportato; la verifica con un controller fisico e ancora da fare.

Il cambio arma da tastiera/mouse usa **rotella giu = arma successiva**, **rotella su = arma precedente**. Il carosello originale compare mentre scorri, evidenzia l'arma selezionata e si chiude automaticamente dopo il cambio. Ogni scatto seleziona ed equipaggia l'arma con l'animazione originale; non occorre premere E o confermare. Gli scatti durante l'animazione vengono conservati per il cambio successivo. Le azioni **Previous weapon** e **Next weapon** restano rimappabili in OPTION → KeyAssign, anche su pulsanti del mouse o tasti alternativi. Il pad mantiene la procedura originale PSP.

Tenendo premuto **Tab** in missione, la scheda comandi mostra le assegnazioni effettive del profilo. Con mouse catturato (F1), i predefiniti sono **Spazio: View Change**, **mouse sinistro: Objective**, **E: Optical Camouflage**, **I: Map**, **J: Fire Order**, **L: Move Order**, **K: Point Order**. Premi il tasto indicato mentre tieni Tab. Anche le rimappature e i pulsanti secondari senza un primario vengono mostrati correttamente. Con mouse libero la scheda segue le assegnazioni dei menu. Evidenze: [diagnostics/MISSION_COMMANDS.md](diagnostics/MISSION_COMMANDS.md).

**R ricarica**, **F attiva il lock-on**. La ricarica usa le munizioni di riserva e l'animazione del gioco; a caricatore pieno non consuma riserve. Le voci **Reload** e **Lock-on** sono separate nelle Options e personalizzabili. I vecchi profili con la mappatura predefinita R → D-pad down vengono aggiornati in memoria automaticamente, senza richiedere un reset delle altre assegnazioni.

La **rotella cambia anche le armi del Tachikoma**: usa l'inventario del veicolo controllato, mostra il suo carosello e conferma automaticamente. Gli slot vuoti vengono saltati; per avere alternative occorre installare piu armi nel menu **Tachikoma** del briefing. Verificati nella prima missione il passaggio lanciarazzi → mitragliatrice → lanciarazzi, lo scorrimento inverso e la chiusura del carosello, con confronto della build precedente. Evidenze: `diagnostics/TACHIKOMA_WEAPONS.md`.

La visuale del mouse segue anche il **Tachikoma** quando entri con E, con la stessa sensibilita e gli stessi limiti verticali del gioco. Uscendo, il controllo torna al personaggio. La correzione riguarda il percorso comune della visuale: verificati ingresso, rotazione orizzontale/verticale e uscita nella prima missione, con confronto della build precedente. Evidenze e prova ripetibile: `diagnostics/TACHIKOMA_MOUSE.md`.

**Tachikoma a 60 FPS (14 novembre 2026):** a bordo il mezzo ora accelera e raggiunge la velocità massima come a 30 FPS. L'inerzia del Tachikoma applicava l'attrito a ogni fotogramma: a 60 FPS la velocità massima scendeva da 650 a circa 541 e l'accelerazione era più lenta, quindi il movimento rispondeva in ritardo rispetto ai comandi. Il personaggio a piedi non ha inerzia e non era interessato; la modalità 30 FPS resta identica. Verificato nella prima missione con prove automatiche a 30 e 60 FPS, anche a tempo reale; manca una prova manuale con tastiera e mouse. Evidenze e prova ripetibile: [diagnostics/TACHIKOMA_INERTIA.md](diagnostics/TACHIKOMA_INERTIA.md).

## Salvataggi

**Save Game → Save Data** apre cinque slot nella finestra del gioco. Usa frecce e Z; uno slot occupato richiede una seconda conferma prima di essere sostituito. **Load Game** apre la stessa selezione e permette di caricare solo gli slot presenti. X annulla.

I dati sono accanto all'eseguibile:

- Slot 1: `tools/PSPRecomp/build/bin/SAVEDATA/ULUS10020/`, compatibile con il salvataggio precedente.
- Slot 2–5: cartelle `ULUS10020PC2` fino a `ULUS10020PC5`.

Il backend conserva DATA.BIN e le immagini/animazione fornite dal gioco. Ogni file viene scritto tramite un temporaneo e poi sostituito. Non e una transazione unica dell'intera cartella e non costituisce ancora un'esportazione completa per PSP/PPSSPP (manca PARAM.SFO).

La prova dal gioco ha scritto lo slot 2 e lo ha ricaricato con successo in un secondo processo. I test usano esclusivamente `diagnostics/native-slot-data/`, senza utilizzare i salvataggi personali. Il ripristino di progressi avanzati di missione resta da verificare.

## Audio e grafica

Le voci erano escluse perche il decoder cercava un file autonomo e identificava male alcuni blocchi ATRAC3plus. Ora riceve i blocchi forniti dal gioco, inclusi quelli mono delle voci, e li converte nell'uscita stereo. Sono verificati flussi reali mono/stereo, decodifica a blocchi, PCM non silenzioso e flussi musica/voce contemporanei. Non sono stati ascoltati e verificati tutti i dialoghi della campagna.

I modelli neri nel briefing dipendevano da parametri di nebbia non finiti: lo shader ora applica lo stesso comportamento del renderer CPU. Il Tachikoma risulta colorato e illuminato nella cattura finale `diagnostics/fps-final-briefing.png`, mantenendo trasformazione hardware e MSAA. La correzione e stata ricontrollata dopo l'ottimizzazione della cache.

## Multiplayer

**Aggiornamento del 15 novembre 2026: stanze online.** Chi sceglie **Create** apre una stanza; chi sceglie **Join** vede l'elenco delle stanze aperte, su Internet e sulla propria rete locale, e sceglie in quale entrare. I giocatori si collegano direttamente fra loro: nessun server inoltra la partita. Una stanza ospita fino a 6 giocatori, come nel gioco originale.

Tutti devono usare **questa stessa versione**: le build precedenti al 15 novembre usano un formato di rete diverso e non si vedono con questa. PSP e PPSSPP non sono supportati.

### Ospitare una partita

1. Avvia **PLAY_KOKAKU.cmd**, entra in **Multi Play** e scegli **Create**.
2. Imposta le regole (modalità, mappa, tempo, frag, fuoco amico, Lock On, rotazione mappe) e conferma.
3. Sei nella stanza. In alto a destra una riga dice se la stanza è pubblicata: **Listed online 1/6**, oppure **Online list unreachable** se la lista Internet non risponde (in quel caso la stanza resta visibile solo in rete locale).
4. Quando gli altri sono entrati scegli **Start**. A partita avviata la stanza resta in elenco come **IN GAME** e non accetta nuovi ingressi.

### Entrare in una partita

1. **Multi Play → Join** apre la schermata **ONLINE ROOMS**.
2. Ogni riga è una stanza: nome, mappa, modalità, giocatori presenti e un'etichetta: **NET** (trovata su Internet), **LAN** (sulla tua rete), **IN GAME** (partita già avviata, non si può entrare). In basso compaiono le regole complete della stanza evidenziata.
3. Scegli con le frecce e conferma. I tasti sono quelli indicati in fondo alla schermata; con i comandi predefiniti: frecce, **Z** per entrare, **V** per aggiornare l'elenco, **X** per tornare indietro.
4. Dopo "Connecting..." il gioco mostra la sua schermata originale **Game room is found**: conferma, poi **Next** e **Ready**.

L'elenco si aggiorna da solo ogni 3 secondi. Se è vuoto, nessuno sta ospitando in quel momento.

### Fine partita

Con **Map Rot ON** a fine round compare la schermata Result e, quando tutti i giocatori risultano pronti, la partita prosegue sulla mappa successiva con gli stessi giocatori. Con **Map Rot OFF** la schermata Result ha il tasto **Exit**, che riporta al menu Multi Play: la stanza si chiude e per rigiocare chi ospita ne crea una nuova.

### Nome e interruttore

In **OPTION → KeyAssign → Z**, premi **Tab** fino alla pagina **NETWORK**:

- **Online rooms: On/Off.** Con Off torna il comportamento originale: Join entra nella prima stanza trovata in rete locale, senza elenco.
- **Player name.** È il nome con cui compare la tua stanza ("Motoko's room"). Premi Invio, scrivi (lettere, cifre, spazio, trattino; massimo 16 caratteri) e conferma con Invio. Finché non lo scegli il gioco usa un nome automatico del tipo `Agent-1234`.

Entrambe le voci valgono dal prossimo avvio.

### Giocare via Internet: cosa aspettarsi

L'elenco delle stanze è tenuto da un piccolo servizio gratuito su Cloudflare (`https://kokaku-lobby.masterfirefoxdani.workers.dev`, sorgente e istruzioni in [lobby/README.md](lobby/README.md)). Il servizio presenta i giocatori fra loro; la partita viaggia direttamente da un PC all'altro.

Nella schermata delle stanze, la riga in basso descrive la tua connessione:

- **open** o **good**: il collegamento diretto funziona con quasi tutti;
- **strict NAT**: il tuo router (o il tuo operatore, tipico di reti mobili e di alcune connessioni FWA) cambia porta a ogni destinazione. Riesci a entrare nelle stanze di chi ha una connessione aperta, ma non in tutte.

Se compare **Could not reach the host**, i due router non permettono il collegamento diretto. Rimedi, in ordine di semplicità:

1. invertite i ruoli: ospita chi dei due ha la connessione migliore;
2. chi ospita apre sul proprio router la porta **UDP 3658** verso il proprio PC (port forwarding);
3. provate da un'altra rete.

Non esiste un server che inoltri la partita quando il collegamento diretto è impossibile. Fa eccezione un caso: se un giocatore raggiunge chi ospita ma non un altro membro della stanza, è chi ospita a inoltrare i pacchetti fra quei due.

**Se qualcosa non funziona**, chiudi il gioco e guarda `KokakuNative.log`, nella cartella del gioco accanto all'eseguibile. Le righe che iniziano con `online:` raccontano i passaggi: lista raggiunta oppure no (e perché), tipo di router rilevato, con chi è riuscito il collegamento diretto, perché un ingresso è fallito. Il file viene riscritto a ogni avvio, quindi copialo prima di riaprire il gioco; per capire un collegamento mancato servono i file di entrambi i giocatori. Gli indirizzi Internet vi compaiono solo per metà (`93.41.x.x`), quelli della rete di casa per intero.

Alla prima partita Windows può chiedere il permesso di accesso alla rete per KokakuNative: consentilo.

**Riservatezza.** L'elenco pubblico mostra nome della stanza, regole e numero di giocatori, mai indirizzi. Chi entra nella stessa stanza, invece, vede l'indirizzo IP degli altri membri, come in ogni gioco con collegamento diretto.

### Rete locale e stesso PC

Le stanze create sulla stessa rete compaiono nell'elenco con l'etichetta **LAN** anche senza Internet. Gli annunci usano il broadcast UDP sulla porta 50050: reti ospiti con isolamento dei dispositivi possono bloccarlo.

Per provare due istanze sullo **stesso PC**, avvia la seconda con **PLAY_KOKAKU_PLAYER2.cmd**: usa porte con offset +1000 e un'identità diversa. Tastiera e mouse controllano la finestra attiva; questa prova locale non offre due postazioni di input indipendenti.

In Uptown, **DECODING...** compare durante la preparazione con conto alla rovescia da **01:00** e scompare al passaggio al round. Non va interpretata come un errore del decoder audio/video.

I log dettagliati dell'allocatore e dei thread sono disattivati durante l'uso normale. Per la diagnostica si possono riattivare con `PSPRECOMP_PARTITION_DIAG=1` e `PSPRECOMP_THREAD_DIAG=1`.

### Configurazione

Sezione `[Network]` di `KokakuNative.ini`. Le righe mancanti valgono come qui sotto; un INI precedente continua a funzionare senza modifiche.

```ini
[Network]
Enabled=true
Online=true
Nickname=MajorKusanagi
Lobby=
MAC=auto
Port=3658
PortOffset=0
```

- `Online=false`: comportamento originale in rete locale, senza elenco.
- `Nickname`: il nome scelto nella pagina NETWORK. Il valore di fabbrica `MajorKusanagi` viene sostituito da un `Agent-1234` automatico.
- `Lobby`: vuoto usa il servizio predefinito; un altro indirizzo `https://...` usa un servizio diverso; `off` disattiva la lista Internet e lascia solo la rete locale.
- `Port`: porta UDP del traffico di gioco (quella da inoltrare sul router). `PortOffset` la sposta per una seconda istanza sullo stesso PC.
- `MAC=auto` genera un'identità distinta a ogni avvio. `Enabled=false` spegne del tutto la rete del gioco.

**Limiti della verifica.** Il percorso completo (pubblicare la stanza, trovarla nell'elenco, entrare, lobby, partita) è stato provato con due istanze del gioco sullo stesso PC attraverso il servizio Cloudflare pubblicato. Con una copia locale dello stesso servizio sono state provate anche tre istanze e una partita da 5 minuti fino alla schermata Result e alla mappa successiva; via rete locale, la modalità a 30 FPS. **Il collegamento fra due connessioni Internet diverse non è stato provato con router reali**, solo con router simulati nei test automatici. Restano da verificare anche più di tre giocatori, danni e uccisioni fra giocatori remoti e il gioco su reti con latenza elevata. Evidenze e comandi: [diagnostics/online/VALIDATION.md](diagnostics/online/VALIDATION.md); per le misure di latenza in locale: [diagnostics/multiplayer/VALIDATION.md](diagnostics/multiplayer/VALIDATION.md).

## Verifiche riproducibili

Build: `tools/w64devkit/bin/cmake.exe --build tools/PSPRecomp/build --target KokakuNative -j 4`, con `tools/w64devkit/bin` nel PATH. Test: `tools/w64devkit/bin/ctest.exe --test-dir tools/PSPRecomp/build --output-on-failure`.

`diagnostics/test-original-options.ps1` raggiunge KeyAssign e cattura la schermata; `-Page 2` seleziona la pagina video. `diagnostics/test-native-save-slots.ps1` salva nello slot 2 isolato; `-Load` lo ricarica, `-Inspect` mostra la selezione senza eseguire operazioni. Una prova con tastiera reale ha verificato rimappatura, profilo video e passaggio 60 → 30 → 60 con persistenza, usando configurazioni diagnostiche separate. Tutti i **20 test CTest** passano (inclusi `kokaku_net_tests` con chiamate HLE PDP reali, `kokaku_transport_tests` con i router simulati, `kokaku_lobby_tests` e `kokaku_online_tests`); sono ripetute con successo anche le prove ATRAC con flussi mono/stereo del gioco e il salvataggio/caricamento dello slot 2 nella build aggiornata.

Restano da verificare: campagna completa, tutti i dialoghi e suoni, ripristino dei progressi avanzati, e controller fisico.
