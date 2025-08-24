#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>
#include "config.h"
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <string.h>
#include <errno.h>


int user_id;
SharedMemory *shm_ptr = NULL;
volatile int day_started = 0;
volatile int simulation_active = 1;
int semid = -1;

// Variabile globale per la coda di messaggi
static int msgid = -1;

void cleanup_resources() {
    // Stacca la memoria condivisa
    if (shm_ptr != NULL && shm_ptr != (void *)-1) {
        shmdt(shm_ptr);
    }
}

// Handler per la terminazione della simulazione
void end_simulation_handler(int signum __attribute__((unused)))
{
    simulation_active = 0;
    cleanup_resources();
    exit(EXIT_SUCCESS);
}

// Restituisce se l'utente arriva (e se arriva il servizio scelto)
int determine_arrival_and_service(int personal_probability)
{
    // Lancia un dado da 1 a 100 per decidere se l'utente arriva
    int arrival_roll = (rand() % 100) + 1;

    // Se il tiro è maggiore della probabilità personale, l'utente non arriva
    if (arrival_roll > personal_probability)
    {
        return -1; // -1 indica che l'utente non si presenta
    }

    // Se l'utente arriva, determina quale servizio desidera
    int chosen_service = rand() % SERVICE_COUNT;
    return chosen_service;
}

// Calcola la probabilità di arrivo personale per l'utente
int calculate_personal_probability()
{
    if (P_SERV_MAX > 0 && P_SERV_MIN <= P_SERV_MAX)
    {
        return P_SERV_MIN + (rand() % (P_SERV_MAX - P_SERV_MIN + 1));
    }
    return 0;
}

// Calcola l'orario di arrivo in minuti (tra 0 e 479)
int determine_arrival_time()
{
    return 1 + (rand() % (OFFICE_CLOSE_TIME - 1));
}

// Converti minuti simulati in secondi reali
double minutes_to_simulation_seconds(int minutes)
{
    return ((double)minutes / WORK_DAY_MINUTES) * DAY_SIMULATION_TIME;
}

// Funzione per richiedere un ticket dal processo di gestione ticket
int request_ticket(int user_id, int service_id)
{
    // Verifica che il processo ticket sia attivo
    if (shm_ptr == NULL || shm_ptr->ticket_pid <= 0)
    {
        return -1;
    }

    // Otteniamo accesso alla coda messaggi se non già fatto
    if (msgid == -1)
    {
        msgid = msgget(MSG_QUEUE_KEY, 0666);
        if (msgid == -1)
        {
            perror("User: msgget failed");
            return -1;
        }
    }

    // Acquisire il mutex per creare la richiesta in shared memory
    struct sembuf sem_op;
    sem_op.sem_num = SEM_QUEUE;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;

    if (semop(semid, &sem_op, 1) < 0)
    {
        perror("Failed to acquire queue mutex");
        return -1;
    }

    // Otteniamo l'indice di richiesta corrente e lo incrementiamo
    int request_index = shm_ptr->next_request_index;
    if (request_index >= MAX_REQUESTS) {
        // Se abbiamo raggiunto il limite, rilasciamo il mutex e restituiamo errore
        sem_op.sem_op = 1; // Unlock
        semop(semid, &sem_op, 1);
        return -1;
    }
    
    // Incrementa next_request_index
    shm_ptr->next_request_index++;

    // Crea la richiesta in shared memory
    TicketRequest *request = &(shm_ptr->ticket_requests[request_index]);
    request->user_id = user_id;
    request->service_id = service_id;
    request->status = REQUEST_PENDING;
    clock_gettime(CLOCK_MONOTONIC, &request->request_time);
    request->ticket_number = 0;
    memset(request->ticket_id, 0, sizeof(request->ticket_id));

    // Rilascio del mutex
    sem_op.sem_op = 1; // Unlock
    if (semop(semid, &sem_op, 1) < 0)
    {
        perror("Failed to release queue mutex");
        return -1;
    }

    // Ora invia il messaggio al processo ticket con il request_index
    TicketRequestMsg msg;
    msg.mtype = MSG_TICKET_REQUEST;
    msg.user_id = user_id;
    msg.service_id = service_id;
    msg.request_index = request_index;  // Passa l'indice della richiesta
    msg.request_time = time(NULL);
    msg.user_pid = getpid();

    // Invia il messaggio alla coda
    if (msgsnd(msgid, &msg, sizeof(msg) - sizeof(long), 0) == -1)
    {
        perror("User: msgsnd failed");
        return -1;
    }

    return request_index;
}

