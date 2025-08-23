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
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

SharedMemory *shm_ptr = NULL;
int operator_id;
int semid = -1;
int running = 1;
int day_in_progress = 0;
volatile sig_atomic_t alarm_triggered = 0; // Flag per il segnale di alarm

ServiceType random_service; // Variabile globale per il servizio assegnato

// Funzione helper per operazioni sui semafori con gestione degli interrupt
int safe_semop(int semid, struct sembuf *sops, size_t nsops) {
    int result;
    while ((result = semop(semid, sops, nsops)) < 0) {
        if (errno == EINTR) {
            // Operazione interrotta da segnale, riprova
            continue;
        } else {
            // Errore reale
            return -1;
        }
    }
    return result;
}

// Handler per il segnale di alarm (per evitare deadlock)
void alarm_handler(int signum __attribute__((unused))) {
    alarm_triggered = 1; // Segnala che il timer è scaduto
}

// Funzione per cercare operatori disponibili e assegnarli agli sportelli liberi
void try_assign_available_operators()
{
    // Acquisire il mutex per l'accesso agli sportelli e operatori
    struct sembuf sem_op;
    sem_op.sem_num = SEM_COUNTERS;
    sem_op.sem_op = -1; // Lock
    sem_op.sem_flg = 0;
    
    if (semop(semid, &sem_op, 1) < 0) {
        return; // Se non riusciamo ad acquisire il lock, evitiamo deadlock
    }
    
    // Cerca sportelli liberi
    for (int counter_id = 0; counter_id < NOF_WORKER_SEATS; counter_id++) {
        if (shm_ptr->counters[counter_id].active && 
            shm_ptr->counters[counter_id].operator_pid == 0) {
            
            ServiceType counter_service = shm_ptr->counters[counter_id].current_service;
            
            // Cerca un operatore disponibile per questo servizio
            for (int op_id = 0; op_id < NOF_WORKERS; op_id++) {
                if (shm_ptr->operators[op_id].active &&
                    shm_ptr->operators[op_id].current_service == counter_service &&
                    shm_ptr->operators[op_id].status == OPERATOR_WAITING) {
                    
                    // Assegna l'operatore allo sportello
                    shm_ptr->counters[counter_id].operator_pid = shm_ptr->operators[op_id].pid;
                    shm_ptr->operators[op_id].status = OPERATOR_WORKING;
                    
                    printf("[RIASSEGNAZIONE] Operatore %d (PID %d) assegnato allo sportello %d per il servizio %s\n",
                           op_id, shm_ptr->operators[op_id].pid, counter_id, SERVICE_NAMES[counter_service]);
                    
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

// Funzione per visualizzare le code degli utenti alla fine della 
// Funzione per calcolare un tempo di servizio casuale nell'intorno di ±50% del valore standard
long calculate_random_service_time(ServiceType service)
{
    // Migliora il seeding per evitare valori ripetuti
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    
    // Combina più fonti di entropia per un seed più robusto
    static int call_count = 0;
    call_count++;
    unsigned int seed = ts.tv_nsec ^ getpid() ^ (operator_id << 16) ^ (call_count << 8) ^ time(NULL);
    srand(seed);
    
    // Chiama rand() più volte per "mescolare" il generatore
    for (int i = 0; i < 3; i++) {
        rand();
    }
    
    long base_time = SERVICE_TIMES[service];

    // Calcola una variazione casuale tra -50% e +50%
    int rand_val = rand() % 101;
    double variation = (rand_val - 50) / 100.0; // Da -0.5 a +0.5

    // Applica la variazione al tempo base
    long adjusted_time = base_time * (1.0 + variation);

    return adjusted_time;
}

// Funzione per servire un utente e gestire la pausa
int serve_customer(int assigned_counter)
{
    // Verifica se ci sono utenti in coda per il servizio
    if (shm_ptr->service_tickets_waiting[random_service] <= 0)
    {
        return 0; // Nessun utente da servire
    }

    // Lock per evitare che più operatori prendano lo stesso ticket
    // USA IL SEMAFORO SPECIFICO PER QUESTO SERVIZIO invece del semaforo globale
    struct sembuf sem_lock;
    sem_lock.sem_num = SEM_SERVICE_LOCK(random_service); // Semaforo specifico per il servizio
    sem_lock.sem_op = -1; // Lock (P operation)
    sem_lock.sem_flg = SEM_UNDO; // Important: auto-release if process terminates
    
    // Acquisiamo il lock con retry in caso di interruzione da segnale
    if (safe_semop(semid, &sem_lock, 1) < 0) {
        perror("[OPERATORE] Errore nell'acquisizione del lock per il ticket");
        return 0;
    }

    // Ottieni il primo ticket nella coda per questo servizio
    int ticket_idx = -1;
    if (shm_ptr->service_tickets_waiting[random_service] > 0)
    {
        int head = shm_ptr->service_queue_head[random_service];
        ticket_idx = shm_ptr->service_queues[random_service][head];
        
        // IMPORTANTE: Rimuovi immediatamente il ticket dalla coda per evitare duplicati
        shm_ptr->service_queue_head[random_service] = (head + 1) % MAX_SERVICE_QUEUE;
        shm_ptr->service_tickets_waiting[random_service]--;
        
        // *** OTTIMIZZAZIONE CRITICA *** 
        // RILASCIA IMMEDIATAMENTE IL SEMAFORO dopo aver preso il ticket!
        // Questo permette ad altri operatori dello stesso servizio di prendere ticket in parallelo
        // mentre questo operatore esegue il servizio (che dura ~100ms)
        struct sembuf sem_unlock_immediate;
        sem_unlock_immediate.sem_num = SEM_SERVICE_LOCK(random_service);
        sem_unlock_immediate.sem_op = 1; // Unlock (V operation)
        sem_unlock_immediate.sem_flg = 0;
        if (safe_semop(semid, &sem_unlock_immediate, 1) < 0) {
            perror("[OPERATORE] Errore nel rilascio immediato del lock per il ticket");
        }
    }
    else
    {
        // Rilascia il lock se non ci sono ticket disponibili dopo il check
        struct sembuf sem_unlock;
        sem_unlock.sem_num = SEM_SERVICE_LOCK(random_service);
        sem_unlock.sem_op = 1; // Unlock (V operation)
        sem_unlock.sem_flg = 0;
        if (safe_semop(semid, &sem_unlock, 1) < 0) {
            perror("[OPERATORE] Errore nel rilascio del lock per il ticket");
        }
        
        return 0; // Nessun utente da servire
    }

    // Se abbiamo un ticket valido, servi l'utente
    if (ticket_idx >= 0 && ticket_idx < MAX_REQUESTS)
    {
        // Ottieni informazioni sul ticket e sull'utente
        TicketRequest *ticket = &shm_ptr->ticket_requests[ticket_idx];

        //TODO: CONTROLLARE QUI
        // Controlla se l'utente è già in servizio da un altro operatore
        if (ticket->being_served && ticket->serving_operator_pid != 0) {
            printf("[OPERATORE %d] L'utente #%d (Ticket: %s) è già in servizio dall'operatore PID %d\n",
                   operator_id, ticket->user_id, ticket->ticket_id, ticket->serving_operator_pid);
            
            // DEVO RE-ACQUISIRE il semaforo per rimettere il ticket in coda
            struct sembuf sem_relock;
            sem_relock.sem_num = SEM_SERVICE_LOCK(random_service);
            sem_relock.sem_op = -1; // Lock (P operation)
            sem_relock.sem_flg = 0;
            if (safe_semop(semid, &sem_relock, 1) < 0) {
                perror("[OPERATORE] Errore nel re-acquisire il lock per rimettere il ticket");
                return 0;
            }
            
            // IMPORTANTE: Rimetti il ticket in coda dato che era già preso da un altro operatore
            int tail = shm_ptr->service_queue_tail[random_service];
            shm_ptr->service_queues[random_service][tail] = ticket_idx;
            shm_ptr->service_queue_tail[random_service] = (tail + 1) % MAX_SERVICE_QUEUE;
            shm_ptr->service_tickets_waiting[random_service]++;
            
            // Rilascia nuovamente il lock
            struct sembuf sem_unlock;
            sem_unlock.sem_num = SEM_SERVICE_LOCK(random_service);
            sem_unlock.sem_op = 1; // Unlock (V operation)
            sem_unlock.sem_flg = 0;
            if (safe_semop(semid, &sem_unlock, 1) < 0) {
                perror("[OPERATORE] Errore nel rilascio del lock dopo aver rimesso il ticket");
            }
            
            return 0; // Non serviamo l'utente
        }

        // Probabilità casuale di prendere una pausa prima di servire il cliente
        // Questo si attiva solo se c'è effettivamente un utente valido da servire
        if ((rand() % 100) < BREAK_PROBABILITY)
        {
            printf("[OPERATORE %d] Vado in pausa e torno a casa per oggi. Lascio libero lo sportello %d.\n", operator_id, assigned_counter);
            
            // Aggiorna le statistiche delle pause con mutex per thread safety
            struct sembuf sem_pause_stats;
            sem_pause_stats.sem_num = SEM_MUTEX;
            sem_pause_stats.sem_op = -1; // Lock
            sem_pause_stats.sem_flg = 0;
            
            if (semop(semid, &sem_pause_stats, 1) == 0) {
                shm_ptr->operators[operator_id].total_pauses++;
                // Rilascia il mutex
                sem_pause_stats.sem_op = 1; // Unlock
                semop(semid, &sem_pause_stats, 1);
            }
            
            // Imposta lo stato dell'operatore come in pausa
            shm_ptr->operators[operator_id].status = OPERATOR_ON_BREAK;

            // Libera lo sportello (l'utente rimarrà in coda)
            shm_ptr->counters[assigned_counter].operator_pid = 0;
            
            // Il semaforo è già stato rilasciato immediatamente dopo aver preso il ticket
            // Non serve rilasciarlo di nuovo qui
            
            // Cerca operatori disponibili per riassegnare sportelli liberi
            try_assign_available_operators();

            return -1; // Indica che l'operatore va in pausa
        }

        // Lock dell'utente - imposta che questo operatore sta servendo questo utente
        ticket->being_served = 1;
        ticket->serving_operator_pid = getpid();

        // Registra il tempo di inizio servizio per calcolare il tempo di attesa
        clock_gettime(CLOCK_MONOTONIC, &ticket->service_start_time);
        
        // Calcola il tempo di attesa in nanosecondi
        long wait_time_ns = (ticket->service_start_time.tv_sec - ticket->request_time.tv_sec) * 1000000000L +
                           (ticket->service_start_time.tv_nsec - ticket->request_time.tv_nsec);
        ticket->wait_time_ns = wait_time_ns;

        // Stampa il messaggio di inizio servizio con il tempo di attesa
        //printf("🕐 [OPERATORE %d] Inizio servizio per utente #%d (Ticket: %s) - Attesa: %.1fms\n",
        //       operator_id, ticket->user_id, ticket->ticket_id, wait_time_ns / 1000000.0);

        // Calcola un tempo di servizio casuale
        long service_time = calculate_random_service_time(random_service); 
        
        // Registra il tempo di inizio servizio per le statistiche
        struct timespec start_service_time;
        clock_gettime(CLOCK_MONOTONIC, &start_service_time);
        
        struct timeval start_time, end_time;
        gettimeofday(&start_time, NULL);

        // Prima di simulare il servizio, verifica se siamo ancora in una giornata lavorativa attiva
        if (!day_in_progress || !shm_ptr->day_in_progress)
        {
            printf("\t\t\t[OPERATORE %d] Giornata terminata mentre mi preparavo a servire l'utente #%d (Ticket: %s). L'utente dovrà attendere.\n",
                   operator_id, ticket->user_id, ticket->ticket_id);
                   
            // NON incrementiamo i contatori qui - sarà gestito dalla funzione di conteggio alla fine del giorno
            
            // MANTENIAMO i valori being_served e serving_operator_pid per indicare che questo ticket
            // è stato preso in carico ma non completato - la funzione di conteggio lo vedrà
            // ticket->being_served = 1;  // Resta 1
            // ticket->serving_operator_pid = getpid();  // Resta impostato
                   
            // Il semaforo è già stato rilasciato immediatamente dopo aver preso il ticket
            // Non serve rilasciarlo di nuovo qui
            
            return 0; // Non serviamo l'utente
        }
        
        // Simuliamo il servizio in modo preciso ma interrompibile solo da SIGUSR2
        
        // SOLUZIONE DEFINITIVA: Timing preciso con nanosleep resistente agli interrupt
        long remaining_time_ns = service_time; // in nanosecondi
        const long check_interval_ns = 50000000L; // 50ms in nanosecondi
        
        volatile sig_atomic_t service_interrupted = 0;
        
        while (remaining_time_ns > 0 && !service_interrupted) {
            long sleep_time_ns = (remaining_time_ns > check_interval_ns) ? check_interval_ns : remaining_time_ns;
            
            struct timespec sleep_spec;
            sleep_spec.tv_sec = sleep_time_ns / 1000000000L;
            sleep_spec.tv_nsec = sleep_time_ns % 1000000000L;
            
            struct timespec remaining_spec;
            
            // CRITICO: nanosleep può essere interrotto dai segnali, quindi dobbiamo 
            // verificare che il tempo completo sia trascorso
            int result = nanosleep(&sleep_spec, &remaining_spec);
            
            if (result == 0) {
                // Sleep completato normalmente
                remaining_time_ns -= sleep_time_ns;
            } else {
                // Sleep interrotto - calcola quanto tempo è effettivamente trascorso
                long actual_sleep_ns = sleep_time_ns - (remaining_spec.tv_sec * 1000000000L + remaining_spec.tv_nsec);
                remaining_time_ns -= actual_sleep_ns;
                
                // Se c'è ancora tempo rimanente, continua il loop
                if (remaining_spec.tv_sec > 0 || remaining_spec.tv_nsec > 0) {
                    // Riprendi il sleep per il tempo rimanente se la giornata è ancora attiva
                    if (day_in_progress && shm_ptr->day_in_progress) {
                        continue;
                    }
                }
            }
            
            // Controlliamo se la giornata è terminata tramite il flag day_in_progress
            // che viene impostato solo dal signal handler di SIGUSR2
            // INOLTRE controlliamo anche shm_ptr->day_in_progress per sincronizzazione immediata
            if (!day_in_progress || !shm_ptr->day_in_progress) {
                service_interrupted = 1;
                break;
            }
        }
        
        // Se la giornata è terminata prima che il servizio fosse completato
        if (!day_in_progress || !shm_ptr->day_in_progress)
        {
            printf("[OPERATORE %d] Giornata terminata mentre stavo servendo l'utente #%d (Ticket: %s). Servizio interrotto.\n",
                   operator_id, ticket->user_id, ticket->ticket_id);
                   
            // NON incrementiamo i contatori qui - sarà gestito dalla funzione di conteggio alla fine del giorno
            // shm_ptr->daily_users_timeout[random_service]++;
            // shm_ptr->total_users_timeout++;
            
            // Rilascia il lock dell'utente
            ticket->being_served = 0;
            ticket->serving_operator_pid = 0;
                   
            // Il semaforo è già stato rilasciato immediatamente dopo aver preso il ticket
            // Non serve rilasciarlo di nuovo qui
            
            return 0; // Servizio interrotto
        }

        // Registra il tempo di fine servizio e calcola la durata reale in secondi
        gettimeofday(&end_time, NULL);
        //double service_duration_sec = (end_time.tv_sec - start_time.tv_sec) +
        //                              (end_time.tv_usec - start_time.tv_usec) / 1000000.0;

        // Registra anche il tempo di fine per le statistiche sui tempi di servizio
        struct timespec end_service_time;
        clock_gettime(CLOCK_MONOTONIC, &end_service_time);
        long actual_service_time_ns = (end_service_time.tv_sec - start_service_time.tv_sec) * 1000000000L + 
                                     (end_service_time.tv_nsec - start_service_time.tv_nsec);

        // Converti la durata da secondi reali a minuti simulati
        // WORK_DAY_MINUTES minuti simulati corrispondono a DAY_SIMULATION_TIME secondi reali
        //double service_duration_min = (service_duration_sec / DAY_SIMULATION_TIME) * WORK_DAY_MINUTES;

        // Aggiorna le statistiche sui tempi di servizio con mutex per thread safety
        struct sembuf sem_service_stats;
        sem_service_stats.sem_num = SEM_MUTEX;
        sem_service_stats.sem_op = -1; // Lock
        sem_service_stats.sem_flg = 0;
        
        if (semop(semid, &sem_service_stats, 1) == 0) {
            // Aggiorna il tempo minimo di servizio
            if (actual_service_time_ns < shm_ptr->min_service_time[random_service]) {
                shm_ptr->min_service_time[random_service] = actual_service_time_ns;
            }
            
            // Aggiorna il tempo massimo di servizio
            if (actual_service_time_ns > shm_ptr->max_service_time[random_service]) {
                shm_ptr->max_service_time[random_service] = actual_service_time_ns;
            }
            
            // Aggiorna il tempo totale e il conteggio dei servizi
            shm_ptr->total_service_time[random_service] += actual_service_time_ns;
            shm_ptr->service_count[random_service]++;
            
            // Rilascia il mutex
            sem_service_stats.sem_op = 1; // Unlock
            semop(semid, &sem_service_stats, 1);
        }

        // Incrementa il contatore degli utenti serviti
        shm_ptr->operators[operator_id].total_served++;
        
        // Incrementa il contatore dei ticket serviti giornalieri
        shm_ptr->daily_tickets_served[random_service]++;
        shm_ptr->total_tickets_served++;
        
        // Incrementa il contatore totale dei servizi erogati per l'intera simulazione
        shm_ptr->total_services_provided_simulation++;

        // Registra il tempo di fine servizio
        clock_gettime(CLOCK_MONOTONIC, &ticket->service_end_time);
        
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
            sem_wait_stats.sem_op = 1; // Unlock
            semop(semid, &sem_wait_stats, 1);
        }

        // Il ticket è già stato rimosso dalla coda all'inizio della funzione
        // Aggiorna solo lo stato del ticket nella memoria condivisa
        ticket->status = REQUEST_COMPLETED;
        ticket->counter_id = assigned_counter;
        ticket->served_successfully = 1;  // Indica che il servizio è stato completato con successo
        
        // Rilascia il lock dell'utente
        ticket->being_served = 0;
        ticket->serving_operator_pid = 0;

        //printf("[OPERATORE %d] Servito l'utente %d (Ticket: %s) per il servizio %s in %.3f secondi (%.1f minuti simulati) allo sportello %d\n",
        //       operator_id, ticket->user_id, ticket->ticket_id, SERVICE_NAMES[random_service], service_duration_sec, service_duration_min, assigned_counter);

        // Il semaforo è già stato rilasciato immediatamente dopo aver preso il ticket
        // Non serve rilasciarlo di nuovo qui - questo permette il parallelismo!

        return 1; // Utente servito con successo
    }

    // Il semaforo è già stato rilasciato immediatamente dopo aver preso il ticket
    // oppure non è mai stato acquisito (nessun ticket disponibile)
    // Non serve rilasciarlo di nuovo qui

    return 0; // Nessun utente valido da servire
}

// Handler per la fine della giornata
void day_end_handler(int signum __attribute__((unused)))
{
    if (signum == SIGUSR2)
    {
        //printf("[OPERATORE %d] Giornata lavorativa terminata (segnale dal direttore).\n", operator_id);

        // Imposta la flag di giorno non più in corso
        day_in_progress = 0;
        
        // Imposta lo stato dell'operatore come finito per il giorno
        shm_ptr->operators[operator_id].status = OPERATOR_FINISHED;
        
        // Alla fine della giornata, rilascia lo sportello che era stato assegnato
        struct sembuf sem_op;
        sem_op.sem_num = SEM_COUNTERS;
        sem_op.sem_op = -1; // Lock
        sem_op.sem_flg = 0;
        
        if (semop(semid, &sem_op, 1) == 0) {
            // Cerca lo sportello assegnato a questo operatore
            for (int i = 0; i < NOF_WORKER_SEATS; i++) {
                if (shm_ptr->counters[i].operator_pid == getpid()) {
                    // Rilascia lo sportello
                    shm_ptr->counters[i].operator_pid = 0;
                    break;
                }
            }
            
            // Rilascia il mutex
            sem_op.sem_op = 1; // Unlock
            semop(semid, &sem_op, 1);
        }
    }
}

// Handler per l'inizio della giornata
void day_start_handler(int signum __attribute__((unused)))
{
    if (signum == SIGUSR1)
    {
        //printf("[OPERATORE %d] Inizio della giornata lavorativa.\n", operator_id);
        day_in_progress = 1;
        
        // Se l'operatore era in pausa, ora può cercare di lavorare di nuovo
        if (shm_ptr->operators[operator_id].status == OPERATOR_ON_BREAK) {
            shm_ptr->operators[operator_id].status = OPERATOR_WAITING;
        }
    }
}

// Handler per la terminazione
void termination_handler(int signum __attribute__((unused)))
{
    //printf("[OPERATORE %d] Ricevuto segnale di terminazione.\n", operator_id);
    running = 0;
}

// Funzione per assegnare casualmente un servizio all'operatore
ServiceType assign_random_service()
{
    // Inizializza il generatore di numeri casuali
    srand(time(NULL) ^ getpid());

    // Genera e restituisce un servizio casuale
    return rand() % SERVICE_COUNT;
}

// Funzione per inizializzare l'operatore nella memoria condivisa
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

    printf("[OPERATORE %d] PID: %d, Servizio assegnato: %s (ID: %d)\n",
           op_id, getpid(), SERVICE_NAMES[random_service], random_service);
}

int main(int argc, char *argv[])
{
    // Verifica se è stato passato l'ID dell'operatore
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <operator_id>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Ottieni l'ID dell'operatore dagli argomenti
    operator_id = atoi(argv[1]);

    // Collega alla memoria condivisa
    int shmid = shmget(SHM_KEY, sizeof(SharedMemory), 0666);
    if (shmid == -1)
    {
        perror("Operator: shmget failed");
        exit(EXIT_FAILURE);
    }

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
    
    // Imposta handler per SIGALRM
    sa.sa_handler = alarm_handler;
    if (sigaction(SIGALRM, &sa, NULL) == -1)
    {
        perror("Operator: sigaction SIGALRM failed");
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

        // Attendi l'inizio della giornata tramite segnale SIGUSR1 o semaforo
        if (!day_in_progress && running)
        {
            // Usa un semaforo per bloccare l'esecuzione finché non inizia la giornata
            struct sembuf sem_wait;
            sem_wait.sem_num = SEM_DAY_START;
            sem_wait.sem_op = -1; // Operazione P (decremento)
            sem_wait.sem_flg = 0;
            
            // Il semaforo ci bloccherà fino a quando il direttore non lo incrementa
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
            break; // Se è stato richiesto di terminare durante l'attesa, esci

        // Salta se l'operatore è in pausa per tutta la giornata
        if (shm_ptr->operators[operator_id].status == OPERATOR_ON_BREAK) {
            // L'operatore è in pausa per tutta la giornata, attende solo la fine
            //printf("[OPERATORE %d] Sono in pausa per tutta la giornata.\n", operator_id);
            
            // Aspetta la fine della giornata usando sigsuspend
            sigset_t wait_mask;
            sigfillset(&wait_mask);
            sigdelset(&wait_mask, SIGUSR2); // Permettiamo a SIGUSR2 (fine giornata) di interromperci
            sigdelset(&wait_mask, SIGTERM); // Permettiamo anche a SIGTERM di interromperci
            
            // Aspetta fino a quando non riceviamo un SIGUSR2 o SIGTERM
            sigsuspend(&wait_mask);
            continue; // Riparte il ciclo per il giorno successivo
        }

        // Loop per cercare uno sportello finché la giornata è in corso
        int assigned_counter = -1;
        while (day_in_progress && running && assigned_counter < 0 && 
               shm_ptr->operators[operator_id].status != OPERATOR_ON_BREAK)
        {
            // Acquisire il mutex per l'accesso agli sportelli
            struct sembuf sem_op;
            sem_op.sem_num = SEM_COUNTERS;
            sem_op.sem_op = -1; // Lock
            sem_op.sem_flg = 0;
            
            if (semop(semid, &sem_op, 1) < 0) {
                perror("Operator: Failed to acquire counter mutex");
                break;
            }
            
            // Cerca uno sportello libero per il nostro servizio
            for (int i = 0; i < NOF_WORKER_SEATS; i++) {
                if (shm_ptr->counters[i].active && 
                    shm_ptr->counters[i].current_service == random_service &&
                    shm_ptr->counters[i].operator_pid == 0) {
                    // Assegna questo operatore allo sportello
                    shm_ptr->counters[i].operator_pid = getpid();
                    shm_ptr->operators[operator_id].status = OPERATOR_WORKING;
                    assigned_counter = i;
                    printf("[OPERATORE %d] Assegnato allo sportello %d per il servizio %s\n", 
                           operator_id, i, SERVICE_NAMES[random_service]);
                    break;
                }
            }
            
            // Rilascia il mutex
            sem_op.sem_op = 1; // Unlock
            if (semop(semid, &sem_op, 1) < 0) {
                perror("Operator: Failed to release counter mutex");
            }
            
            // Se non ha trovato uno sportello, aspetta usando sigsuspend
            if (assigned_counter < 0 && day_in_progress && running) {
                // Imposta lo stato come in attesa
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
                // Servi un cliente
                int result = serve_customer(assigned_counter);
                if (result == -1)
                {
                    // L'operatore è andato in pausa
                    break;
                }
                
                // Se non ci sono clienti da servire, attendi il segnale di nuovo ticket
                if (result == 0 && day_in_progress && running && 
                    shm_ptr->operators[operator_id].status == OPERATOR_WORKING)
                {
                    // ATTESA ISTANTANEA: usa sigsuspend per attendere un segnale senza polling
                    sigset_t wait_mask;
                    sigfillset(&wait_mask);
                    sigdelset(&wait_mask, SIGUSR1); // Nuovo ticket o riassegnazione
                    sigdelset(&wait_mask, SIGUSR2); // Fine giornata
                    sigdelset(&wait_mask, SIGTERM); // Terminazione
                    
                    // Aspetta istantaneamente fino al prossimo segnale - ZERO ATTESA ATTIVA
                    sigsuspend(&wait_mask);
                }
                else if (result == 1 && day_in_progress && running && 
                         shm_ptr->operators[operator_id].status == OPERATOR_WORKING)
                {
                    // Cliente servito con successo - continua immediatamente col prossimo
                    // ZERO DELAY - controllo immediato per il prossimo cliente
                }
            }
        }
        else
        {
            // Non ha trovato uno sportello e la giornata è finita o l'operatore è in pausa
            // Non fa nulla, il loop esterno ripartirà per il giorno successivo
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