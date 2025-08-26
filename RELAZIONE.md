# Relazione Tecnica - Progetto SO_Finale
## Simulazione di Ufficio Postale con Processi Concorrenti

**Autore:** [Nome Studente]  
**Corso:** Sistemi Operativi  
**Anno Accademico:** 2024/2025  
**Data:** 25 Agosto 2025

---

## Indice

1. [Visione Generale](#1-visione-generale)
2. [Architettura del Sistema](#2-architettura-del-sistema)
3. [Configurazioni di Test](#3-configurazioni-di-test)
4. [Strutture Dati](#4-strutture-dati)
5. [Meccanismi di Sincronizzazione](#5-meccanismi-di-sincronizzazione)
6. [Processi e Funzionalità](#6-processi-e-funzionalità)
7. [Gestione degli Errori](#7-gestione-degli-errori)
8. [Compilazione ed Esecuzione](#8-compilazione-ed-esecuzione)
9. [Testing e Validazione](#9-testing-e-validazione)
10. [Conclusioni](#10-conclusioni)

---

## 1. Visione Generale

### 1.1 Compilazione e avvio:
Abbiamo due opzioni per compilare e avviare: una è completamente automatica mentre l'altra richiede compilazione e avvio manuale (specificando quale configurazione .conf usare. 
Compilazione e avvio automatico: in un terminale nella cartella con tutti i file presenti scriviamo il comando make run-timeout oppure make run-explode. I due comandi compileranno il file di configurazione specificato ed eseguiranno il processo direttore con dei dati specifici. L'esecuzione di make run eseguirà il default (ovvero terminazione per timeout).
Compilazione manuale: eseguiamo semplicemente make all. Successivamente, eseguiamo ./direttore seguito da explode o timeout.

### 1.2 Inizializzazione del Sistema: L'Avvio dell'Ecosistema

#### 1.2.1 Bootstrap del Sistema

Quando viene eseguito il comando `./direttore`, si avvia una sequenza orchestrata di inizializzazione che trasforma il sistema da un insieme di file separati in un ecosistema digitale funzionante. Il **processo direttore** funge da coordinatore centrale, responsabile di:

- **Creazione delle risorse IPC** (memoria condivisa, semafori, code di messaggi)
- **Avvio di tutti i processi** in un ordine specifico per evitare race conditions
- **Sincronizzazione globale** per garantire che tutti i componenti siano pronti prima dell'inizio

#### 1.2.2 Infrastruttura di Comunicazione

Il sistema stabilisce un'infrastruttura di comunicazione robusta basata su tre pilastri fondamentali:

**Memoria Condivisa:** Il "cervello" del sistema che contiene tutte le informazioni condivise tra processi:
- Stato degli sportelli e informazioni degli operatori
- Code dei ticket per ogni servizio 
- Statistiche in tempo reale
- Flag di controllo per coordinare le fasi della simulazione

**Sistema di Semafori:** Un'architettura multi-livello con 14 semafori specializzati per gestire l'accesso concorrente alle risorse critiche, dalla sincronizzazione generale a quella specifica per ogni tipo di servizio.

**Code di Messaggi:** Canale di comunicazione asincrona per le richieste di ticket tra utenti e sistema centrale.

#### 1.2.3 Sequenza di Avvio Controllata

Il sistema segue una sequenza di avvio ben definita per garantire stabilità e coerenza, sia di "stampa" che di organizzazione dei processi:

1. **Inizializzazione delle risorse IPC** e azzeramento delle strutture dati
2. **Creazione del processo ticket** (deve essere pronto a ricevere richieste)
3. **Avvio degli operatori** (devono registrarsi e cercare sportelli)
4. **Creazione degli utenti** (possono iniziare a fare richieste)
5. **Semaforo verde per tutti i processi creati**

Ogni fase include pause strategiche per assicurare che l'inizializzazione sia completa prima di procedere al passo successivo.

#### 1.2.4 Auto-Registrazione dei Processi

Ogni processo creato si **auto-registra** nella memoria condivisa e stabilisce le proprie connessioni alle risorse IPC:

- **Gli operatori** si assegnano un servizio casuale e si registrano come disponibili
- **Il processo ticket** inizializza le code per ogni servizio 
- **Gli utenti** si collegano per poter inviare richieste e ricevere notifiche

#### 1.2.5 Meccanismo di Sincronizzazione Globale

Prima che inizi effettivamente la simulazione, il sistema utilizza una **barriera di sincronizzazione** (il semaforo verde nel punto 5) per garantire che tutti i processi siano completamente inizializzati e pronti. Solo quando tutti i componenti (utenti, operatori, processo ticket) hanno segnalato la loro prontezza, il direttore avvia la prima giornata lavorativa.

Con questo "semaforo" di avvio, creiamo **equità tra i processi**, evitando di far iniziare i processi inviando segnali singolarmente, che creerebbe del ritardo sull'ultimo utente avvisato (lavorando nell'ordine dei nanosecondi e millisecondi non possiamo permettercelo).

Questa architettura garantisce che il sistema sia **deterministico** nell'inizializzazione ma **flessibile** nell'esecuzione, permettendo comportamenti emergenti realistici pur mantenendo la robustezza necessaria per una simulazione affidabile.

### 1.3 Simulazione di una giornata

Una volta completata l'inizializzazione del sistema, per ogni giornata il direttore segue una sequenza precisa:

1. **Inizializzazione della giornata** (configurazione sportelli e reset contatori)
2. **Invio segnale SIGUSR1** a tutti i processi: è l'equivalente digitale del "si apre l'ufficio!"
3. **Attivazione flag day_in_progress** nella memoria condivisa
4. **Rilascio della barriera semaforo** per sincronizzazione globale

Questo momento rappresenta una **sincronizzazione globale** perfettamente orchestrata: tutti i processi ricevono prima la notifica di inizio giornata (SIGUSR1), poi attendono tutti insieme sulla barriera semaforo prima di iniziare effettivamente a lavorare. Questo garantisce **equità temporale** - nessun processo inizia prima degli altri, eliminando vantaggi dovuti al timing di invio dei segnali.

Quando avviamo `./direttore`, assistiamo alla nascita di un ecosistema digitale che ricrea fedelmente la complessità di un ufficio postale moderno. Il **processo direttore** agisce come il manager dell'ufficio: coordina tutto, dall'apertura mattutina alla chiusura serale, raccogliendo statistiche e garantendo che ogni ingranaggio funzioni perfettamente.

Il primo atto è la **creazione delle risorse**: la memoria condivisa diventa il "cervello" del sistema, contenendo tutte le informazioni condivise tra i processi. I semafori fungono da "semafori digitali" per coordinare l'accesso alle risorse critiche, mentre le code di messaggi permettono la comunicazione asincrona tra utenti e sistema di ticket.

### 1.3.2 L'Apertura dell'Ufficio: Un Balletto Sincronizzato

```
🏢 Ore 8:00 - APERTURA UFFICIO POSTALE
```

Il direttore **invia il segnale SIGUSR1** a tutti i processi: è l'equivalente digitale del "si apre l'ufficio!". Questo momento rappresenta una **sincronizzazione globale** perfettamente orchestrata:

- **Gli operatori** si precipitano a cercare uno sportello libero compatibile con il loro servizio
- **Il processo ticket** si attiva e inizia ad ascoltare le richieste in arrivo
- **Gli utenti** iniziano a pianificare le loro visite, alcuni decidendo di andare subito, altri di aspettare

## 2. Analisi dei Processi: Esecuzione e Comportamenti

### 2.0 Esecuzione del codice di Direttore

Il **processo direttore** funge da orchestratore centrale dell'intera simulazione, gestendo il ciclo di vita del sistema e coordinando tutti gli altri processi attraverso una sequenza di operazioni precise e temporizzate.

**Inizializzazione Sistema:**
Il direttore inizia creando tutte le risorse IPC necessarie (memoria condivisa, semafori, code di messaggi) e successivamente genera i processi specializzati in ordine specifico: prima il processo ticket, poi gli operatori, infine gli utenti. Ogni creazione è seguita da una pausa di sincronizzazione per garantire che i processi si registrino correttamente nella memoria condivisa.

**Gestione Giornaliera:**
Per ogni giornata di simulazione, il direttore esegue una sequenza ritualizzata: configura casualmente i servizi degli sportelli, invia il segnale SIGUSR1 a tutti i processi per notificare l'apertura, attiva il flag `day_in_progress` e rilascia la barriera semaforo SEM_DAY_START per permettere l'inizio sincronizzato delle attività.

**Monitoraggio Attivo:**
Durante la giornata lavorativa, il direttore entra in un ciclo di attesa temporizzata utilizzando `alarm()` e `sigsuspend()`, controllando ogni secondo il progresso della simulazione e verificando le condizioni di "esplosione" (troppi utenti in coda). Quando la soglia viene superata, termina immediatamente la simulazione per proteggere il sistema.

**Terminazione Controllata:**
Al termine di ogni giornata, il direttore raccoglie tutte le statistiche, conta i ticket non serviti, svuota le code, stampa i report dettagliati e invia SIGUSR2 per notificare la fine della giornata. Infine, resetta tutti i contatori per la giornata successiva e gestisce la pulizia finale delle risorse IPC.

### 2.1 Esecuzione del codice di Operatore

Ogni **processo operatore** rappresenta un impiegato specializzato che gestisce un servizio specifico e compete per l'accesso agli sportelli disponibili attraverso meccanismi di concorrenza controllata.

**Avvio e Specializzazione:**
All'avvio, ogni operatore riceve un ID univoco e seleziona casualmente il proprio servizio di specializzazione utilizzando `assign_random_service()`. Si registra nella memoria condivisa aggiornando i propri dati (PID, servizio, stato OPERATOR_WAITING) e configura i gestori per i segnali SIGUSR1 (inizio giornata), SIGUSR2 (fine giornata) e SIGTERM (terminazione).

**Sincronizzazione Giornaliera:**
All'inizio di ogni giornata, l'operatore attende prima il segnale SIGUSR1 dal direttore, poi si blocca sul semaforo SEM_DAY_START per la sincronizzazione globale. Una volta rilasciato, entra nella fase di competizione per uno sportello.

**Acquisizione Sportello:**
L'operatore entra in un ciclo di ricerca sportelli protetto dal mutex SEM_COUNTERS, cercando uno sportello libero compatibile con il proprio servizio. Se non trova sportelli disponibili, si mette in stato OPERATOR_WAITING e usa `sigsuspend()` per attendere segnali di riassegnazione o fine giornata, evitando completamente l'attesa attiva.

**Ciclo di Servizio:**
Una volta assegnato a uno sportello, l'operatore entra nel ciclo principale dove chiama ripetutamente `serve_customer()`. Questa funzione acquisisce il lock specifico del servizio con SEM_SERVICE_LOCK, estrae un ticket dalla coda del servizio, simula il tempo di servizio con `nanosleep()` interrompibile, aggiorna le statistiche di attesa e servizio, e gestisce le pause probabilistiche.

**Gestione Pause:**
Durante il servizio, l'operatore può decidere di prendersi una pausa (probabilità configurabile), cambiando il proprio stato in OPERATOR_ON_BREAK e liberando lo sportello per permettere la riassegnazione ad altri operatori tramite `try_assign_available_operators()`.

### 2.2 Esecuzione del codice di Ticket

Il **processo ticket** costituisce il sistema nervoso centrale della simulazione, gestendo in tempo reale tutte le richieste di ticket e coordinando la comunicazione tra utenti e operatori.

**Inizializzazione Infrastructure:**
All'avvio, il processo ticket si connette alla memoria condivisa e ai semafori, crea o accede alla coda di messaggi con chiave MSG_QUEUE_KEY, si registra memorizzando il proprio PID e inizializza i contatori dei ticket per ogni servizio partendo da 1.

**Attesa Sincronizzazione:**
Come tutti i processi, attende il segnale SIGUSR1 del direttore e poi si blocca sul semaforo SEM_DAY_START. Una volta sincronizzato, resetta tutti i contatori giornalieri e prepara le strutture dati per la nuova giornata.

**Ciclo di Elaborazione:**
Il cuore del processo è un loop che usa `msgrcv()` bloccante per ricevere messaggi di tipo MSG_TICKET_REQUEST. Questa chiamata sospende il processo fino all'arrivo di una richiesta, eliminando completamente l'attesa attiva. Ogni messaggio ricevuto viene elaborato immediatamente tramite `process_new_ticket_request()`.

**Generazione Ticket:**
Per ogni richiesta valida, il processo acquisisce il mutex SEM_QUEUE, ottiene il prossimo numero progressivo per il servizio richiesto, genera l'ID del ticket (es. "B15" per Bancoposta #15), inserisce il ticket nella coda specifica del servizio e aggiorna tutti i contatori. Crucialmente, notifica istantaneamente tutti gli operatori del servizio con SIGUSR1.

**Comunicazione Asincrona:**
Dopo aver elaborato la richiesta, il processo invia SIGUSR1 all'utente richiedente per notificare che il ticket è pronto. Questa comunicazione bidirezionale permette elaborazione completamente asincrona: l'utente può continuare altre attività mentre il ticket viene processato in parallelo.

**Gestione Fine Giornata:**
Al ricevimento di SIGUSR2, il processo termina l'elaborazione di nuove richieste, completa quelle in corso e si prepara per la giornata successiva resettando tutte le strutture dati.

### 2.3 Esecuzione del codice di Utente

Ogni **processo utente** simula un cittadino che interagisce con l'ufficio postale seguendo pattern realistici di arrivo, richiesta servizi e attesa, con comportamenti probabilistici che rendono ogni esecuzione unica.

**Inizializzazione Personale:**
All'avvio, ogni utente riceve un ID univoco, si connette alle risorse IPC (memoria condivisa, semafori, coda messaggi), configura i gestori di segnale e calcola la propria probabilità personale di visitare l'ufficio utilizzando una distribuzione statistica che varia tra 30% e 90%.

**Decisione Giornaliera:**
All'inizio di ogni giornata, dopo aver ricevuto SIGUSR1 e superato la barriera semaforo, l'utente decide probabilisticamente se visitare l'ufficio. Se decide di non andare, incrementa semplicemente le statistiche degli utenti non presentati e attende la fine della giornata.

**Pianificazione Visita:**
Se decide di visitare l'ufficio, l'utente seleziona casualmente il servizio desiderato e calcola un orario di arrivo aleatorio durante la giornata lavorativa. Questo simula realisticamente il fatto che gli utenti non arrivano tutti contemporaneamente all'apertura.

**Programmazione Timer Preciso:**
L'utente utilizza `timer_create()` con `CLOCK_MONOTONIC` per programmare un timer POSIX con precisione ai nanosecondi. L'orario di arrivo viene convertito da minuti simulati a nanosecondi reali utilizzando la proporzione temporale della simulazione, eliminando completamente l'attesa attiva e garantendo tempistiche precise.

**Attesa Event-Driven:**
Invece di controllare attivamente l'orario, l'utente si blocca su `sigtimedwait()` aspettando il segnale SIGALRM del timer programmato, SIGUSR2 (fine giornata) o SIGTERM (terminazione). Questo approccio è completamente event-driven e non consuma risorse CPU durante l'attesa.

**Verifica Disponibilità Servizio:**
Al momento dell'arrivo (ricezione di SIGALRM), l'utente verifica immediatamente la disponibilità del servizio richiesto controllando se esistono sportelli attivi e operatori compatibili. Se il servizio non è disponibile, l'utente decide di tornare a casa senza fare la coda, simulando un comportamento realistico di evitamento delle attese inutili.

**Richiesta Ticket:**
All'arrivo, se il servizio è disponibile, l'utente acquisisce un slot nella memoria condivisa tramite il mutex SEM_MUTEX, inizializza una TicketRequest con i propri dati e timestamp preciso, e invia un messaggio TicketRequestMsg al processo ticket tramite `msgsnd()`.

**Attesa Elaborazione:**
Dopo aver inviato la richiesta, l'utente usa `sigtimedwait()` per attendere la risposta del processo ticket, con timeout periodici per controllare lo stato nella memoria condivisa. Questo meccanismo è completamente event-driven e non sperpera risorse CPU.

**Gestione Servizio:**
Una volta ottenuto il ticket, l'utente attende che un operatore lo chiami controllando periodicamente lo stato `being_served` nella propria TicketRequest. Quando viene servito, rimane in attesa della completion e poi esce dalla simulazione con successo.

**Gestione Timeout:**
Se la giornata termina prima che l'utente riceva il ticket o venga servito, viene automaticamente classificato nelle appropriate statistiche (no_ticket, timeout, o home) e termina la propria esecuzione.

## 3. Conclusioni

Il progetto **SO_Finale** rappresenta un **laboratorio vivente** per esplorare i concetti fondamentali dei sistemi operativi attraverso una simulazione realistica e complessa:

- **Concorrenza** controllata nell'assegnazione sportelli tra operatori  
- **Sincronizzazione** multiprocesso con semafori e segnali
- **Gestione delle risorse** finite (sportelli, tempo, operatori)
- **Comunicazione asincrona** tra processi indipendenti
- **Robustezza** contro sovraccarichi e terminazioni impreviste
- **Efficienza energetica** attraverso l'eliminazione completa dell'attesa attiva

L'architettura dimostra come processi autonomi possano collaborare efficacemente attraverso meccanismi IPC, creando un sistema complesso ma deterministico che simula fedelmente le dinamiche di un ufficio postale reale. L'utilizzo di timer POSIX ad alta precisione e meccanismi event-driven garantisce che il sistema non sprechi risorse CPU, rendendolo scalabile ed efficiente anche con centinaia di processi utente simultanei.

---

*Questa analisi fornisce il framework per comprendere l'esecuzione dei singoli processi e le loro interazioni nel sistema di simulazione dell'ufficio postale.*
