#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>  // Per gettimeofday()
#include "config.h"

// Variabili globali
SharedMemory *shm_ptr = NULL;
int semid = -1;
int shmid = -1;
volatile int running = 1;
volatile int day_in_progress = 0;

// Variabile globale per la coda di messaggi
int msgid = -1;

// Gestore segnale per la terminazione
void termination_handler(int signum __attribute__((unused)))
{
    running = 0;
}

// Funzione per il reset giornaliero di code, contatori e richieste
void reset_daily_counters() {
    if (shm_ptr != NULL && shm_ptr != (void *)-1) {
        struct sembuf sem_op;
        sem_op.sem_num = SEM_QUEUE;
        sem_op.sem_op = -1; // Lock
        sem_op.sem_flg = 0;
        if (semop(semid, &sem_op, 1) < 0) {
            perror("Ticket: Failed to acquire queue mutex for daily reset");
            return;
        }
        shm_ptr->next_request_index = 0;
        for (int i = 0; i < SERVICE_COUNT; i++) {
            shm_ptr->service_queue_head[i] = 0;
            shm_ptr->service_queue_tail[i] = 0;
            shm_ptr->service_tickets_waiting[i] = 0;
            shm_ptr->next_service_ticket[i] = 1;
        }
        // Reset solo delle richieste completate/rifiutate/undefined
        for (int i = 0; i < MAX_REQUESTS; i++) {
            if (shm_ptr->ticket_requests[i].status == REQUEST_COMPLETED ||
                shm_ptr->ticket_requests[i].status == REQUEST_REJECTED ||
                shm_ptr->ticket_requests[i].status == REQUEST_UNDEFINED) {
                memset(&shm_ptr->ticket_requests[i], 0, sizeof(TicketRequest));
            }
        }
        sem_op.sem_op = 1; // Unlock
        if (semop(semid, &sem_op, 1) < 0) {
            perror("Ticket: Failed to release queue mutex after daily reset");
        }
    }
}

// Handler per il segnale di inizio giornata
void day_start_handler(int signum __attribute__((unused)))
{
    // Il reset verrà fatto nel loop principale per evitare duplicazioni
    day_in_progress = 1;
}

// Handler per la fine della giornata
void day_end_handler(int signum __attribute__((unused)))
{
    day_in_progress = 0;
}

