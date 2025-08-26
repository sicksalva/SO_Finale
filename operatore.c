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
#include <errno.h>

SharedMemory *shm_ptr = NULL;
int operator_id;
int semid = -1;
int running = 1;
int day_in_progress = 0;

// Servizio assegnato casualmente all'operatore (FISSO)
ServiceType random_service; 

// Gestisce gli interrupt dei semafori
int safe_semop(int semid, struct sembuf *sops, size_t nsops) {
    int result;
    while ((result = semop(semid, sops, nsops)) < 0) {
        if (errno == EINTR) {
            // Interruzione da segnale
            continue;
        } else {
            // Errore reale
            return -1;
        }
    }
    return result;
}

// Assegna operatore a sportello (PAUSA)
void try_assign_available_operators()
{
    // Acquisire il mutex per l'accesso agli sportelli e operatori
    struct sembuf sem_op;
    sem_op.sem_num = SEM_COUNTERS;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;
    
    // Errore acquisizione lock: esci
    if (semop(semid, &sem_op, 1) < 0) {
        return;
    }
    
    // Cerca sportelli liberi
    for (int counter_id = 0; counter_id < NOF_WORKER_SEATS; counter_id++) {
        if (shm_ptr->counters[counter_id].active && 
            shm_ptr->counters[counter_id].operator_pid == 0) {
            
            ServiceType counter_service = shm_ptr->counters[counter_id].current_service;
            
            // Cerca operatore compatibile
            for (int op_id = 0; op_id < NOF_WORKERS; op_id++) {
                if (shm_ptr->operators[op_id].active &&
                    shm_ptr->operators[op_id].current_service == counter_service &&
                    shm_ptr->operators[op_id].status == OPERATOR_WAITING) {
                    
                    // Assegna l'operatore allo sportello
                    shm_ptr->counters[counter_id].operator_pid = shm_ptr->operators[op_id].pid;
                    shm_ptr->operators[op_id].status = OPERATOR_WORKING;
                    
                    // DEBUG: Stampa riassegnazione
                    //printf("[RIASSEGNAZIONE] Operatore %d (PID %d) assegnato allo sportello %d per il servizio %s\n",op_id, shm_ptr->operators[op_id].pid, counter_id, SERVICE_NAMES[counter_service]);
                    
                    // Sveglia l'operatore con un segnale SIGUSR1
                    kill(shm_ptr->operators[op_id].pid, SIGUSR1);
                    break;
                }
            }
        }
    }
    
    // Rilascia il mutex
    sem_op.sem_op = 1; // Unlock
    semop(semid, &sem_op, 1);
}

// Calcola tempo di servizio con variazione casuale ±50%
long calculate_random_service_time(ServiceType service)
{
    // Cicla rand per migliorare casualità
    for (int i = 0; i < 3; i++) {
        rand();
    }

    long base_time = SERVICE_TIMES[service];

    // Calcola una variazione casuale tra -50% e +50%
    int rand_val = rand() % 101;
    double variation = (rand_val - 50) / 100.0; // Da -0.5 a +0.5

    long adjusted_time = base_time * (1.0 + variation);

    return adjusted_time;
}