// Gestione della visita all'ufficio postale
int handle_post_office_visit(int user_id, int service_id, int request_index)
{
    if (shm_ptr == NULL)
    {
        int shmid = shmget(SHM_KEY, sizeof(SharedMemory), 0666);
        if (shmid == -1)
        {
            perror("User: shmget failed");
            return -1;
        }

        shm_ptr = (SharedMemory *)shmat(shmid, NULL, 0);
        if (shm_ptr == (void *)-1)
        {
            perror("User: shmdt failed");
            return -1;
        }
    }

    // Registra un signal handler temporaneo per SIGUSR1 come notifica da ticket
    struct sigaction sa_ticket;
    memset(&sa_ticket, 0, sizeof(sa_ticket));
    sa_ticket.sa_handler = SIG_IGN; // Solo per impostare l'handler (saremo in attesa esplicita)
    sigemptyset(&sa_ticket.sa_mask);
    sa_ticket.sa_flags = 0;
    
    if (sigaction(SIGUSR1, &sa_ticket, NULL) == -1) {
        perror("User: sigaction for ticket signal failed");
    }
    
    // Prepara il set di segnali per sigtimedwait
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, SIGUSR1); // Segnale che indica ticket processato
    sigaddset(&wait_set, SIGUSR2); // Segnale di fine giornata
    
    // Blocca temporaneamente questi segnali per usare sigwait
    sigprocmask(SIG_BLOCK, &wait_set, NULL);
    
    // Imposta un timeout
    struct timespec timeout;
    timeout.tv_sec = 0;
    timeout.tv_nsec = 200000000; // 200ms
    
    // Loop di attesa principale - Ora utilizza SIGUSR1 per la notifica dal ticket e SIGUSR2 per fine giornata
    while (shm_ptr->day_in_progress && simulation_active)
    {
        // Attendere un segnale o il timeout
        int sig = sigtimedwait(&wait_set, NULL, &timeout);
        
        // Controlla lo stato della richiesta dopo il segnale o il timeout
        if (shm_ptr->ticket_requests[request_index].status == REQUEST_COMPLETED)
        {
            // Sblocca i segnali prima di uscire
            sigprocmask(SIG_UNBLOCK, &wait_set, NULL);
            return 0;  // Success
        }
        else if (shm_ptr->ticket_requests[request_index].status == REQUEST_REJECTED)
        {
            printf("\t[UTENTE %d] Richiesta ticket rifiutata\n", user_id);
            // Sblocca i segnali prima di uscire
            sigprocmask(SIG_UNBLOCK, &wait_set, NULL);
            return -1;  // Request rejected
        }
        
        if (sig == SIGUSR2 || !shm_ptr->day_in_progress) {
            // La giornata è finita, esci dal loop
            break;
        }
    }
   
    // Aggiorna il contatore degli utenti non serviti per mancanza di tempo
    struct sembuf sem_op;
    sem_op.sem_num = SEM_MUTEX;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;
    
    if (semop(semid, &sem_op, 1) == 0) {
        // Se la richiesta era in elaborazione ma non completata (utente non ha mai ricevuto il ticket)
        if (shm_ptr->ticket_requests[request_index].status == REQUEST_PENDING ||
            shm_ptr->ticket_requests[request_index].status == REQUEST_PROCESSING) {
            shm_ptr->daily_users_no_ticket[service_id]++;
            shm_ptr->total_users_no_ticket++;
            
        } else {
            // La richiesta non era nemmeno iniziata o è stata esplicitamente rifiutata
            printf("\t[UTENTE %d] Richiesta non elaborata o rifiutata alla fine della giornata\n", user_id);
        }
        
        // Rilascia il mutex
        sem_op.sem_op = 1; // Unlock
        semop(semid, &sem_op, 1);
    }
    
    // Sblocca i segnali prima di uscire
    sigprocmask(SIG_UNBLOCK, &wait_set, NULL);
    return -1;
}

