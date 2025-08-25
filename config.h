#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <sys/types.h>
#include <time.h> // Added for time_t

// Limiti di sistema per la memoria condivisa
#define MAX_USERS 100000        
#define MAX_QUEUE_SIZE 10000    // Massimo utenti in coda contemporaneamente
#define SHM_SIZE sizeof(SharedMemory) // Assicura che SHM_SIZE usi la struttura corretta

// Gestione del tempo
#define WORK_DAY_HOURS 8
#define WORK_DAY_MINUTES (WORK_DAY_HOURS * 60)  // 480 minuti
#define DAY_SIMULATION_TIME 5   
#define SIM_DURATION 5          // Numero totale di giorni da simulare
#define TOTAL_SIMULATION_TIME (SIM_DURATION * DAY_SIMULATION_TIME)  // Secondi totali prima del timeout
#define N_NANO_SECS ((DAY_SIMULATION_TIME * 1000000000L) / WORK_DAY_MINUTES)  // Nanosecondi per minuto simulato
#define BREAK_PROBABILITY 0 // Probabilità di fare una pausa (%)

// Parametri di simulazione
#define NOF_WORKERS 10         // Numero di processi operatore
#define NOF_USERS 100         // Ridotto a un numero più gestibile per le risorse di sistema
#define NOF_WORKER_SEATS 10
#define NOF_PAUSE 5
#define P_SERV_MIN 50            // da 0 a 100!!! Non è decimale
#define P_SERV_MAX 100       
#define EXPLODE_THRESHOLD 1000

// Orari di lavoro (in minuti dall'inizio del giorno)
#define OFFICE_OPEN_TIME 0      // L'ufficio apre all'inizio del giorno
#define OFFICE_CLOSE_TIME 480   // L'ufficio chiude dopo 8 ore

// Configurazione semafori
#define SEM_KEY 0x1234

// Definizione degli indici dei semafori
#define SEM_MUTEX 0         // Mutex per accesso generale
#define SEM_QUEUE 1         // Semaforo per la gestione delle code
#define SEM_TICKET_REQ 2    // Richiesta di biglietti
#define SEM_TICKET_READY 3  // Biglietti pronti
#define SEM_COUNTERS 4      // Accesso agli sportelli
#define SEM_SYNC 5          // Sincronizzazione generica
#define SEM_DAY_START 6     // Sincronizzazione inizio giorno
#define SEM_TICKET_WAIT 7   // Sincronizzazione attesa ticket

// Semafori separati per ogni servizio (molto più efficiente!)
#define SEM_SERVICE_LOCK_BASE 8  // Base per i semafori dei servizi
#define SEM_SERVICE_LOCK_PACKAGES (SEM_SERVICE_LOCK_BASE + PACKAGES)      // 8
#define SEM_SERVICE_LOCK_LETTERS (SEM_SERVICE_LOCK_BASE + LETTERS)        // 9
#define SEM_SERVICE_LOCK_BANCOPOST (SEM_SERVICE_LOCK_BASE + BANCOPOST)    // 10
#define SEM_SERVICE_LOCK_BILLS (SEM_SERVICE_LOCK_BASE + BILLS)            // 11
#define SEM_SERVICE_LOCK_FINANCIAL (SEM_SERVICE_LOCK_BASE + FINANCIAL)    // 12
#define SEM_SERVICE_LOCK_WATCHES (SEM_SERVICE_LOCK_BASE + WATCHES)        // 13

// Macro per calcolare il semaforo specifico di un servizio
#define SEM_SERVICE_LOCK(service) (SEM_SERVICE_LOCK_BASE + (service))

#define NUM_SEMS (SEM_SERVICE_LOCK_BASE + SERVICE_COUNT)  // 14 semafori totali

// Dimensione massima della coda delle richieste di ticket
#define MAX_REQUESTS 2000

// Dimensione massima della coda per ogni servizio
#define MAX_SERVICE_QUEUE 2000

// Chiave per la coda messaggi
#define MSG_QUEUE_KEY 8912

