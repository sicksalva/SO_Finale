#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>
#include <sys/types.h>
#include <time.h> // Added for time_t

// System limits for shared memory
#define MAX_USERS 100000        // Support up to 100k users
#define MAX_QUEUE_SIZE 10000    // Max users in queue at once
#define SHM_SIZE sizeof(SharedMemory) // Ensure SHM_SIZE uses the correct struct

// Time management
#define WORK_DAY_HOURS 8
#define WORK_DAY_MINUTES (WORK_DAY_HOURS * 60)  // 480 minutes
#define DAY_SIMULATION_TIME 5   
#define SIM_DURATION 5          // Total number of days to simulate
#define TOTAL_SIMULATION_TIME (SIM_DURATION * DAY_SIMULATION_TIME)  // Total seconds before timeout
#define N_NANO_SECS ((DAY_SIMULATION_TIME * 1000000000L) / WORK_DAY_MINUTES)  // Nanoseconds per simulated minute
#define BREAK_PROBABILITY 0 //Probability of taking a break (%)

// Simulation parameters
#define NOF_WORKERS 10         // Number of operator processes
#define NOF_USERS 200         // Reduced to a more manageable number for system resources
#define NOF_WORKER_SEATS 10
#define NOF_PAUSE 5
#define P_SERV_MIN 100            // da 0 a 100!!! Non è decimale
#define P_SERV_MAX 100       
#define EXPLODE_THRESHOLD 500

// Working hours (in minutes from start of day)
#define OFFICE_OPEN_TIME 0      // Office opens at start of day
#define OFFICE_CLOSE_TIME 480   // Office closes after 8 hours

// Semaphore configuration
#define SEM_KEY 0x1234

// Modifica la definizione degli indici dei semafori

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
#define MAX_REQUESTS 5000

// Dimensione massima della coda per ogni servizio
#define MAX_SERVICE_QUEUE 500

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

// Service types
typedef enum {
    PACKAGES,           // 10 mins
    LETTERS,           // 8 mins
    BANCOPOST,         // 6 mins
    BILLS,             // 8 mins
    FINANCIAL,         // 20 mins
    WATCHES,           // 20 mins
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

// Array of service names 
const char* SERVICE_NAMES[] = {
    "Pacchi",
    "Lettere",
    "Bancoposta",
    "Bollette",
    "Finanza",
    "Orologi"
};

// Convert minutes to nanoseconds for simulation time
static const long SERVICE_TIMES[] = {
    104200000,  // Pacchi (10 min = 104.2 ms)
    83360000,   // Lettere (8 min = 83.36 ms)
    62520000,   // Bancoposta (6 min = 62.52 ms)
    83360000,   // Bollette (8 min = 83.36 ms)
    208400000,  // Finanza (20 min = 208.4 ms)
    208400000   // Orologi (20 min = 208.4 ms)
};

// Forward declare structs if needed due to order
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

// Shared Memory Structures

typedef struct UserData { // Added struct tag name
    ServiceType service_requested;
    int ticket_number;
    char service_letter;     // Lettera che identifica il servizio
    int served;             // Flag showing if user was served
    int counter_id;         // Which counter is serving this user
    int simulation_day;     // Which day this ticket was issued on
    pid_t pid;              // PID of this specific user process (if needed)
} UserData;

typedef struct Counter { // Added struct tag name
    pid_t operator_pid; // Maybe rename this if Operator struct holds the PID
    int active;
    ServiceType current_service;
    int total_served;
} Counter;

typedef struct Operator { // Added struct tag name
    pid_t pid;                     // Process ID of operator
    int active;                    // Is operator active
    ServiceType current_service;   // Current service being provided
    int total_served;             // Total number of users served
    int total_pauses;             // Number of breaks taken
    OperatorStatus status;        // Stato corrente dell'operatore
} Operator;

typedef struct LogMessage { // Added struct tag name
    MessageType type;
    int user_id; // Or pid_t user_pid;
    pid_t process_pid;
    ServiceType service;
    int ticket_number;
    int simulation_day;
} LogMessage;

// This is the main structure for the shared memory segment
typedef struct {
    // Process IDs - MOVED HERE
    pid_t ticket_pid;
    pid_t user_pids[NOF_USERS];     // Array to store all user PIDs
    pid_t operator_pids[NOF_WORKERS]; // Array to store all operator PIDs

    // Service availability
    int service_available[SERVICE_COUNT];

    // Counters and operators
    Counter counters[NOF_WORKER_SEATS];
    Operator operators[NOF_WORKERS]; // Array to store detailed operator info

    // Queue management
    UserData users[MAX_QUEUE_SIZE]; // Array for user data/queue entries
    int queue_head; // Example: Indices for queue management
    int queue_tail; // Example: Indices for queue management
    int queue_count; // Example: Number of users currently in queue

    // Ticket management
    int current_tickets[SERVICE_COUNT];

    // Statistics
    int daily_services[SERVICE_COUNT];
    long avg_service_times[SERVICE_COUNT];
    long min_service_times[SERVICE_COUNT];
    long max_service_times[SERVICE_COUNT];
    int total_services[SERVICE_COUNT];
    int daily_unserved[SERVICE_COUNT];

    // Message system
    LogMessage log_messages[MAX_QUEUE_SIZE]; // Consider a smaller size?
    int message_count;
    int message_index; // Or use head/tail for circular buffer

    // Simulation control
    int simulation_day;    // Current day in the simulation
    int day_in_progress;   // Flag to indicate if a day is currently in progress
    int termination_flag; // Signal for processes to exit
    int reset_complete;

    // Request handling (if needed by director)
    int pending_request;
    ServiceType requested_service;

    // Ticket request management
    int next_request_index;                 // Indice per la prossima richiesta di ticket
    int next_ticket_number;                 // Numero progressivo per il prossimo ticket
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
    int total_tickets_served;               // Totale ticket serviti
    int total_users_home;                   // Totale utenti tornati a casa
    int total_users_timeout;                // Totale utenti non serviti per mancanza di tempo
    int total_users_no_ticket;              // Totale utenti che non hanno ricevuto il ticket
    
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

    // Somma totale delle pause di tutti gli operatori
    int total_pauses_global;

} SharedMemory;

// IPC Keys
#define SHM_KEY 0x1234

#endif