// Funzione per elaborare direttamente i messaggi di richiesta ticket usando request_index
void process_new_ticket_request(TicketRequestMsg *msg)
{
    // CONTROLLO CRITICO: Verifica che la giornata sia ancora in corso
    if (!shm_ptr->day_in_progress) {
        printf("Ticket: [REJECTED] Richiesta da utente %d rifiutata - giornata terminata\n", msg->user_id);
        
        // Notifica l'utente che la richiesta è stata rifiutata
        if (msg->user_pid > 0) {
            kill(msg->user_pid, SIGUSR2);
        }
        return;
    }

    // Salva il PID dell'utente per inviare la notifica successivamente
    pid_t user_pid = msg->user_pid;
    int request_index = msg->request_index;

    // Verifica che il request_index sia valido
    if (request_index < 0 || request_index >= MAX_REQUESTS) {
        printf("Ticket: [ERROR] request_index %d non valido\n", request_index);
        return;
    }

    // Ottieni direttamente la richiesta dalla memoria condivisa usando l'indice
    TicketRequest *request = &(shm_ptr->ticket_requests[request_index]);
    
    // Verifica che la richiesta esista e sia in stato PENDING
    if (request->status != REQUEST_PENDING) {
        printf("Ticket: [ERROR] Richiesta %d non è PENDING (status: %d)\n", 
               request_index, request->status);
        return;
    }

    // Aggiorna lo stato della richiesta a elaborazione in corso
    request->status = REQUEST_PROCESSING;

    // Acquisisce il mutex per le code dei ticket
    struct sembuf sem_op;
    sem_op.sem_num = SEM_QUEUE;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;

    if (semop(semid, &sem_op, 1) < 0)
    {
        perror("Ticket: Failed to acquire queue mutex");
        request->status = REQUEST_REJECTED;
        return;
    }

    // Ottiene il prossimo numero di ticket per questo servizio specifico
    int service_id = request->service_id;
    int ticket_number = shm_ptr->next_service_ticket[service_id]++;

    // Aggiunge il ticket alla coda del servizio appropriata
    int queue_pos = shm_ptr->service_queue_tail[service_id];
    shm_ptr->service_queues[service_id][queue_pos] = request_index;

    // Aggiorna la coda di coda (buffer circolare)
    shm_ptr->service_queue_tail[service_id] = (queue_pos + 1) % MAX_SERVICE_QUEUE;

    // Aggiorna il conteggio dei ticket per questo servizio
    shm_ptr->service_tickets_waiting[service_id]++;

    // Rilascia il mutex
    sem_op.sem_num = SEM_QUEUE;
    sem_op.sem_op = 1; // Unlock
    sem_op.sem_flg = 0;

    if (semop(semid, &sem_op, 1) < 0)
    {
        perror("Ticket: Failed to release queue mutex");
        // Continuiamo comunque poiché la sezione critica è completata
    }
    
    // Notifica gli operatori che c'è un nuovo ticket disponibile
    struct sembuf ticket_ready_signal;
    ticket_ready_signal.sem_num = SEM_TICKET_READY;
    ticket_ready_signal.sem_op = 1; // Signal (operazione V)
    ticket_ready_signal.sem_flg = 0;
    
    if (semop(semid, &ticket_ready_signal, 1) < 0)
    {
        perror("Ticket: Failed to signal ticket ready");
    }

    // NOTIFICA ISTANTANEA: Invia segnale SIGUSR1 a tutti gli operatori attivi per questo servizio
    // per notificare immediatamente la disponibilità di un nuovo ticket
    for (int i = 0; i < NOF_WORKERS; i++) {
        if (shm_ptr->operators[i].active && 
            (int)shm_ptr->operators[i].current_service == service_id &&
            shm_ptr->operators[i].status == OPERATOR_WORKING &&
            shm_ptr->operators[i].pid > 0) {
            kill(shm_ptr->operators[i].pid, SIGUSR1);
        }
    }

    // Genera l'identificativo del ticket (es. L1, B1, ecc.)
    char ticket_id[10];
    sprintf(ticket_id, "%c%d", SERVICE_PREFIXES[service_id], ticket_number);

    // Aggiorna la richiesta con le informazioni del ticket
    request->ticket_number = ticket_number;
    request->status = REQUEST_COMPLETED; // Generazione ticket completata
    strncpy(request->ticket_id, ticket_id, sizeof(request->ticket_id) - 1);
    request->ticket_id[sizeof(request->ticket_id) - 1] = '\0'; // Assicura terminazione null

    //printf("Ticket: Assigned ticket %s to user %d for service %s\n",
    //       ticket_id, request->user_id, SERVICE_NAMES[request->service_id]);
    
    // Invia un segnale all'utente per notificare che la richiesta è stata elaborata
    if (user_pid > 0) {
        //printf("Ticket: Invio segnale SIGUSR1 all'utente %d (PID: %d)\n", 
        //      msg->user_id, user_pid);
        kill(user_pid, SIGUSR1);
    }
}