// Prefissi per i ticket di ogni servizio
static const char SERVICE_PREFIXES[] = {
    'P',  // Pacchi
    'L',  // Lettere
    'B',  // Bancoposta
    'F',  // Bollette (Fatture)
    'I',  // Finanza (Investimenti)
    'O'   // Orologi
};

// Tipi di servizio
typedef enum {
    PACKAGES,           // 10 minuti
    LETTERS,           // 8 minuti
    BANCOPOST,         // 6 minuti
    BILLS,             // 8 minuti
    FINANCIAL,         // 20 minuti
    WATCHES,           // 20 minuti
    SERVICE_COUNT
} ServiceType;

// Stati possibili per una richiesta di ticket
typedef enum {
    REQUEST_UNDEFINED = 0,   // Stato iniziale/non definito
    REQUEST_PENDING = 1,     // Richiesta in attesa di elaborazione
    REQUEST_PROCESSING = 2,  // Richiesta in fase di elaborazione
    REQUEST_COMPLETED = 3,   // Richiesta completata con successo
    REQUEST_REJECTED = 4     // Richiesta rifiutata
} RequestStatus;

// Message types for logging
typedef enum {
    MSG_USER_VISIT,
    MSG_USER_LEAVE,
    MSG_USER_NO_SERVICE,
    MSG_TICKET_ISSUED,
    MSG_SERVICE_START,
    MSG_SERVICE_END,
    MSG_OPERATOR_START,
    MSG_OPERATOR_SERVICE_END,
    MSG_OPERATOR_BREAK,
    MSG_OPERATOR_RETURN,
    MSG_TICKET_START,      // Ticket process started
    MSG_TICKET_RESET,      // Daily ticket reset
    MSG_USER_WAITING,      // User is waiting in queue
    MSG_COUNTER_AVAILABLE  // Counter became available
} MessageType;

// Aggiungi questa definizione per i messaggi
#define MSG_TICKET_REQUEST 1

typedef struct {
    long mtype;             // Tipo di messaggio (sempre 1 per ticket request)
    int user_id;            // ID utente richiedente
    int service_id;         // Servizio richiesto
    int request_index;      // Indice della richiesta in shared memory
    time_t request_time;    // Timestamp della richiesta
    pid_t user_pid;         // PID del processo utente richiedente
} TicketRequestMsg;

// Array dei nomi dei servizi 
const char* SERVICE_NAMES[] = {
    "Pacchi",
    "Lettere",
    "Bancoposta",
    "Bollette",
    "Finanza",
    "Orologi"
};

// Converti minuti in nanosecondi per il tempo di simulazione
static const long SERVICE_TIMES[] = {
    104200000,  // Pacchi (10 min = 104.2 ms)
    83360000,   // Lettere (8 min = 83.36 ms)
    62520000,   // Bancoposta (6 min = 62.52 ms)
    83360000,   // Bollette (8 min = 83.36 ms)
    208400000,  // Finanza (20 min = 208.4 ms)
    208400000   // Orologi (20 min = 208.4 ms)
};

// Dichiarazioni anticipate delle strutture se necessario per l'ordine
struct Operator;
struct Counter;
struct UserData;
struct LogMessage;

// Definizione degli stati degli operatori
typedef enum {
    OPERATOR_UNDEFINED = 0,  // Stato iniziale/non definito
    OPERATOR_WORKING = 1,    // L'operatore sta lavorando a uno sportello
    OPERATOR_WAITING = 2,    // L'operatore è in attesa (non ha trovato uno sportello)
    OPERATOR_ON_BREAK = 3,   // L'operatore è in pausa
    OPERATOR_FINISHED = 4    // L'operatore ha terminato il turno giornaliero
} OperatorStatus;