// Funzione per verificare se un servizio è effettivamente disponibile (sportello + operatore)
int is_service_available(SharedMemory *shm, int service_id)
{
    // Itera su tutti gli sportelli
    for (int i = 0; i < NOF_WORKER_SEATS; i++)
    {
        // Controlla se lo sportello è attivo e assegnato al servizio richiesto
        if (shm->counters[i].active && shm->counters[i].current_service == (ServiceType)service_id)
        {
            // Se lo sportello è attivo per il servizio, controlla se c'è un operatore assegnato
            if (shm->counters[i].operator_pid > 0)
            {
                // Itera su tutti gli operatori per trovare quello assegnato
                for (int j = 0; j < NOF_WORKERS; j++)
                {
                    // Controlla se l'operatore è attivo e corrisponde al PID nello sportello
                    if (shm->operators[j].active && shm->operators[j].pid == shm->counters[i].operator_pid)
                    {
                        // L'assegnazione dello sportello implica la competenza, quindi se troviamo
                        // un operatore attivo assegnato, il servizio è disponibile.
                        return 1; // Trovato sportello attivo con operatore.
                    }
                }
            }
        }
    }
    return 0; // Nessuno sportello attivo con operatore trovato per questo servizio.
}

// Refactored function to increment stats for users who go home without service
void increment_users_home_stats(int service_id) {
    struct sembuf sem_op;
    sem_op.sem_num = SEM_MUTEX;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;
    
    if (semop(semid, &sem_op, 1) == 0) {
        shm_ptr->daily_users_home[service_id]++;
        shm_ptr->total_users_home++;
        
        // Rilascia il mutex
        sem_op.sem_op = 1; // Unlock
        semop(semid, &sem_op, 1);
    }
}

// Refactored function to wait for a signal with a timeout
void wait_for_signal(int signum, volatile int *condition) {
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, signum);
    sigaddset(&wait_set, SIGTERM);

    sigset_t old_mask;
    sigprocmask(SIG_BLOCK, &wait_set, &old_mask);

    while (*condition && simulation_active) {
        struct timespec timeout = {.tv_sec = 0, .tv_nsec = 500000000}; // 500ms
        int sig = sigtimedwait(&wait_set, NULL, &timeout);

        if (sig == signum || !(*condition)) {
            break;
        } else if (sig == SIGTERM) {
            simulation_active = 0;
            break;
        }
    }
    sigprocmask(SIG_SETMASK, &old_mask, NULL);
}


