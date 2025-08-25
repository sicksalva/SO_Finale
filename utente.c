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

// Gestore per la terminazione della simulazione
void end_simulation_handler(int signum __attribute__((unused)))
{
    simulation_active = 0;
    cleanup_resources();
    exit(EXIT_SUCCESS);
}

// Determina se l'utente arriva (e se arriva il servizio scelto)
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
    //return 1;
    return 1 + (rand() % (OFFICE_CLOSE_TIME - 1));
}

// Converti minuti simulati in secondi reali di simulazione
double minutes_to_simulation_seconds(int minutes)
{
    return ((double)minutes / WORK_DAY_MINUTES) * DAY_SIMULATION_TIME;
}

// Funzione per richiedere un ticket dal processo di gestione ticket
int request_ticket(int user_id, int service_id)
{
    // Verifica se il processo ticket è attivo
    if (shm_ptr == NULL || shm_ptr->ticket_pid <= 0)
    {
        return -1;
    }

    // Accesso alla coda di messaggi
    if (msgid == -1)
    {
        msgid = msgget(MSG_QUEUE_KEY, 0666);
        if (msgid == -1)
        {
            perror("User: msgget failed");
            return -1;
        }
    }

    // Acquisisce il mutex per creare la richiesta in memoria condivisa
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
        // Limite massimo di richieste raggiunto, errore e unlock
        sem_op.sem_op = 1; // Unlock
        semop(semid, &sem_op, 1);
        return -1;
    }
    
    // Incrementa l'indice per la prossima richiesta
    shm_ptr->next_request_index++;

    // Crea la richiesta in shared memory
    TicketRequest *request = &(shm_ptr->ticket_requests[request_index]);
    request->user_id = user_id;         // ID dell'utente
    request->service_id = service_id;   // Servizio richiesto
    request->status = REQUEST_PENDING;  // Indica che è in attesa del ticket
    clock_gettime(CLOCK_MONOTONIC, &request->request_time); // Per statistiche
    request->ticket_number = 0;
    memset(request->ticket_id, 0, sizeof(request->ticket_id)); // Inizializza a vuoto

    // Rilascio del mutex
    sem_op.sem_op = 1; // Unlock
    if (semop(semid, &sem_op, 1) < 0)
    {
        perror("Failed to release queue mutex");
        return -1;
    }

    usleep(1000); // FIX per race condition

    // Invia il messaggio di richiesta ticket
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

    // Registra un gestore di segnale temporaneo per SIGUSR1 come notifica da ticket
    struct sigaction sa_ticket;
    memset(&sa_ticket, 0, sizeof(sa_ticket));
    sa_ticket.sa_handler = SIG_IGN; // Imposta l'handler a SIG_IGN per evitare la terminazione di default
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
    
    // Loop principale: attende SIGUSR1 (ticket) o SIGUSR2 (fine giornata)
    while (shm_ptr->day_in_progress && simulation_active)
    {
        // Attende un segnale o il timeout
        int sig = sigtimedwait(&wait_set, NULL, &timeout);
        
        // Controlla lo stato della richiesta dopo il segnale o il timeout
        if (shm_ptr->ticket_requests[request_index].status == REQUEST_COMPLETED)
        {
            // Ricevuto il ticket con successo (rimuove mascheramento)
            sigprocmask(SIG_UNBLOCK, &wait_set, NULL);
            return 0; 
        }
        else if (shm_ptr->ticket_requests[request_index].status == REQUEST_REJECTED)
        {
            printf("\t[UTENTE %d] Richiesta ticket rifiutata\n", user_id);
            sigprocmask(SIG_UNBLOCK, &wait_set, NULL);
            return -1;
        }

        // La giornata è finita, esci dal loop
        if (sig == SIGUSR2 || !shm_ptr->day_in_progress) {
            break;
        }
    }
   
    // Break alla riga 228 -> contiamo come non servito (e non come tornato a casa)
    struct sembuf sem_op;
    sem_op.sem_num = SEM_MUTEX;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;
    
    if (semop(semid, &sem_op, 1) == 0) {
        // Utente non ha ricevuto il ticket entro la fine della giornata
        if (shm_ptr->ticket_requests[request_index].status == REQUEST_PENDING ||
            shm_ptr->ticket_requests[request_index].status == REQUEST_PROCESSING) {
            shm_ptr->daily_users_no_ticket[service_id]++;
            shm_ptr->total_users_no_ticket++;
        } else {
            printf("\t[UTENTE %d] Richiesta non elaborata o rifiutata alla fine della giornata\n", user_id);
        }
        
        // Rilascia il mutex
        sem_op.sem_op = 1; // Unlock
        semop(semid, &sem_op, 1);
    }
    
    // Sblocca i segnali prima di uscire (in caso di break dal loop)
    sigprocmask(SIG_UNBLOCK, &wait_set, NULL);
    return -1;
}