// Struttura per una richiesta di ticket
typedef struct TicketRequest {
    int user_id;                // ID dell'utente che ha fatto la richiesta
    int service_id;             // Servizio richiesto
    struct timespec request_time;       // Timestamp preciso della richiesta (nanosecondi)
    struct timespec service_start_time; // Timestamp preciso di quando inizia il servizio
    RequestStatus status;       // Stato attuale della richiesta
    int ticket_number;          // Numero del ticket assegnato (progressivo per servizio)
    char ticket_id[10];         // Identificativo del ticket (es. "L5", "B12")
    int counter_id;             // ID dello sportello assegnato (se presente)
    pid_t serving_operator_pid; // PID dell'operatore che sta servendo questo utente (0 se nessuno)
    int being_served;           // Flag: 1 se attualmente in servizio, 0 altrimenti
    int served_successfully;    // Flag: 1 se il servizio è stato completato con successo, 0 altrimenti
    long wait_time_ns;          // Tempo di attesa in nanosecondi (calcolato quando il servizio finisce)
} TicketRequest;

// Strutture per la memoria condivisa

typedef struct Counter { // Aggiunto nome tag struttura
    pid_t operator_pid; // PID dell'operatore assegnato a questo sportello
    int active;
    ServiceType current_service;
    int total_served;
} Counter;

typedef struct Operator { // Aggiunto nome tag struttura
    pid_t pid;                     // ID processo dell'operatore
    int active;                    // L'operatore è attivo
    ServiceType current_service;   // Servizio attualmente fornito
    int total_served;             // Numero totale di utenti serviti
    int total_pauses;             // Numero di pause fatte
    OperatorStatus status;        // Stato corrente dell'operatore
} Operator;

typedef struct LogMessage { // Aggiunto nome tag struttura
    MessageType type;
    int user_id; // O pid_t user_pid;
    pid_t process_pid;
    ServiceType service;
    int ticket_number;
    int simulation_day;
} LogMessage;