int main(int argc, char *argv[])
{
    // Get user ID from argument (passed by director)
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <user_id>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    user_id = atoi(argv[1]);

    // Seed the random number generator ONCE per user process for unique behavior.
    srand(getpid() ^ time(NULL));

    // Determine this user's personal probability of visiting the post office.
    int personal_arrival_probability = calculate_personal_probability();

    
    // Set up signal handlers.
    // We register SIG_IGN to prevent the default termination action for SIGUSR1/SIGUSR2.
    // The signals are handled synchronously by sigtimedwait in the main loop.
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGTERM, end_simulation_handler); // Signal to end simulation

    // Connect to shared memory
    int shmid = shmget(SHM_KEY, sizeof(SharedMemory), 0666);
    if (shmid == -1)
    {
        perror("User: shmget failed");
        exit(EXIT_FAILURE);
    }

    shm_ptr = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shm_ptr == (void *)-1)
    {
        perror("User: shmat failed");
        exit(EXIT_FAILURE);
    }

    // Connect to semaphores
    semid = semget(SEM_KEY, NUM_SEMS, 0666);
    if (semid == -1)
    {
        perror("User: semget failed");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // Seed the random number generator once per process
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    srand(ts.tv_nsec ^ getpid() ^ shm_ptr->simulation_day);

    // Main simulation loop - runs for the entire simulation
    while (simulation_active)
    {
        // Reset day_started all'inizio di ogni ciclo
        day_started = 0;
        
        // Wait for day start signal (SIGUSR1)
        int day_in_progress_flag = !shm_ptr->day_in_progress;
        wait_for_signal(SIGUSR1, &day_in_progress_flag);
        
        if (!simulation_active) break;
        
        // Dopo aver ricevuto il segnale, attendi anche sul semaforo SEM_DAY_START
        struct sembuf sem_wait;
        sem_wait.sem_num = SEM_DAY_START;
        sem_wait.sem_op = -1;  // Operazione P (wait)
        sem_wait.sem_flg = 0;
        
        //printf("\t[UTENTE %d] Attendo sulla barriera semaforo SEM_DAY_START...\n", user_id);
        
        // Attendi sul semaforo
        if (semop(semid, &sem_wait, 1) < 0) {
            if (errno != EINTR) {  // Ignora se interrotto da segnale
                perror("User: semop wait for day start failed");
            }
        }
        
        // Set day_started flag
        day_started = 1;
        
        // Start timing
        struct timeval day_start;
        gettimeofday(&day_start, NULL);
        
        //printf("\t[UTENTE %d] Giornata %d iniziata\n", user_id, shm_ptr->simulation_day);

        // Decide se l'utente si presenta e per quale servizio
        int service_id = determine_arrival_and_service(personal_arrival_probability);

        // Se service_id è >= 0, l'utente ha deciso di visitare l'ufficio postale
        if (service_id >= 0)
        {
            int arrival_minute = determine_arrival_time();
            double arrival_seconds = minutes_to_simulation_seconds(arrival_minute);

            //printf("\t\t\t\t\t\t\t\t[UTENTE %d] Scheduled to arrive at minute %d on day %d for service: %s\n",
            //       user_id, arrival_minute, shm_ptr->simulation_day, SERVICE_NAMES[service_id]);

            // La logica del resto della giornata rimane la stessa...
            int visited = 0;
            while (shm_ptr->day_in_progress && simulation_active && !visited)
            {
                // Controlla l'ora di simulazione corrente
                struct timeval current_time;
                gettimeofday(&current_time, NULL);
                double elapsed_seconds = (current_time.tv_sec - day_start.tv_sec) +
                                         (current_time.tv_usec - day_start.tv_usec) / 1000000.0;

                // Se è ora di arrivare all'ufficio postale
                if (elapsed_seconds >= arrival_seconds)
                {
                    // Controlla se il servizio è realmente disponibile (sportello E operatore)
                    if (!is_service_available(shm_ptr, service_id))
                    {
                        // Incrementa il contatore degli utenti tornati a casa
                        // perché il servizio non era disponibile (es. nessuno sportello o nessun operatore)
                        increment_users_home_stats(service_id);
                        visited = 1; // Segna come "visitato" anche se non ha ottenuto servizio
                    }
                    else
                    {
                        // Se il servizio è disponibile, procede con la richiesta del ticket
                        int request_index = request_ticket(user_id, service_id);

                        // Gestisce la visita all'ufficio postale
                        if (request_index >= 0)
                        {
                            int result = handle_post_office_visit(user_id, service_id, request_index);
                            
                            if (result < 0)
                            {
                                visited = 1; // Termina la visita anche in caso di errore
                                
                                // Se la giornata è finita durante l'elaborazione del ticket
                                if (!shm_ptr->day_in_progress) {
                                    printf("\t\t\t\t\t\t[UTENTE %d] Non è stato possibile ricevere il biglietto per il servizio %s perché il giorno è terminato\n",
                                           user_id, SERVICE_NAMES[service_id]);
                                }
                            }
                            else
                            {
                                // L'utente ha ricevuto il ticket con successo
                                // NON incrementiamo daily_tickets_served qui - sarà fatto dall'operatore
                                // quando completa effettivamente il servizio
                                visited = 1;
                            }
                        }
                        else
                        {
                            visited = 1; // Termina la visita anche in caso di errore
                            
                            // Se request_ticket fallisce, contiamo l'utente come "tornato a casa"
                            printf("\t[UTENTE %d] Errore nella richiesta del ticket per %s. Torno a casa.\n", 
                                   user_id, SERVICE_NAMES[service_id]);
                            
                            increment_users_home_stats(service_id);
                        }
                    }
                }

                // Attendi un po' prima di controllare di nuovo
                usleep(5000); // Sleep for 5ms
            }

            // TRACCIAMENTO UTENTI MANCANTI: Se l'utente non è mai riuscito a visitare l'ufficio postale
            // (cioè visited = 0), significa che è arrivato troppo tardi e la giornata è finita
            // prima che potesse arrivare. Contiamolo come "tornato a casa"
            if (!visited && day_started) {
                printf("\t[UTENTE %d] Non sono riuscito ad arrivare in tempo (arrivo previsto: minuto %d). Torno a casa.\n", 
                       user_id, arrival_minute);
                
                // Incrementa il contatore degli utenti tornati a casa per mancanza di tempo
                increment_users_home_stats(service_id);
            }
        }
        else
        {
            // L'utente ha deciso di non visitare l'ufficio oggi
            //printf("\t[UTENTE %d] Oggi non vado all'ufficio postale.\n", user_id);
        }
        
        // TRACCIAMENTO AGGIUNTIVO: Se l'utente non ha mai iniziato la giornata
        if (!day_started && simulation_active) {
            printf("\t[UTENTE %d] La giornata non è mai iniziata per me. Conteggiato come non servito.\n", user_id);
            
            // Incrementa il contatore degli utenti tornati a casa
            increment_users_home_stats(service_id);
        }

        // Wait for day end signal (SIGUSR2) using sigwait
        if (shm_ptr->day_in_progress && simulation_active) {
            // Crea un set di segnali per attendere SIGUSR2 e SIGTERM
            sigset_t day_end_set;
            sigemptyset(&day_end_set);
            sigaddset(&day_end_set, SIGUSR2);
            sigaddset(&day_end_set, SIGTERM);
            
            // Blocca i segnali per poterli catturare con sigwait
            sigset_t old_mask;
            sigprocmask(SIG_BLOCK, &day_end_set, &old_mask);
            
            // Attendi il segnale o il cambio della variabile day_in_progress
            while (shm_ptr->day_in_progress && simulation_active) {
                int sig;
                struct timespec timeout;
                timeout.tv_sec = 0;
                timeout.tv_nsec = 200000000;  // 200 ms (più reattivo)
                
                // Attendi il segnale con timeout
                sig = sigtimedwait(&day_end_set, NULL, &timeout);
                
                if (sig == SIGUSR2 || !shm_ptr->day_in_progress) {
                    day_started = 0;
                    break;
                }
                else if (sig == SIGTERM) {
                    // Gestisci la terminazione
                    simulation_active = 0;
                    break;
                }
            }
            
            // Ripristina la maschera di segnali precedente
            sigprocmask(SIG_SETMASK, &old_mask, NULL);
        }
    }

    // Detach from shared memory before exiting
    cleanup_resources();

    //printf("\t[UTENTE %d] Process terminated\n", user_id);
    return 0;
}