// Funzione per servire un utente e gestire le pause
int serve_customer(int assigned_counter)
{
    // Verifica utenti in coda per il servizio dell'operatore
    if (shm_ptr->service_tickets_waiting[random_service] <= 0)
    {
        return 0; // Nessun utente da servire
    }

    // Verifica probabilità di pausa PRIMA di servire l'utente
    if (shm_ptr->total_pauses_simulation < NOF_PAUSE && (rand() % 100) < BREAK_PROBABILITY)
    {
        // Aggiorna la pausa se non ci sono utenti in coda
        struct sembuf sem_pause_stats;
        sem_pause_stats.sem_num = SEM_MUTEX;
        sem_pause_stats.sem_op = -1; // Lock
        sem_pause_stats.sem_flg = 0;

        if (semop(semid, &sem_pause_stats, 1) == 0) {
            // Ricontrolla dopo aver acquisito il mutex
            if (shm_ptr->total_pauses_simulation < NOF_PAUSE) {
                shm_ptr->operators[operator_id].total_pauses++;
                shm_ptr->total_pauses_simulation++;
                // Rilascia il mutex
                sem_pause_stats.sem_op = 1; // Unlock
                semop(semid, &sem_pause_stats, 1);

                // Pausa avviata (DEBUG)
                shm_ptr->operators[operator_id].status = OPERATOR_ON_BREAK;
                // Libera lo sportello
                shm_ptr->counters[assigned_counter].operator_pid = 0;
                try_assign_available_operators();
                return -1;
            } else {
                // Rilascia il mutex
                sem_pause_stats.sem_op = 1; // Unlock
                semop(semid, &sem_pause_stats, 1);
            }
        }
    }

    // Semaforo SPECIFICO per il servizio (lock esclusivo su utente da servire)
    struct sembuf sem_lock;
    sem_lock.sem_num = SEM_SERVICE_LOCK(random_service);
    sem_lock.sem_op = -1;
    sem_lock.sem_flg = SEM_UNDO;
    
    // Riprova ad acquisire il lock in caso di interruzione
    if (safe_semop(semid, &sem_lock, 1) < 0) {
        perror("[OPERATORE] Errore nell'acquisizione del lock per il ticket");
        return 0;
    }

    // Estrae ticket da servire dalla coda
    int ticket_idx = -1;
    TicketRequest *ticket = NULL;
    if (shm_ptr->service_tickets_waiting[random_service] > 0)
    {
        int head = shm_ptr->service_queue_head[random_service];
        ticket_idx = shm_ptr->service_queues[random_service][head];
        // Ottieni il puntatore al ticket corrispondente
        ticket = &shm_ptr->ticket_requests[ticket_idx];
        
        shm_ptr->service_queue_head[random_service] = (head + 1) % MAX_SERVICE_QUEUE;
        shm_ptr->service_tickets_waiting[random_service]--;
        
        // Rilascio immediato del semaforo dopo aver estratto il ticket
        struct sembuf sem_unlock_immediate;
        sem_unlock_immediate.sem_num = SEM_SERVICE_LOCK(random_service);
        sem_unlock_immediate.sem_op = 1;
        sem_unlock_immediate.sem_flg = 0;
        if (safe_semop(semid, &sem_unlock_immediate, 1) < 0) {
            perror("[OPERATORE] Errore nel rilascio immediato del lock per il ticket");
        }
    }
    else
    {
        // Nessun ticket disponibile, rilascia il lock
        struct sembuf sem_unlock;
        sem_unlock.sem_num = SEM_SERVICE_LOCK(random_service);
        sem_unlock.sem_op = 1;
        sem_unlock.sem_flg = 0;
        if (safe_semop(semid, &sem_unlock, 1) < 0) {
            perror("[OPERATORE] Errore nel rilascio del lock per il ticket");
        }
        return 0; // Nessun utente da servire
    }

    // Serve l'utente
    if (ticket_idx >= 0 && ticket_idx < MAX_REQUESTS)
    {
        if (ticket->being_served && ticket->serving_operator_pid != 0) {
            printf("[OPERATORE %d] L'utente #%d (Ticket: %s) è già in servizio dall'operatore PID %d\n",
                   operator_id, ticket->user_id, ticket->ticket_id, ticket->serving_operator_pid);
            
            // Riacquisisce semaforo per rimettere il ticket in coda
            struct sembuf sem_relock;
            sem_relock.sem_num = SEM_SERVICE_LOCK(random_service);
            sem_relock.sem_op = -1;
            sem_relock.sem_flg = 0;
            if (safe_semop(semid, &sem_relock, 1) < 0) {
                perror("[OPERATORE] Errore nel re-acquisire il lock per rimettere il ticket");
                return 0;
            }

            // Rimette il ticket in coda
            int tail = shm_ptr->service_queue_tail[random_service];
            shm_ptr->service_queues[random_service][tail] = ticket_idx;
            shm_ptr->service_queue_tail[random_service] = (tail + 1) % MAX_SERVICE_QUEUE;
            shm_ptr->service_tickets_waiting[random_service]++;
            
            // Rilascia il lock
            struct sembuf sem_unlock;
            sem_unlock.sem_num = SEM_SERVICE_LOCK(random_service);
            sem_unlock.sem_op = 1;
            sem_unlock.sem_flg = 0;
            if (safe_semop(semid, &sem_unlock, 1) < 0) {
                perror("[OPERATORE] Errore nel rilascio del lock dopo aver rimesso il ticket");
            }
            
            return 0; // Non serviamo l'utente
        }

        // Marca l'utente come in servizio da questo operatore
        ticket->being_served = 1;
        ticket->serving_operator_pid = getpid();

        // Calcola tempo di attesa (in nanosecondi)
        clock_gettime(CLOCK_MONOTONIC, &ticket->service_start_time);
        long wait_time_ns = (ticket->service_start_time.tv_sec - ticket->request_time.tv_sec) * 1000000000L +
                           (ticket->service_start_time.tv_nsec - ticket->request_time.tv_nsec);
        ticket->wait_time_ns = wait_time_ns;

        // DEBUG: Stampa tempo attesa
        //printf("🕐 [OPERATORE %d] Inizio servizio per utente #%d (Ticket: %s) - Attesa: %.1fms\n",       operator_id, ticket->user_id, ticket->ticket_id, wait_time_ns / 1000000.0);

        long service_time = calculate_random_service_time(random_service); 
        
        // Timestamp inizio tempo di servizio (millisecondi)
        struct timespec start_service_time;
        clock_gettime(CLOCK_MONOTONIC, &start_service_time);
        
        struct timeval start_time, end_time;
        gettimeofday(&start_time, NULL);

        // Prima di simulare il servizio, verifica se siamo ancora in una giornata lavorativa attiva
        if (!day_in_progress || !shm_ptr->day_in_progress)
        {
            // DEBUG: Stampa interruzione
            //printf("\t\t\t[OPERATORE %d] Giornata terminata mentre mi preparavo a servire l'utente #%d (Ticket: %s). L'utente dovrà attendere.\n",operator_id, ticket->user_id, ticket->ticket_id);
                   
            return 0; // Non serviamo l'utente
        }
        
        // Simulazione del servizio in modo preciso ma interrompibile solo da SIGUSR2
        
        long remaining_time_ns = service_time; // in nanosecondi
        const long check_interval_ns = 50000000L; // 50ms in nanosecondi
        
        volatile sig_atomic_t service_interrupted = 0;
        
        while (remaining_time_ns > 0 && !service_interrupted) {
            long sleep_time_ns = (remaining_time_ns > check_interval_ns) ? check_interval_ns : remaining_time_ns;
            
            struct timespec sleep_spec;
            sleep_spec.tv_sec = sleep_time_ns / 1000000000L;
            sleep_spec.tv_nsec = sleep_time_ns % 1000000000L;
            
            struct timespec remaining_spec;
            
            int result = nanosleep(&sleep_spec, &remaining_spec);
            
            if (result == 0) {
                // Sleep completato normalmente
                remaining_time_ns -= sleep_time_ns;
            } else {
                // Ricevuto segnale SIGTERM o SIGUSR2
                long actual_sleep_ns = sleep_time_ns - (remaining_spec.tv_sec * 1000000000L + remaining_spec.tv_nsec);
                remaining_time_ns -= actual_sleep_ns;
            }
            
            if (!day_in_progress || !shm_ptr->day_in_progress) {
                service_interrupted = 1;
                break;
            }
        }
        
        // Conteggio servizio interrotto (break alla riga 287)
        if (!day_in_progress || !shm_ptr->day_in_progress)
        {
            // DEBUG: Stampa interruzione
            //printf("[OPERATORE %d] Giornata terminata mentre stavo servendo l'utente #%d (Ticket: %s). Servizio interrotto.\n",operator_id, ticket->user_id, ticket->ticket_id);

            // Rilascia il lock dell'utente
            ticket->being_served = 0;
            ticket->serving_operator_pid = 0;

            return 0; // Servizio interrotto
        }

        // Calcola il tempo di servizio effettivo
        gettimeofday(&end_time, NULL);
        
        // Calcola fine servizio per le statistiche
        struct timespec end_service_time;
        clock_gettime(CLOCK_MONOTONIC, &end_service_time);
        long actual_service_time_ns = (end_service_time.tv_sec - start_service_time.tv_sec) * 1000000000L + 
                                     (end_service_time.tv_nsec - start_service_time.tv_nsec);

        struct sembuf sem_service_stats;
        sem_service_stats.sem_num = SEM_MUTEX;
        sem_service_stats.sem_op = -1;
        sem_service_stats.sem_flg = 0;
        
        if (semop(semid, &sem_service_stats, 1) == 0) {
            // Aggiorna tempo minimo
            if (actual_service_time_ns < shm_ptr->min_service_time[random_service]) {
                shm_ptr->min_service_time[random_service] = actual_service_time_ns;
            }
            
            // Aggiorna tempo massimo
            if (actual_service_time_ns > shm_ptr->max_service_time[random_service]) {
                shm_ptr->max_service_time[random_service] = actual_service_time_ns;
            }
            
            // Aggiorna tempo totale
            shm_ptr->total_service_time[random_service] += actual_service_time_ns;
            shm_ptr->service_count[random_service]++;
            
            // Rilascia il mutex
            sem_service_stats.sem_op = 1;
            semop(semid, &sem_service_stats, 1);
        }

        // Incrementa contatori
        shm_ptr->operators[operator_id].total_served++;
        
        shm_ptr->daily_tickets_served[random_service]++;
        shm_ptr->total_tickets_served++;
        
        shm_ptr->total_services_provided_simulation++;

        // Aggiorna le statistiche sui tempi di attesa
        struct sembuf sem_wait_stats;
        sem_wait_stats.sem_num = SEM_MUTEX;
        sem_wait_stats.sem_op = -1; // Lock
        sem_wait_stats.sem_flg = 0;
        
        if (semop(semid, &sem_wait_stats, 1) == 0) {
            // Aggiorna le statistiche sui tempi di attesa per questo servizio
            shm_ptr->total_wait_time[random_service] += ticket->wait_time_ns;
            shm_ptr->wait_count[random_service]++;
            
            // Aggiorna min/max per questo servizio
            if (shm_ptr->wait_count[random_service] == 1 || ticket->wait_time_ns < shm_ptr->min_wait_time[random_service]) {
                shm_ptr->min_wait_time[random_service] = ticket->wait_time_ns;
            }
            if (ticket->wait_time_ns > shm_ptr->max_wait_time[random_service]) {
                shm_ptr->max_wait_time[random_service] = ticket->wait_time_ns;
            }
            
            // Aggiorna le statistiche giornaliere sui tempi di attesa
            shm_ptr->daily_total_wait_time[random_service] += ticket->wait_time_ns;
            shm_ptr->daily_wait_count[random_service]++;
            
            // Aggiorna le statistiche aggregate per tutti i servizi
            shm_ptr->total_wait_time_all_services += ticket->wait_time_ns;
            shm_ptr->total_wait_count_all_services++;
            shm_ptr->daily_total_wait_time_all += ticket->wait_time_ns;
            shm_ptr->daily_wait_count_all++;
            
            // Rilascia il mutex
            sem_wait_stats.sem_op = 1;
            semop(semid, &sem_wait_stats, 1);
        }

        ticket->status = REQUEST_COMPLETED;
        ticket->counter_id = assigned_counter;
        ticket->served_successfully = 1;
        
        // Libera il lock dell'utente
        ticket->being_served = 0;
        ticket->serving_operator_pid = 0;

        // DEBUG: Tempo di servizio.
        //printf("[OPERATORE %d] Servito l'utente %d (Ticket: %s) per il servizio %s in %.3f secondi (%.1f minuti simulati) allo sportello %d\n",      operator_id, ticket->user_id, ticket->ticket_id, SERVICE_NAMES[random_service], service_duration_sec, service_duration_min, assigned_counter);
        
        return 1;
    }

    return 0; // Nessun utente valido da servire
}