// Questa è la struttura principale per il segmento di memoria condivisa
typedef struct {
    // ID dei processi
    pid_t ticket_pid;
    pid_t user_pids[NOF_USERS];     // Array per memorizzare tutti i PID degli utenti
    pid_t operator_pids[NOF_WORKERS]; // Array per memorizzare tutti i PID degli operatori

    // Disponibilità servizi
    int service_available[SERVICE_COUNT];

    // Sportelli e operatori
    Counter counters[NOF_WORKER_SEATS];
    Operator operators[NOF_WORKERS]; // Array per memorizzare informazioni dettagliate degli operatori

    // Controllo simulazione
    int simulation_day;    // Giorno corrente nella simulazione
    int day_in_progress;   // Flag per indicare se un giorno è attualmente in corso
    int termination_flag; // Segnale per i processi di uscire
    int reset_complete;

    // Gestione richieste ticket
    int next_request_index;                 // Indice per la prossima richiesta di ticket
    TicketRequest ticket_requests[MAX_REQUESTS];  // Coda delle richieste di ticket

    // Code separate per ogni servizio
    int next_service_ticket[SERVICE_COUNT]; // Contatore per i ticket di ogni servizio
    int service_queues[SERVICE_COUNT][MAX_SERVICE_QUEUE]; // Code dei ticket (indici delle richieste)
    int service_queue_head[SERVICE_COUNT];  // Indice di testa per ogni coda
    int service_queue_tail[SERVICE_COUNT];  // Indice di coda per ogni coda
    int service_tickets_waiting[SERVICE_COUNT]; // Numero di ticket in attesa per ogni servizio

    int daily_tickets_served[SERVICE_COUNT]; // Ticket serviti per ogni servizio
    int daily_users_home[SERVICE_COUNT];    // Utenti tornati a casa per ogni servizio
    int daily_users_timeout[SERVICE_COUNT];  // Utenti non serviti per mancanza di tempo
    int daily_users_no_ticket[SERVICE_COUNT]; // Utenti che non hanno ricevuto il ticket entro la giornata
    int daily_users_not_arrived[SERVICE_COUNT]; // Utenti che non si sono presentati all'ufficio postale
    int total_tickets_served;               // Totale ticket serviti
    int total_users_home;                   // Totale utenti tornati a casa
    int total_users_timeout;                // Totale utenti non serviti per mancanza di tempo
    int total_users_no_ticket;              // Totale utenti che non hanno ricevuto il ticket
    int total_users_not_arrived;            // Totale utenti che non si sono presentati all'ufficio postale
    int total_users_not_arrived_per_service[SERVICE_COUNT]; // Totale utenti che non si sono presentati per servizio
    
    // Statistiche sui tempi di attesa
    long min_wait_time[SERVICE_COUNT];     // Tempo minimo di attesa per servizio (in nanosecondi)
    long max_wait_time[SERVICE_COUNT];     // Tempo massimo di attesa per servizio (in nanosecondi)
    long total_wait_time[SERVICE_COUNT];   // Tempo totale di attesa per servizio (in nanosecondi)
    int wait_count[SERVICE_COUNT];         // Conteggio degli utenti che hanno atteso
    
    // Statistiche sui tempi di attesa per giorno
    long daily_total_wait_time[SERVICE_COUNT]; // Tempo totale di attesa giornaliero per servizio (in nanosecondi)
    int daily_wait_count[SERVICE_COUNT];       // Conteggio giornaliero degli utenti che hanno atteso
    
    // Statistiche aggregate sui tempi di attesa
    long total_wait_time_all_services;     // Tempo totale di attesa per tutti i servizi (in nanosecondi)
    int total_wait_count_all_services;     // Conteggio totale degli utenti che hanno atteso
    long daily_total_wait_time_all;       // Tempo totale di attesa giornaliero per tutti i servizi (in nanosecondi)
    int daily_wait_count_all;             // Conteggio giornaliero di tutti gli utenti che hanno atteso

    // Statistiche sui tempi di servizio (per le nuove statistiche richieste)
    long min_service_time[SERVICE_COUNT];  // Tempo minimo di servizio per tipo (in nanosecondi)
    long max_service_time[SERVICE_COUNT];  // Tempo massimo di servizio per tipo (in nanosecondi) 
    long total_service_time[SERVICE_COUNT]; // Tempo totale di servizio per tipo
    int service_count[SERVICE_COUNT];      // Numero di servizi erogati per tipo
    
    // Statistiche aggregate per la simulazione
    int total_users_served_simulation;     // Totale utenti serviti in tutta la simulazione
    int total_services_provided_simulation; // Totale servizi erogati in tutta la simulazione 
    int total_services_not_provided_simulation; // Totale servizi non erogati in tutta la simulazione
    int total_pauses_simulation;           // Totale pause in tutta la simulazione
    
    // Array per memorizzare le statistiche di ogni giorno (per calcolare le medie)
    int users_served_per_day[SIM_DURATION];
    int services_not_provided_per_day[SIM_DURATION]; 
    long total_wait_time_per_day[SIM_DURATION];      // Tempo totale di attesa per giorno (in nanosecondi)
    int wait_count_per_day[SIM_DURATION];
    long total_service_time_per_day[SIM_DURATION];
    int service_count_per_day[SIM_DURATION];
    int pauses_per_day[SIM_DURATION];
    int operators_active_per_day[SIM_DURATION];
    
    // Statistiche per servizio per giorno (per le medie per tipo di servizio)
    int users_served_per_service_per_day[SERVICE_COUNT][SIM_DURATION];
    int services_not_provided_per_service_per_day[SERVICE_COUNT][SIM_DURATION];
    long total_wait_time_per_service_per_day[SERVICE_COUNT][SIM_DURATION];  // Tempo di attesa per servizio per giorno (in nanosecondi)
    int wait_count_per_service_per_day[SERVICE_COUNT][SIM_DURATION];
    long total_service_time_per_service_per_day[SERVICE_COUNT][SIM_DURATION];
    int service_count_per_service_per_day[SERVICE_COUNT][SIM_DURATION];

    // Medie cumulative progressive per ogni giorno
    double cumulative_avg_users_served[SIM_DURATION];
    double cumulative_avg_services_provided[SIM_DURATION];
    double cumulative_avg_services_not_provided[SIM_DURATION];
    
    // Somma totale degli operatori attivi per servizio durante tutta la simulazione
    int operators_active_per_service_total[SERVICE_COUNT];

} SharedMemory;

// Chiavi IPC
#define SHM_KEY 0x1234

#endif