// Verifica disponibilità del servizio (sportello + operatore)
int is_service_available(SharedMemory *shm, int service_id)
{
    for (int i = 0; i < NOF_WORKER_SEATS; i++)
    {
        // Controlla se lo sportello è attivo
        if (shm->counters[i].active && shm->counters[i].current_service == (ServiceType)service_id)
        {
            // Controlla se c'è un operatore attivo
            if (shm->counters[i].operator_pid > 0)
            {
                for (int j = 0; j < NOF_WORKERS; j++)
                {
                    if (shm->operators[j].active && shm->operators[j].pid == shm->counters[i].operator_pid)
                    {
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

// Incrementa conteggio utenti tornati a casa senza servizio
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

// Incrementa conteggio utenti che non si sono presentati all'ufficio postale
void increment_users_not_arrived_stats() {
    struct sembuf sem_op;
    sem_op.sem_num = SEM_MUTEX;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;
    
    if (semop(semid, &sem_op, 1) == 0) {
        // Non sappiamo quale servizio avrebbe scelto, quindi incrementiamo un servizio casuale
        int random_service = rand() % SERVICE_COUNT;
        shm_ptr->daily_users_not_arrived[random_service]++;
        shm_ptr->total_users_not_arrived++;
        shm_ptr->total_users_not_arrived_per_service[random_service]++;
        
        // Rilascia il mutex
        sem_op.sem_op = 1; // Unlock
        semop(semid, &sem_op, 1);
    }
}

// Funzione di attesa per un segnale specifico con timeout
void wait_for_signal(int signum, volatile int *condition) {
    sigset_t wait_set;
    sigemptyset(&wait_set);
    sigaddset(&wait_set, signum);
    sigaddset(&wait_set, SIGTERM);

    sigset_t old_mask;
    sigprocmask(SIG_BLOCK, &wait_set, &old_mask);

    // Condition è day_in_progress_flag
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
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <user_id>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    user_id = atoi(argv[1]);

    // Seme per il generatore di numeri casuali
    srand(getpid() ^ time(NULL));

    int personal_arrival_probability = calculate_personal_probability();

    // Impostazione gestori di segnale (altrimenti esegue kill)
    signal(SIGUSR1, SIG_IGN);
    signal(SIGUSR2, SIG_IGN);
    signal(SIGTERM, end_simulation_handler);

    // Connessione alla memoria condivisa
    int shmid = shmget(SHM_KEY, sizeof(SharedMemory), 0666);
    if (shmid == -1)
    {
        perror("User: shmget failed");
        exit(EXIT_FAILURE);
    }

    // Attacca la memoria condivisa
    shm_ptr = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shm_ptr == (void *)-1)
    {
        perror("User: shmat failed");
        exit(EXIT_FAILURE);
    }

    // Accesso ai semafori
    semid = semget(SEM_KEY, NUM_SEMS, 0666);
    if (semid == -1)
    {
        perror("User: semget failed");
        shmdt(shm_ptr);
        exit(EXIT_FAILURE);
    }

    // ------------------------------------------------------------------------------
    // Simulazione principale
    // ------------------------------------------------------------------------------

    while (simulation_active)
    {
        day_started = 0;
        
        // Attende il segnale di inizio giornata (SIGUSR1) dal direttore
        int day_in_progress_flag = !shm_ptr->day_in_progress;
        wait_for_signal(SIGUSR1, &day_in_progress_flag);
        
        if (!simulation_active) break;
        
        // Semaforo per iniziare la giornata
        struct sembuf sem_wait;
        sem_wait.sem_num = SEM_DAY_START;
        sem_wait.sem_op = -1;
        sem_wait.sem_flg = 0;
        
        // Attendi sul semaforo
        if (semop(semid, &sem_wait, 1) < 0) {
            if (errno != EINTR) {  // Ignora se interrotto da segnale
                perror("User: semop wait for day start failed");
            }
        }
        
        day_started = 1;
        
        // Inizia il timer per la giornata
        struct timeval day_start;
        gettimeofday(&day_start, NULL);
        
        int service_id = determine_arrival_and_service(personal_arrival_probability);

        // Se service_id è >= 0, l'utente ha deciso di visitare l'ufficio postale
        if (service_id >= 0)
        {
            int arrival_minute = determine_arrival_time();
            double arrival_seconds = minutes_to_simulation_seconds(arrival_minute);

            // DEBUG: stampa info arrivo
            //printf("\t\t\t\t\t\t\t\t[UTENTE %d] Scheduled to arrive at minute %d on day %d for service: %s\n",       user_id, arrival_minute, shm_ptr->simulation_day, SERVICE_NAMES[service_id]);

            int visited = 0;

            // ------------------------------------------------------------------------------
            // Loop GIORNATA (non simulazione)
            // ------------------------------------------------------------------------------

            while (shm_ptr->day_in_progress && simulation_active && !visited)
            {
                // Controlla tempo trascorso
                struct timeval current_time;
                gettimeofday(&current_time, NULL);
                double elapsed_seconds = (current_time.tv_sec - day_start.tv_sec) +
                                         (current_time.tv_usec - day_start.tv_usec) / 1000000.0;

                // Verifica se è ora di arrivare
                if (elapsed_seconds >= arrival_seconds)
                {
                    if (!is_service_available(shm_ptr, service_id))
                    {
                        // Utente tornato a casa
                        increment_users_home_stats(service_id);
                        visited = 1;
                    }
                    else
                    {
                        int request_index = request_ticket(user_id, service_id);

                        if (request_index >= 0)
                        {
                            int result = handle_post_office_visit(user_id, service_id, request_index);
                            
                            if (result < 0)
                            {
                                // Ramo errore
                                visited = 1;
                                
                                if (!shm_ptr->day_in_progress) {
                                    printf("[UTENTE %d] Non è stato possibile ricevere il biglietto per il servizio %s perché il giorno è terminato\n", user_id, SERVICE_NAMES[service_id]);
                                }
                            }
                            else
                            {
                                // Ramo successo
                                visited = 1;
                            }
                        }
                        else
                        {
                            visited = 1;
                            // DEBUG: stampa errore richiesta ticket
                            //printf("\t[UTENTE %d] Errore nella richiesta del ticket per %s. Torno a casa.\n", user_id, SERVICE_NAMES[service_id]);
                            
                            increment_users_home_stats(service_id);
                        }
                    }
                }

                // Attende un po' prima di controllare di nuovo
                usleep(5000); 
            }

            if (!visited && day_started) {
                printf("\t[UTENTE %d] Non sono riuscito ad arrivare in tempo (arrivo previsto: minuto %d). Torno a casa.\n", 
                       user_id, arrival_minute);

                increment_users_home_stats(service_id);
            }
        }
        else
        {
            // L'utente ha deciso di non presentarsi all'ufficio postale
            // DEBUG: stampa informazioni di non arrivo
            //printf("\t[UTENTE %d] Oggi non vado all'ufficio postale.\n", user_id);
            
            // Incrementa il contatore degli utenti che non si sono presentati
            increment_users_not_arrived_stats();
        }
        
        // Segnale di inizio giornata mai ricevuto
        if (!day_started && simulation_active) {
            printf("[UTENTE %d] La giornata non è mai iniziata per me. Conteggiato come non servito.\n", user_id);
            
            increment_users_home_stats(service_id);
        }

        // Attende il segnale di fine giornata (SIGUSR2) dal direttore
        if (shm_ptr->day_in_progress && simulation_active) {
            
            sigset_t day_end_set;
            sigemptyset(&day_end_set);
            sigaddset(&day_end_set, SIGUSR2);
            sigaddset(&day_end_set, SIGTERM);
            sigset_t old_mask;
            sigprocmask(SIG_BLOCK, &day_end_set, &old_mask);
            
            // Aspetta segnale di fine giornata o terminazione
            while (shm_ptr->day_in_progress && simulation_active) {
                int sig;
                struct timespec timeout;
                timeout.tv_sec = 0;
                timeout.tv_nsec = 20000000;  // 20 ms
                
                sig = sigtimedwait(&day_end_set, NULL, &timeout);
                
                if (sig == SIGUSR2 || !shm_ptr->day_in_progress) {
                    day_started = 0;
                    break;
                }
                else if (sig == SIGTERM) {
                    // Gestisce la terminazione
                    simulation_active = 0;
                    break;
                }
            }
            
            sigprocmask(SIG_SETMASK, &old_mask, NULL);
        }
    }

    cleanup_resources();
    return 0;
}