// Gestore per la fine della giornata
void day_end_handler(int signum __attribute__((unused)))
{
    // Riceve segnale di fine giornata
    if (signum == SIGUSR2)
    {
        // Imposta la flag di giorno non più in corso
        day_in_progress = 0;
        
        // Imposta lo stato dell'operatore come finito per il giorno
        shm_ptr->operators[operator_id].status = OPERATOR_FINISHED;
        

        struct sembuf sem_op;
        sem_op.sem_num = SEM_COUNTERS;
        sem_op.sem_op = -1; // Lock
        sem_op.sem_flg = 0;
        
        if (semop(semid, &sem_op, 1) == 0) {
            // Libera sportello da operatore attivo
            for (int i = 0; i < NOF_WORKER_SEATS; i++) {
                if (shm_ptr->counters[i].operator_pid == getpid()) {
                    shm_ptr->counters[i].operator_pid = 0;
                    break;
                }
            }

            sem_op.sem_op = 1;
            semop(semid, &sem_op, 1);
        }
    }
}

// Gestore per l'inizio della giornata
void day_start_handler(int signum __attribute__((unused)))
{
    if (signum == SIGUSR1)
    {
        day_in_progress = 1;
        
        // Risveglia operatore in caso di pausa
        if (shm_ptr->operators[operator_id].status == OPERATOR_ON_BREAK) {
            shm_ptr->operators[operator_id].status = OPERATOR_WAITING;
        }
    }
}

