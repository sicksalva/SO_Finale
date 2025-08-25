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

## 1. Un Giorno All'Ufficio Postale: Come Funziona la Simulazione

### 1.1 Il Risveglio del Sistema

Quando avviamo `./direttore`, assistiamo alla nascita di un ecosistema digitale che ricrea fedelmente la complessità di un ufficio postale moderno. Il **processo direttore** agisce come il manager dell'ufficio: coordina tutto, dall'apertura mattutina alla chiusura serale, raccogliendo statistiche e garantendo che ogni ingranaggio funzioni perfettamente.

Il primo atto è la **creazione delle risorse**: la memoria condivisa diventa il "cervello" del sistema, contenendo tutte le informazioni condivise tra i processi. I semafori fungono da "semafori digitali" per coordinare l'accesso alle risorse critiche, mentre le code di messaggi permettono la comunicazione asincrona tra utenti e sistema di ticket.

### 1.2 L'Apertura dell'Ufficio: Un Balletto Sincronizzato

```
🏢 Ore 8:00 - APERTURA UFFICIO POSTALE
```

Il direttore **invia il segnale SIGUSR1** a tutti i processi: è l'equivalente digitale del "si apre l'ufficio!". Questo momento rappresenta una **sincronizzazione globale** perfettamente orchestrata:

- **Gli operatori** si precipitano a cercare uno sportello libero compatibile con il loro servizio
- **Il processo ticket** si attiva e inizia ad ascoltare le richieste in arrivo
- **Gli utenti** iniziano a pianificare le loro visite, alcuni decidendo di andare subito, altri di aspettare

### 1.3 La Vita degli Operatori: Competizione e Servizio

Immaginiamo l'**Operatore #5**, specializzato in "Bancoposta". Al segnale di apertura, deve **competere** con gli altri operatori per conquistare uno sportello. Il sistema implementa una vera **race condition controllata**: chi arriva prima (in termini di scheduling del SO) si aggiudica lo sportello.

Una volta assegnato allo **Sportello #3**, l'operatore entra in un ciclo intelligente:

```c
while (day_in_progress && running) {
    // 1. Cerca ticket da servire nella coda Bancoposta
    // 2. Se trova un cliente, lo serve per 4-9 minuti simulati
    // 3. Se non trova nessuno, usa sigsuspend() per attendere
    // 4. Occasionalmente decide di prendersi una pausa
}
```

Il bello è che **non c'è attesa attiva**: quando non ci sono clienti, l'operatore si "addormenta" fino a quando il processo ticket gli invia un `SIGUSR1` per dirgli "è arrivato un nuovo cliente!".

### 1.4 Il Sistema Ticket: Il Cuore Pulsante

Il **processo ticket** è il vero gioiello dell'architettura. Funziona come un **dispatcher intelligente** che:

1. **Riceve richieste** via coda di messaggi (`msgrcv` bloccante)
2. **Genera immediatamente** un ticket (es. "B15" per Bancoposta #15)
3. **Inserisce** il ticket nella coda specifica del servizio
4. **Notifica istantaneamente** tutti gli operatori del servizio con `SIGUSR1`

La magia sta nella **comunicazione asincrona**: l'utente invia la richiesta e può immediatamente controllare lo stato nella memoria condivisa, mentre il ticket viene processato in parallelo.

### 1.5 L'Esperienza dell'Utente: Dall'Arrivo al Servizio

Seguiamo l'**Utente #23** in una giornata tipo:

**Mattina presto (7:30):**
L'utente "dorme" in attesa del segnale di apertura ufficio.

**Apertura (8:00):**
Riceve `SIGUSR1`, decide di visitare l'ufficio (probabilità del 70%) e sceglie il servizio "Lettere".

**Arrivo pianificato (10:30):**
Il sistema simula il tempo di arrivo - non tutti arrivano subito! L'utente arriva al minuto 150 della giornata.

**Richiesta ticket:**
```c
// Invia messaggio al processo ticket
TicketRequestMsg msg = {
    .mtype = MSG_TICKET_REQUEST,
    .user_id = 23,
    .service_id = LETTERS,
    .user_pid = getpid()
};
msgsnd(msgid, &msg, sizeof(msg), 0);
```

**Attesa del ticket:**
L'utente usa `sigtimedwait()` per attendere la risposta del sistema ticket. Non c'è polling - è **event-driven**.

**Ricevimento ticket "L8":**
Il processo ticket risponde con `SIGUSR1` e l'utente trova il suo ticket nella memoria condivisa.

**Attesa del servizio:**
L'utente ora aspetta che un operatore lo chiami. Il sistema tracked tutto: tempo di richiesta, tempo di servizio, tempo di attesa.

**Servizio completato:**
Dopo 6-12 minuti simulati, l'operatore completa il servizio e l'utente esce soddisfatto.

### 1.6 Momenti di Stress: Gestione delle Situazioni Critiche

**Scenario "Explode" - Troppi utenti contemporaneamente:**
Con 200 utenti e soglia explode a 20, quando ci sono troppi ticket in attesa, il sistema **interrompe la creazione di nuovi utenti** per 30 secondi - una feature di auto-protezione intelligente.

**Scenario "Timeout" - Pressione temporale:**
Con 150 utenti, 8 operatori e tempi ridotti, osserviamo come il sistema gestisce la pressione: gli utenti potrebbero non ricevere il ticket in tempo, alcuni operatori potrebbero andare in pausa, creando **code dinamiche** che si allungano e accorciano.

**Fine giornata improvvisa:**
Quando il direttore invia `SIGUSR2` (fine giornata), assistiamo a una **terminazione orchestrata**:
- Gli operatori terminano il cliente attuale ma non ne prendono di nuovi
- I ticket in coda vengono marcati come "non serviti"
- Le statistiche vengono aggiornate atomicamente

### 1.7 La Bellezza della Concorrenza

Quello che rende magico questo sistema è vedere **processi indipendenti** che collaborano senza conoscersi direttamente:

- **Un utente** non sa quale operatore lo servirà
- **Un operatore** non sa quali utenti arriveranno
- **Il ticket system** non sa chi è disponibile, ma coordina tutto perfettamente
- **Il direttore** osserva tutto senza interferire, come un manager ideale

Ogni esecuzione è **unica**: l'ordine di arrivo degli utenti, l'assegnazione degli sportelli, le pause degli operatori - tutto dipende dal timing del sistema operativo, creando scenari sempre diversi ma sempre coerenti.

### 1.8 Un Sistema Che Impara

Il progetto **SO_Finale** non è solo una simulazione - è un **laboratorio vivente** per esplorare i concetti fondamentali dei sistemi operativi:

- **Race conditions** controllate nell'assegnazione sportelli
- **Sincronizzazione** complessa tra processi indipendenti  
- **Gestione delle risorse** finite (sportelli, tempo, operatori)
- **Comunicazione asincrona** scalabile ed efficiente
- **Robustezza** contro guasti e sovraccarichi

Ogni riga di codice racconta una storia di **coordinazione perfetta** tra entità autonome, proprio come nella vita reale di un ufficio postale, ma con la precisione e determinismo che solo un sistema digitale può offrire.

---

*Questa visione generale fornisce il framework concettuale per comprendere l'architettura e gli obiettivi del sistema. I capitoli successivi approfondiranno i dettagli implementativi e le scelte progettuali.*