int main()
{
    // Carica la configurazione dalla variabile d'ambiente
    const char* config_file = getenv("SO_CONFIG_FILE");
    if (!config_file) config_file = "timeout.conf"; // default
    read_config(config_file);

    // Imposta i gestori di segnale
    signal(SIGTERM, termination_handler); // Segnale di terminazione
    signal(SIGINT, termination_handler);  // Ctrl+C
    signal(SIGUSR1, day_start_handler);   // Inizio del giorno
    signal(SIGUSR2, day_end_handler);     // Fine del giorno

    //printf("Ticket process starting...\n");

    // Attacca alla memoria condivisa
    shmid = shmget(SHM_KEY, sizeof(SharedMemory), 0666);
    if (shmid == -1)
    {
        perror("Ticket: shmget failed");
        exit(EXIT_FAILURE);
    }

    shm_ptr = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shm_ptr == (void *)-1)
    {
        perror("Ticket: shmat failed");
        exit(EXIT_FAILURE);
    }

    // Ottieni accesso ai semafori
    semid = semget(SEM_KEY, NUM_SEMS, 0666);
    if (semid == -1)
    {
        perror("Ticket: semget failed");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // Crea o accedi alla coda di messaggi
    msgid = msgget(MSG_QUEUE_KEY, IPC_CREAT | 0666);
    if (msgid == -1)
    {
        perror("Ticket: msgget failed");
        exit(EXIT_FAILURE);
    }

    // Memorizza il nostro PID nella memoria condivisa
    shm_ptr->ticket_pid = getpid();

    // Inizializzazione iniziale
    struct sembuf sem_op;
    sem_op.sem_num = SEM_QUEUE;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;

    if (semop(semid, &sem_op, 1) < 0)
    {
        perror("Ticket: Failed to acquire queue mutex for initialization");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // Inizializza le code dei servizi
    for (int i = 0; i < SERVICE_COUNT; i++)
    {
        if (shm_ptr->next_service_ticket[i] == 0)
        {
            shm_ptr->next_service_ticket[i] = 1; // Inizia ogni servizio con ticket #1
        }
    }

    // Rilascia il mutex
    sem_op.sem_num = SEM_QUEUE;
    sem_op.sem_op = 1; // Unlock
    sem_op.sem_flg = 0;

    if (semop(semid, &sem_op, 1) < 0)
    {
        perror("Ticket: Failed to release queue mutex after initialization");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    //printf("Ticket process initialized. PID: %d\n", getpid());

    // Loop principale per la simulazione
    while (running)
    {
        // Attendi l'inizio della giornata tramite segnale e semaforo
        if (!shm_ptr->day_in_progress && running)
        {
            //printf("Ticket: Attendo l'inizio della giornata...\n");
            
            // Configura un set di segnali per attendere il segnale SIGUSR1
            sigset_t wait_mask;
            sigfillset(&wait_mask);
            sigdelset(&wait_mask, SIGUSR1);  // Attendi il segnale di inizio giornata
            sigdelset(&wait_mask, SIGTERM);  // Permettiamo anche la terminazione
            
            // Sospendi l'esecuzione fino a quando non arriva un segnale
            sigsuspend(&wait_mask);
            
            // Se il segnale era di terminazione, esci dal loop
            if (!running) {
                continue;
            }
            
            // Dopo aver ricevuto il segnale, attendi anche sul semaforo SEM_DAY_START
            struct sembuf sem_wait;
            sem_wait.sem_num = SEM_DAY_START;
            sem_wait.sem_op = -1;  // Operazione P (wait)
            sem_wait.sem_flg = 0;
            
            //printf("Ticket: Attendo sulla barriera semaforo SEM_DAY_START...\n");
            
            // Attendi sul semaforo
            if (semop(semid, &sem_wait, 1) < 0) {
                if (errno != EINTR) {  // Ignora se interrotto da segnale
                    perror("Ticket: semop wait for day start failed");
                }
            } else {
                //printf("Ticket: Semaforo SEM_DAY_START acquisito, giornata iniziata\n");
            }
        }

        if (!running)
            break;

        // All'inizio di ogni giornata, resetta i contatori e le code
        reset_daily_counters();

        //-----------------------------------------------------------------------------------------------------------------------------------------------------
        // Loop per la giornata corrente - gestisce le richieste di ticket
        //printf("Ticket: Giorno %d in corso, in attesa di richieste ticket\n", shm_ptr->simulation_day);
        
        while (shm_ptr->day_in_progress && running)
        {
            // ZERO ATTESA ATTIVA: msgrcv bloccante che aspetta istantaneamente i messaggi
            TicketRequestMsg msg;
            ssize_t msgsize;
            
            // Attesa BLOCCANTE sui messaggi - il processo si blocca fino all'arrivo di un messaggio
            // o fino a quando viene interrotto da un segnale (es. SIGUSR2 per fine giornata)
            msgsize = msgrcv(msgid, &msg, sizeof(msg) - sizeof(long),
                          MSG_TICKET_REQUEST, 0); // 0 = bloccante, nessun IPC_NOWAIT
            
            if (msgsize > 0) 
            {
                //printf("Ticket: Ricevuta richiesta da utente %d (PID: %d) per servizio %s\n",
                //      msg.user_id, msg.user_pid, SERVICE_NAMES[msg.service_id]);
                      
                if (shm_ptr->day_in_progress) {
                    // Richiesta ricevuta - processa immediatamente
                    process_new_ticket_request(&msg);
                    
                    // Segnala il completamento dell'elaborazione
                    //printf("Ticket: Richiesta elaborata per utente %d\n", msg.user_id);
                } else {
                    // La giornata è finita mentre elaboravamo la richiesta
                    //printf("Ticket: Giornata terminata durante l'elaborazione della richiesta per utente %d\n", 
                    //      msg.user_id);
                    
                    // Segnala comunque all'utente che la richiesta è stata ricevuta ma non elaborata
                    if (msg.user_pid > 0) {
                        kill(msg.user_pid, SIGUSR2);
                    }
                }
            }
            else if (errno == EINTR) 
            {
                // Interrotto da un segnale (es. SIGUSR2 per fine giornata) - comportamento normale
                // Il loop ricontrollerà la condizione day_in_progress
            }
            else if (errno != ENOMSG) 
            {
                // Errore diverso da "nessun messaggio" o interruzione da segnale
                perror("Ticket: msgrcv failed");
            }
        }

        //printf("Ticket: Day %d completed\n", shm_ptr->simulation_day);
    }

    //printf("Ticket process terminating...\n");

    if (shm_ptr != NULL && shm_ptr != (void *)-1)
    {
        shm_ptr->ticket_pid = 0; // Pulisce il nostro PID dalla memoria condivisa
        shmdt(shm_ptr);
    }

    //printf("Ticket process terminated\n");
    return 0;
}