// Gestore per la terminazione
void termination_handler(int signum __attribute__((unused)))
{
    running = 0;
}

// Assegna servizio casuale all'operatore (FISSO)
ServiceType assign_random_service()
{
    srand(time(NULL) ^ getpid());

    return rand() % SERVICE_COUNT;
}

// Inizializza l'operatore 
void initialize_operator(int op_id)
{
    // Assegna un servizio casuale
    random_service = assign_random_service();

    // Aggiorna le informazioni dell'operatore nella memoria condivisa
    shm_ptr->operators[op_id].pid = getpid();
    shm_ptr->operators[op_id].current_service = random_service;
    shm_ptr->operators[op_id].active = 1;
    shm_ptr->operators[op_id].total_served = 0;
    shm_ptr->operators[op_id].total_pauses = 0;
    shm_ptr->operators[op_id].status = OPERATOR_WAITING; // Inizia in attesa

    // DEBUG: Stampa informazioni operatore
    //printf("[OPERATORE %d] PID: %d, Servizio assegnato: %s (ID: %d)\n", op_id, getpid(), SERVICE_NAMES[random_service], random_service);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <operator_id>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Carica la configurazione dalla variabile d'ambiente
    const char* config_file = getenv("SO_CONFIG_FILE");
    if (!config_file) config_file = "timeout.conf"; // default
    read_config(config_file);

    // Identifica l'operatore
    operator_id = atoi(argv[1]);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned int seed = ts.tv_nsec ^ getpid() ^ (operator_id << 16) ^ time(NULL);
    srand(seed);

    // Collegamento alla memoria condivisa
    int shmid = shmget(SHM_KEY, sizeof(SharedMemory), 0666);
    if (shmid == -1)
    {
        perror("Operator: shmget failed");
        exit(EXIT_FAILURE);
    }

    // Attacca la memoria condivisa
    shm_ptr = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shm_ptr == (void *)-1)
    {
        perror("Operator: shmat failed");
        exit(EXIT_FAILURE);
    }

    // Ottieni l'ID del set di semafori
    semid = semget(SEM_KEY, NUM_SEMS, 0666);
    if (semid == -1)
    {
        perror("Operator: semget failed");
        exit(EXIT_FAILURE);
    }

    // Inizializza l'operatore
    initialize_operator(operator_id);

    // Imposta i gestori dei segnali
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    // Imposta handler per la fine della giornata (SIGUSR2)
    sa.sa_handler = day_end_handler;
    if (sigaction(SIGUSR2, &sa, NULL) == -1)
    {
        perror("Operator: sigaction SIGUSR2 failed");
        exit(EXIT_FAILURE);
    }

    // Imposta handler per l'inizio della giornata (SIGUSR1)
    sa.sa_handler = day_start_handler;
    if (sigaction(SIGUSR1, &sa, NULL) == -1)
    {
        perror("Operator: sigaction SIGUSR1 failed");
        exit(EXIT_FAILURE);
    }

    // Imposta handler per la terminazione (SIGTERM)
    sa.sa_handler = termination_handler;
    if (sigaction(SIGTERM, &sa, NULL) == -1)
    {
        perror("Operator: sigaction SIGTERM failed");
        exit(EXIT_FAILURE);
    }
    
    // Ciclo esterno per i giorni di simulazione
    while (running)
    {
        // Verifica se il giorno è già iniziato nella memoria condivisa
        if (shm_ptr->day_in_progress)
        {
            day_in_progress = 1;
        }

        // Aspetta segnale SIGUSR1 per iniziare
        if (!day_in_progress && running)
        {
            // Attesa bloccante sul semaforo
            struct sembuf sem_wait;
            sem_wait.sem_num = SEM_DAY_START;
            sem_wait.sem_op = -1;
            sem_wait.sem_flg = 0;
            
            if (semop(semid, &sem_wait, 1) == -1)
            {
                if (errno != EINTR) // Ignora se interrotto da segnale
                {
                    perror("Operator: semop wait for day start failed");
                }
            }
            else
            {
                day_in_progress = 1;
            }
        }

        if (!running)
            break; 

        // Salta se l'operatore è in pausa per tutta la giornata
        if (shm_ptr->operators[operator_id].status == OPERATOR_ON_BREAK) {

            // Aspetta la fine della giornata usando sigsuspend
            sigset_t wait_mask;
            sigfillset(&wait_mask);
            sigdelset(&wait_mask, SIGUSR2); // Permettiamo a SIGUSR2 (fine giornata) di interromperci
            sigdelset(&wait_mask, SIGTERM); // Permettiamo anche a SIGTERM di interromperci
            
            // Aspetta fino a quando non riceviamo un SIGUSR2 o SIGTERM
            sigsuspend(&wait_mask);
            continue; // Riparte il ciclo per il giorno successivo
        }

        // Ricerca di uno sportello disponibile
        int assigned_counter = -1;
        while (day_in_progress && running && assigned_counter < 0 && 
               shm_ptr->operators[operator_id].status != OPERATOR_ON_BREAK)
        {
            // Acquisisce il mutex per l'accesso agli sportelli
            struct sembuf sem_op;
            sem_op.sem_num = SEM_COUNTERS;
            sem_op.sem_op = -1;
            sem_op.sem_flg = 0;
            
            if (semop(semid, &sem_op, 1) < 0) {
                perror("Operator: Failed to acquire counter mutex");
                break;
            }
            
            // Ricerca di uno sportello libero
            for (int i = 0; i < NOF_WORKER_SEATS; i++) {
                if (shm_ptr->counters[i].active && 
                    shm_ptr->counters[i].current_service == random_service &&
                    shm_ptr->counters[i].operator_pid == 0) {
                    // Assegna questo operatore allo sportello
                    shm_ptr->counters[i].operator_pid = getpid();
                    shm_ptr->operators[operator_id].status = OPERATOR_WORKING;
                    assigned_counter = i;
                    // DEBUG: Stampa assegnazione
                    //printf("[OPERATORE %d] Assegnato allo sportello %d per il servizio %s\n", operator_id, i, SERVICE_NAMES[random_service]);
                    break;
                }
            }
            
            // Rilascia il mutex
            sem_op.sem_op = 1; // Unlock
            if (semop(semid, &sem_op, 1) < 0) {
                perror("Operator: Failed to release counter mutex");
            }
            
            // Se non trova sportello, entra in attesa
            if (assigned_counter < 0 && day_in_progress && running) {
                shm_ptr->operators[operator_id].status = OPERATOR_WAITING;
                
                // Aspetta un segnale usando sigsuspend invece dell'attesa attiva
                sigset_t wait_mask;
                sigfillset(&wait_mask);
                sigdelset(&wait_mask, SIGUSR1); // Permettiamo SIGUSR1 (riassegnazione sportello)
                sigdelset(&wait_mask, SIGUSR2); // Permettiamo SIGUSR2 (fine giornata)
                sigdelset(&wait_mask, SIGTERM); // Permettiamo SIGTERM (terminazione)
                
                // Aspetta fino a quando non riceviamo un segnale
                sigsuspend(&wait_mask);
            }
        }
        
        if (assigned_counter >= 0)
        {
            // Ciclo interno per la giornata lavorativa
            while (day_in_progress && running && 
                   shm_ptr->operators[operator_id].status == OPERATOR_WORKING)
            {
                // Serve un cliente
                int result = serve_customer(assigned_counter);
                if (result == -1)
                {
                    // Entrato in pausa
                    break;
                }
                
                // Attesa ticket in caso di nessun utente in coda
                if (result == 0 && day_in_progress && running && 
                    shm_ptr->operators[operator_id].status == OPERATOR_WORKING)
                {
                    // ATTESA ISTANTANEA: usa sigsuspend per attendere un segnale senza polling
                    sigset_t wait_mask;
                    sigfillset(&wait_mask);
                    sigdelset(&wait_mask, SIGUSR1); // Nuovo ticket o riassegnazione
                    sigdelset(&wait_mask, SIGUSR2); // Fine giornata
                    sigdelset(&wait_mask, SIGTERM); // Terminazione
                    
                    sigsuspend(&wait_mask);
                }
            }
        }
    }

    // Distacca dalla memoria condivisa prima di uscire
    if (shmdt(shm_ptr) == -1)
    {
        perror("Operator: shmdt failed");
        exit(EXIT_FAILURE);
    }

    return 0;
}