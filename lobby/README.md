# Lista stanze online di KokakuNative

Questo è il piccolo servizio che tiene l'elenco delle stanze multiplayer visibili da Internet. Gira su **Cloudflare Workers**, nel piano gratuito, senza server da mantenere.

Fa due sole cose:

1. elenca le stanze aperte (nome, regole, numero di giocatori);
2. quando un giocatore entra in una stanza, comunica agli altri membri gli indirizzi a cui provare il collegamento diretto.

Il traffico di gioco **non passa mai da qui**: i giocatori si collegano direttamente fra loro. Per questo il servizio consuma pochissimo e resta nel piano gratuito, ma per lo stesso motivo non può aiutare due giocatori i cui router rifiutano il collegamento diretto (vedi "Limiti").

**Stato attuale:** pubblicato il 15 novembre 2026 su `https://kokaku-lobby.masterfirefoxdani.workers.dev`, che è anche l'indirizzo predefinito dentro il gioco.

## Pubblicarlo

Serve un account Cloudflare gratuito e Node.js 20 o successivo. Gli stessi comandi servono per ripubblicare dopo una modifica o per metterlo su un altro account.

```powershell
cd lobby
npm install
npx wrangler login      # apre il browser: accedi e autorizza
npx wrangler deploy
```

Su un account nuovo la prima pubblicazione fallisce con "You need a workers.dev subdomain": basta aprire una volta **Compute → Workers & Pages** nella dashboard di Cloudflare, che crea il sottodominio, e ripetere `npx wrangler deploy`. Il certificato HTTPS del nuovo indirizzo può richiedere qualche minuto.

Al termine `wrangler` stampa l'indirizzo pubblico, del tipo:

```
https://kokaku-lobby.TUO-SOTTODOMINIO.workers.dev
```

Aprilo nel browser: deve rispondere `KokakuNative lobby, protocol 1`. Aggiungendo `/rooms` si vede l'elenco delle stanze in formato JSON.

Il gioco usa l'indirizzo scritto nella costante `default_lobby_url` in `tools/PSPRecomp/profiles/kokaku/host/kokaku_net.cpp`; vale per tutti, anche per chi ha un INI vecchio senza la riga `Lobby=`. Se l'indirizzo cambia va aggiornata quella costante e ricompilato il gioco. Un singolo giocatore può puntare a un altro servizio dal proprio `KokakuNative.ini`:

```ini
[Network]
Lobby=https://kokaku-lobby.TUO-SOTTODOMINIO.workers.dev
```

## Verificarlo

Con il servizio in esecuzione in locale:

```powershell
npm run dev      # in una finestra: avvia il servizio su http://127.0.0.1:8787
npm test         # in un'altra: 50 controlli del protocollo (47 contro un servizio pubblicato)
```

Contro il servizio pubblicato:

```powershell
$env:LOBBY_URL = "https://kokaku-lobby.TUO-SOTTODOMINIO.workers.dev"
npm test
```

Anche il gioco ha una prova che usa lo stesso client del multiplayer:

```powershell
tools\PSPRecomp\build\bin\KokakuNative.exe --test-lobby-live https://kokaku-lobby.TUO-SOTTODOMINIO.workers.dev 45
```

Il numero finale è una pausa in secondi: verifica che una connessione inattiva resti aperta con i soli ping.

## Costi e limiti del piano gratuito

Ogni giocatore tiene aperta una sola connessione WebSocket mentre è nel menu multiplayer o in una stanza. I messaggi sono pochi (elenco ogni 3 secondi mentre la lista è aperta, un ping ogni 20 secondi) e l'oggetto che li gestisce va in pausa quando non c'è nulla da fare.

Il piano gratuito di Cloudflare concede 100.000 richieste al giorno; ogni connessione ne consuma una e venti messaggi ne valgono un'altra. Una serata con decine di giocatori resta molto lontana dal limite. Se venisse superato, la lista smette di rispondere fino alla mezzanotte UTC; le partite già avviate continuano, perché non dipendono dal servizio.

Limiti interni, modificabili in testa a `src/index.js`: 200 stanze, 1000 connessioni, 16 connessioni dallo stesso indirizzo, 40 messaggi di raffica per connessione.

## Limiti del collegamento diretto

Senza un server che inoltri il traffico, due giocatori si collegano solo se i loro router lo permettono. Funziona nella maggior parte delle connessioni di casa. Può non funzionare quando almeno uno dei due è dietro un NAT "stretto" (alcune reti mobili, alcune connessioni FWA, reti aziendali o universitarie): in quel caso il gioco avvisa che non riesce a raggiungere chi ospita.

Se chi è in difficoltà riesce comunque a raggiungere chi ospita, ma non un altro giocatore della stessa stanza, è chi ospita a inoltrare i pacchetti fra quei due.

## Riservatezza

- L'elenco pubblico non contiene indirizzi IP.
- Chi entra in una stanza riceve gli indirizzi degli altri membri (pubblico e di rete locale), come in ogni gioco con collegamento diretto. Vale anche al contrario.
- Il servizio non salva nulla su disco: quando una connessione si chiude, i suoi dati spariscono.
- Cloudflare, come ogni fornitore, vede gli indirizzi di chi si collega.

## Aggiornarlo o spegnerlo

Dopo una modifica a `src/index.js`: `npx wrangler deploy`. Le stanze aperte in quel momento spariscono dall'elenco per qualche secondo e si ripubblicano da sole; le partite in corso non se ne accorgono.

Per spegnerlo: `npx wrangler delete`. Il gioco continua a funzionare in rete locale.

## Protocollo (versione 1)

WebSocket su `/ws`, messaggi JSON con campo `t`.

| Dal gioco | Significato |
| --- | --- |
| `hello` | versione, versione del netcode, MAC virtuale, nome |
| `cand` | indirizzi a cui farsi raggiungere |
| `host` | crea o aggiorna la propria stanza (id, chiave, nome, regole, stato) |
| `unhost` | chiude la propria stanza |
| `list` | chiede l'elenco |
| `join` | entra in una stanza; con la chiave si rientra anche a partita avviata |
| `leave` | esce dalla stanza |
| `ping` | testo semplice, risposta `pong` |

| Dal servizio | Significato |
| --- | --- |
| `welcome` | connessione accettata, indirizzo pubblico visto dal servizio |
| `rooms` | elenco delle stanze della stessa versione del netcode |
| `hosted` | stanza registrata |
| `joined` | chiave della stanza e membri presenti con i loro indirizzi |
| `join_failed` | `gone`, `full`, `playing`, `closed`, `version` |
| `peer` | un membro è entrato o ha cambiato indirizzi |
| `gone` | un membro è uscito |
| `closed` | la stanza non è più in elenco |
| `error` | `version`, `hello`, `bad`, `rate`, `taken`, `full` |

Gli indirizzi accettati sono quelli di rete privata, quello da cui proviene la connessione e al massimo altri due pubblici (quelli che STUN può legittimamente riferire).
