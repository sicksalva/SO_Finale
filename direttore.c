#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h> // Needed for signal handling
#include <time.h>
#include <sys/msg.h>
#include <sys/types.h>
#include "config.h"
#include <sys/shm.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>

// Variabili globali per la memoria condivisa e i semafori
int shmid = -1;
int semid = -1;
SharedMemory *shared_memory = NULL;
volatile sig_atomic_t alarm_triggered = 0; // Flag per l'alarm handler
volatile sig_atomic_t cleanup_in_progress = 0; // Flag to prevent re-entrancy

// Handler per SIGALRM
void alarm_handler(int signum __attribute__((unused))) {
    alarm_triggered = 1;
}

// Handler per la pulizia in caso di segnali di terminazione
void cleanup_handler(int signum __attribute__((unused))) {
    if (cleanup_in_progress) {
        return;
    }
    cleanup_in_progress = 1;

    printf("Pulizia iniziata...\n");

    // 1. Termina tutti i processi figli
    if (shared_memory != NULL && shared_memory != (void *)-1) {
        // Termina ticket process
        if (shared_memory->ticket_pid > 0) {
            kill(shared_memory->ticket_pid, SIGTERM);
        }
        
        // Termina tutti gli utenti
        for (int i = 0; i < NOF_USERS; i++) {
            if (shared_memory->user_pids[i] > 0) {
                kill(shared_memory->user_pids[i], SIGTERM);
            }
        }
        
        // Termina tutti gli operatori
        for (int i = 0; i < NOF_WORKERS; i++) {
            if (shared_memory->operator_pids[i] > 0) {
                kill(shared_memory->operator_pids[i], SIGTERM);
            }
        }

        // Attendi la terminazione di tutti i processi figli in modo robusto
        pid_t wpid;
        int status;
        while ((wpid = wait(&status)) > 0 || (wpid == -1 && errno == EINTR)) {
            // Il loop continua finché i figli terminano o wait è interrotto
        }
    }
    
    // 2. Pulisci la coda di messaggi
    printf("Pulendo le risorse IPC...\n");
    int msgid = msgget(MSG_QUEUE_KEY, 0666);
    if (msgid != -1) {
        if (msgctl(msgid, IPC_RMID, NULL) == -1) {
            perror("Failed to remove message queue");
        }
    }
    
    // 3. Pulisci la memoria condivisa
    if (shared_memory != NULL && shared_memory != (void *)-1) {
        shmdt(shared_memory);
        shared_memory = NULL;
    }
    
    if (shmid != -1) {
        shmctl(shmid, IPC_RMID, NULL);
        shmid = -1;
    }
    
    // 4. Pulisci i semafori
    if (semid != -1) {
        semctl(semid, 0, IPC_RMID);
        semid = -1;
    }
    
    printf("Pulizia completata.\n");
    // 5. Esci dal programma
    exit(EXIT_SUCCESS);
}

// Timeout handler
void handle_timeout(int signum __attribute__((unused))) {
    printf("Simulation timeout reached. Cleaning up...\n");
    
    // Notifica tutti i processi di terminare
    for (int i = 0; i < NOF_USERS; i++) {
        if (shared_memory->user_pids[i] > 0) {
            kill(shared_memory->user_pids[i], SIGTERM);
        }
    }
    
    cleanup_handler(0); // Passa 0 come dummy signum
}

void create_users(SharedMemory *shm_ptr)
{
    for (int i = 0; i < NOF_USERS; i++)
    {
        pid_t user_pid = fork();
        if (user_pid == 0)
        {
            // Processi figli
            char user_id[10];
            sprintf(user_id, "%d", i);
            execl("./utente", "./utente", user_id, NULL);
            perror("execl failed for utente");
            exit(EXIT_FAILURE);
        }
        else if (user_pid < 0)
        {
            perror("fork for utente failed");
            cleanup_handler(0); // Chiamata a cleanup in caso di errore di fork
            exit(EXIT_FAILURE);
        }
        else
        {
            // Parent process: Store the PID in shared memory
            shm_ptr->user_pids[i] = user_pid;
        }
    }
}

void create_ticket_process(SharedMemory *shm_ptr)
{
    pid_t ticket_pid = fork();
    if (ticket_pid == 0)
    {
        // Processi figli
        execl("./ticket", "./ticket", NULL);
        perror("execl failed for ticket");
        exit(EXIT_FAILURE);
    }
    else if (ticket_pid < 0)
    {
        perror("fork for ticket failed");
        exit(EXIT_FAILURE);
    }
    else
    {
        // Parent process: Store the PID in shared memory
        shm_ptr->ticket_pid = ticket_pid;
    }
}

void create_operators(SharedMemory *shm_ptr)
{
    printf("Creating %d operators...\n", NOF_WORKERS);

    // Inizializza gli operatori in memoria condivisa
    for (int i = 0; i < NOF_WORKERS; i++) {
        shm_ptr->operators[i].active = 1;
        shm_ptr->operators[i].total_served = 0;
        shm_ptr->operators[i].total_pauses = 0;
    }

    // Crea i processi operatore
    for (int i = 0; i < NOF_WORKERS; i++)
    {
        pid_t operator_pid = fork();
        if (operator_pid == 0)
        {
            // Processi figli
            char operator_id[10];
            sprintf(operator_id, "%d", i);
            execl("./operatore", "./operatore", operator_id, NULL);
            perror("execl failed for operatore");
            exit(EXIT_FAILURE);
        }
        else if (operator_pid < 0)
        {
            perror("fork for operatore failed");
            exit(EXIT_FAILURE);
        }
        else
        {
            // Parent process: Store the PID
            shm_ptr->operator_pids[i] = operator_pid;
            shm_ptr->operators[i].pid = operator_pid;
        }
    }
    printf("All operators created successfully.\n");
}

// Inizializza gli sportelli con servizi casuali all'inizio di ogni giornata
void initialize_counters_for_day(SharedMemory *shm_ptr)
{
    // Generatore di numeri casuali
    srand(time(NULL) ^ shm_ptr->simulation_day);
    
    for (int counter_idx = 0; counter_idx < NOF_WORKER_SEATS; counter_idx++) {
        // Genera un servizio casuale
        int random_service = rand() % SERVICE_COUNT;
        
        // Inizializza lo sportello
        shm_ptr->counters[counter_idx].active = 1;
        shm_ptr->counters[counter_idx].current_service = random_service;
        shm_ptr->counters[counter_idx].operator_pid = 0;
        shm_ptr->counters[counter_idx].total_served = 0;
        
        printf("Sportello %d: Servizio %s (%d)\n", 
               counter_idx, SERVICE_NAMES[random_service], random_service);
    }

    printf("All counters initialized for day %d.\n", shm_ptr->simulation_day);
}

// Funzione per gestire la condizione di "esplosione"
void handle_explode_condition(SharedMemory *shm) {
    int total_waiting_users = 0;
    for (int i = 0; i < SERVICE_COUNT; i++) {
        total_waiting_users += shm->service_tickets_waiting[i];
    }

    if (total_waiting_users > EXPLODE_THRESHOLD) {
        printf("\n\n[EXPLODE] Il numero totale di utenti in coda (%d) ha superato la soglia di %d.\nLa simulazione termina per congestione eccessiva.\n\n", total_waiting_users, EXPLODE_THRESHOLD);
        
        // Trigger cleanup e terminazione
        cleanup_handler(0);
    }
}

// Funzione per contare i ticket rimasti in coda alla fine della giornata
void count_remaining_tickets(SharedMemory *shm) {
    for (int i = 0; i < MAX_REQUESTS; i++) {
        TicketRequest *ticket = &shm->ticket_requests[i];
        
        // Ticket ricevuto ma non servito con successo
        if (ticket->status == REQUEST_COMPLETED && !ticket->served_successfully) {
            
            shm->daily_users_timeout[ticket->service_id]++;
            shm->total_users_timeout++;
            
            // DEBUG: stampa conteggio
            //printf("[CONTEGGIO] Ticket %s (utente #%d) non servito entro fine giornata - contato come interrotto\n", ticket->ticket_id, ticket->user_id);
        }
    }
}

// Funzione per svuotare tutte le code alla fine della giornata
void clear_all_queues_at_day_end(SharedMemory *shm) {
    printf("[RESET] Svuotamento di tutte le code alla fine della giornata %d\n", shm->simulation_day);
    
    // Svuota tutte le code dei servizi
    for (int service = 0; service < SERVICE_COUNT; service++) {
        if (shm->service_tickets_waiting[service] > 0) {
            // DEBUG: stampa quanti e quali ticket vengono scartati
            //printf("[RESET] Servizio %s: %d ticket non serviti scartati\n", SERVICE_NAMES[service], shm->service_tickets_waiting[service]);
        }
        
        // Reset delle code per questo servizio
        shm->service_tickets_waiting[service] = 0;
        shm->service_queue_head[service] = 0;
        shm->service_queue_tail[service] = 0;
        
        // Pulisce anche l'array della coda
        memset(shm->service_queues[service], 0, MAX_SERVICE_QUEUE * sizeof(int));
    }
    
}

// Funzione per stampare il riepilogo giornaliero
void print_daily_summary(SharedMemory *shm_ptr) {
    // Tabella 1: Statistiche sugli utenti
    printf("\n+----------------------+----------------------+----------------------+----------------------+----------------------+\n");
    printf("| STATISTICHE GIORNO %-3d                                                                          |\n", shm_ptr->simulation_day);
    printf("+----------------------+----------------------+----------------------+----------------------+----------------------+\n");
    printf("|      Servizio       |  Utenti Serviti      |  Tornati a Casa      | Ticket Non Ricevuti  |  Servizio Interrotto |\n");
    printf("+----------------------+----------------------+----------------------+----------------------+----------------------+\n");

    int total_timeout = 0;
    int total_no_ticket = 0;
    for (int i = 0; i < SERVICE_COUNT; i++) {
        printf("| %-20s | %-20d | %-20d | %-20d | %-20d |\n",
               SERVICE_NAMES[i],
               shm_ptr->daily_tickets_served[i],
               shm_ptr->daily_users_home[i],
               shm_ptr->daily_users_no_ticket[i],
               shm_ptr->daily_users_timeout[i]);
        total_timeout += shm_ptr->daily_users_timeout[i];
        total_no_ticket += shm_ptr->daily_users_no_ticket[i];
    }

    printf("+----------------------+----------------------+----------------------+----------------------+----------------------+\n");
    printf("| Totale               | %-20d | %-20d | %-20d | %-20d |\n",
           shm_ptr->total_tickets_served,
           shm_ptr->total_users_home,
           total_no_ticket,
           total_timeout);
    printf("+----------------------+----------------------+----------------------+----------------------+----------------------+\n");
}

// Funzione per notificare tutti i processi con un segnale specifico
void notify_all_processes(SharedMemory *shm, int signum) {
    const char *signal_name;
    if (signum == SIGUSR1) {
        signal_name = "SIGUSR1";
    } else if (signum == SIGUSR2) {
        signal_name = "SIGUSR2";
    } else {
        signal_name = "Unknown Signal";
    }

    // Notifica il processo ticket
    if (shm->ticket_pid > 0) {
        if (kill(shm->ticket_pid, signum) < 0) {
            char error_msg[100];
            sprintf(error_msg, "Failed to send %s to ticket process", signal_name);
            perror(error_msg);
        }
    }

    // Notifica tutti gli operatori
    for (int i = 0; i < NOF_WORKERS; i++) {
        if (shm->operator_pids[i] > 0) {
            if (kill(shm->operator_pids[i], signum) < 0) {
                char error_msg[100];
                sprintf(error_msg, "Failed to send %s to operator %d", signal_name, i);
                perror(error_msg);
            }
        }
    }

    // Notifica tutti gli utenti
    for (int i = 0; i < NOF_USERS; i++) {
        if (shm->user_pids[i] > 0) {
            if (kill(shm->user_pids[i], signum) < 0) {
                char error_msg[100];
                sprintf(error_msg, "Failed to send %s to user %d", signal_name, i);
                perror(error_msg);
            }
        }
    }
}

// Funzione per raccogliere le statistiche di fine giornata
void collect_daily_statistics(SharedMemory *shm, int day_index) {
    // Raccoglie le statistiche aggregate della giornata
    shm->users_served_per_day[day_index] = shm->total_tickets_served;
    shm->total_users_served_simulation += shm->total_tickets_served;
    
    // Calcola servizi non erogati totali per questa giornata
    int daily_services_not_provided = 0;
    int daily_total_wait_count = 0;
    long daily_total_wait_time = 0;
    int daily_total_service_count = 0;
    long daily_total_service_time = 0;
    
    for (int i = 0; i < SERVICE_COUNT; i++) {
        // Servizi non erogati = utenti tornati a casa + timeout + no ticket
        int service_not_provided = shm->daily_users_home[i] + shm->daily_users_timeout[i] + shm->daily_users_no_ticket[i];
        daily_services_not_provided += service_not_provided;
        
        // Raccoglie statistiche per servizio per questo giorno
        // usa una matrice 2D [servizio][giorno]
        shm->users_served_per_service_per_day[i][day_index] = shm->daily_tickets_served[i];
        shm->services_not_provided_per_service_per_day[i][day_index] = service_not_provided;
        shm->total_wait_time_per_service_per_day[i][day_index] = shm->total_wait_time[i];
        shm->wait_count_per_service_per_day[i][day_index] = shm->wait_count[i];
        shm->total_service_time_per_service_per_day[i][day_index] = shm->total_service_time[i];
        shm->service_count_per_service_per_day[i][day_index] = shm->service_count[i];
        
        // Accumula per le statistiche aggregate giornaliere
        daily_total_wait_count += shm->wait_count[i];
        daily_total_wait_time += shm->total_wait_time[i];
        daily_total_service_count += shm->service_count[i];
        daily_total_service_time += shm->total_service_time[i];
    }
    
    shm->services_not_provided_per_day[day_index] = daily_services_not_provided;
    shm->total_services_not_provided_simulation += daily_services_not_provided;
    
    shm->total_wait_time_per_day[day_index] = daily_total_wait_time;
    shm->wait_count_per_day[day_index] = daily_total_wait_count;
    shm->total_service_time_per_day[day_index] = daily_total_service_time;
    shm->service_count_per_day[day_index] = daily_total_service_count;
    
    // Conta operatori attivi e pause per questo giorno
    int daily_operators_active = 0;
    int daily_total_pauses = 0;
    
    for (int i = 0; i < NOF_WORKERS; i++) {
        // Conto le pause degli operatori attivi
        if (shm->operators[i].active && shm->operators[i].total_served > 0) {
            daily_operators_active++;
        }
        daily_total_pauses += shm->operators[i].total_pauses;
    }
    
    // Calcola le pause di un giorno (pause totale - pause fino a ieri)
    int pauses_for_this_day_only = daily_total_pauses;
    if (day_index > 0) {
        pauses_for_this_day_only = daily_total_pauses - shm->total_pauses_simulation;
    }
    
    shm->operators_active_per_day[day_index] = daily_operators_active;
    shm->pauses_per_day[day_index] = pauses_for_this_day_only; // Solo le pause di questo giorno
    shm->total_pauses_simulation += pauses_for_this_day_only;
    
    // Conta operatori attivi per servizio e aggiorna le somme totali
    for (int service = 0; service < SERVICE_COUNT; service++) {
        int active_operators_for_service = 0;
        
        // Conta gli operatori attivi per questo servizio specifico
        for (int i = 0; i < NOF_WORKERS; i++) {
            if (shm->operators[i].active && 
                (int)shm->operators[i].current_service == service && 
                shm->operators[i].total_served > 0) {
                active_operators_for_service++;
            }
        }
        
        // Somma al totale per questo servizio
        shm->operators_active_per_service_total[service] += active_operators_for_service;
    }
    
    // Calcola le medie cumulative progressive fino al giorno corrente
    shm->cumulative_avg_users_served[day_index] = (double)shm->total_users_served_simulation / (day_index + 1);
    shm->cumulative_avg_services_provided[day_index] = (double)shm->total_services_provided_simulation / (day_index + 1);
    shm->cumulative_avg_services_not_provided[day_index] = (double)shm->total_services_not_provided_simulation / (day_index + 1);
    
}

// Funzione per convertire nanosecondi in minuti simulati
double nanoseconds_to_simulated_minutes(long nanoseconds) {
    
    double real_seconds = nanoseconds / 1000000000.0;
    
    // Converte secondi reali in minuti simulati
    double simulated_minutes = (real_seconds / DAY_SIMULATION_TIME) * WORK_DAY_MINUTES;
    
    return simulated_minutes;
}

// Funzione per stampare la tabella separata dei tempi di servizio
void print_service_timing_statistics_table(SharedMemory *shm, int days_completed) {
    printf("\n+----------------------+----------+----------+----------+----------+----------+----------+----------+----------+\n");
    printf("| STATISTICHE TEMPI DI SERVIZIO                                                                  |\n");
    printf("+----------------------+----------+----------+----------+----------+----------+----------+----------+----------+\n");
    printf("|      Servizio        | Tempo    | Tempo    | Tempo    | Tempo    | Minimo   | Minimo   | Massimo  | Massimo  |\n");
    printf("|                      | Serv.    | Serv.    | Serv.    | Serv.    | Giorno   | Giorno   | Giorno   | Giorno   |\n");
    printf("|                      | Medio    | Medio    | Medio    | Medio    | (Sec)    | (Min)    | (Sec)    | (Min)    |\n");
    printf("|                      | Giorno   | Simul.   | Giorno   | Simul.   |          |          |          |          |\n");
    printf("+----------------------+----------+----------+----------+----------+----------+----------+----------+----------+\n");
    
    for (int i = 0; i < SERVICE_COUNT; i++) {
        int total_service_count_service = 0;
        long total_service_time_service = 0;
        
        // Calcola statistiche per tutta la simulazione
        for (int day = 0; day < SIM_DURATION; day++) {
            total_service_time_service += shm->total_service_time_per_service_per_day[i][day];
            total_service_count_service += shm->service_count_per_service_per_day[i][day];
        }
        
        // Calcola la media del tempo di servizio per l'ultimo giorno
        double avg_service_time_daily = 0;
        int last_day = days_completed - 1;
        if (last_day >= 0 && shm->service_count_per_service_per_day[i][last_day] > 0) {
            avg_service_time_daily = (double)shm->total_service_time_per_service_per_day[i][last_day] / 
                                   shm->service_count_per_service_per_day[i][last_day] / 1000000000.0;
        }
        
        // Calcola la media del tempo di servizio per tutta la simulazione
        double avg_service_time_simulation = 0;
        if (total_service_count_service > 0) {
            avg_service_time_simulation = (double)total_service_time_service / total_service_count_service / 1000000000.0; // Converti in secondi
        }

        double min_service_time_sec = shm->min_service_time[i] == LONG_MAX ? 0 : shm->min_service_time[i] / 1000000000.0;
        double max_service_time_sec = shm->max_service_time[i] / 1000000000.0;
        
        // Calcola i tempi in minuti simulati usando la funzione di conversione
        double avg_service_time_daily_min = 0;
        double avg_service_time_simulation_min = 0;
        
        if (last_day >= 0 && shm->service_count_per_service_per_day[i][last_day] > 0) {
            long avg_nano_daily = shm->total_service_time_per_service_per_day[i][last_day] / 
                                 shm->service_count_per_service_per_day[i][last_day];
            avg_service_time_daily_min = nanoseconds_to_simulated_minutes(avg_nano_daily);
        }
        
        if (total_service_count_service > 0) {
            long avg_nano_simulation = total_service_time_service / total_service_count_service;
            avg_service_time_simulation_min = nanoseconds_to_simulated_minutes(avg_nano_simulation);
        }

        double min_service_time_min = shm->min_service_time[i] == LONG_MAX ? 0 : nanoseconds_to_simulated_minutes(shm->min_service_time[i]);
        double max_service_time_min = nanoseconds_to_simulated_minutes(shm->max_service_time[i]);
        
        printf("| %-20s | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f | %8.3f |\n",
               SERVICE_NAMES[i], 
               avg_service_time_daily,           // Tempo medio giornaliero (secondi)
               avg_service_time_simulation,      // Tempo medio simulazione (secondi)
               avg_service_time_daily_min,       // Tempo medio giornaliero (minuti simulati)
               avg_service_time_simulation_min,  // Tempo medio simulazione (minuti simulati)
               min_service_time_sec,             // Tempo minimo (secondi)
               min_service_time_min,             // Tempo minimo (minuti simulati)
               max_service_time_sec,             // Tempo massimo (secondi)
               max_service_time_min);            // Tempo massimo (minuti simulati)
    }
    
    printf("+----------------------+----------+----------+----------+----------+----------+----------+----------+----------+\n");
}

// Funzione per stampare le statistiche complete finali
void print_comprehensive_statistics(SharedMemory *shm, int days_completed) {
    printf("\n");
    printf("================================================================================\n");
    printf("                        STATISTICHE COMPLETE DELLA SIMULAZIONE\n");
    printf("================================================================================\n");
    
    // Calcoli preliminari
    int total_operators_active = 0;
    long total_wait_time_simulation = 0;
    int total_wait_count_simulation = 0;
    long total_service_time_simulation = 0;
    int total_service_count_simulation = 0;
    
    for (int i = 0; i < SERVICE_COUNT; i++) {
        total_wait_time_simulation += shm->total_wait_time[i];
        total_wait_count_simulation += shm->wait_count[i];
        total_service_time_simulation += shm->total_service_time[i];
        total_service_count_simulation += shm->service_count[i];
    }
    
    for (int i = 0; i < NOF_WORKERS; i++) {
        if (shm->operators[i].total_served > 0) {
            total_operators_active++;
        }
    }
    
    double avg_operators_per_day = 0;
    for (int day = 0; day < days_completed; day++) {
        avg_operators_per_day += shm->operators_active_per_day[day];
    }
    if (days_completed > 0) {
        avg_operators_per_day /= days_completed;
    }
    
    // TABELLA 1: STATISTICHE UTENTI E SERVIZI (TOTALI E MEDIE)
    printf("\n+--------------------------------+--------------------+--------------------+\n");
    printf("| STATISTICHE UTENTI E SERVIZI                                             |\n");
    printf("+--------------------------------+--------------------+--------------------+\n");
    printf("| Descrizione                    | Totale Simulazione | Media Cumulativa   |\n");
    printf("+--------------------------------+--------------------+--------------------+\n");
    printf("| Utenti Serviti                 | %-18d | %-18.2f |\n", 
           shm->total_users_served_simulation, days_completed > 0 ? shm->cumulative_avg_users_served[days_completed - 1] : 0.0);
    printf("| Servizi Non Erogati            | %-18d | %-18.2f |\n", 
           shm->total_services_not_provided_simulation, days_completed > 0 ? shm->cumulative_avg_services_not_provided[days_completed - 1] : 0.0);
    printf("+--------------------------------+--------------------+--------------------+\n");
    
    // TABELLA 2: STATISTICHE PER TIPOLOGIA DI SERVIZIO
    printf("\n+----------------------+----------+----------+----------+----------+\n");
    printf("| STATISTICHE PER TIPOLOGIA DI SERVIZIO                       |\n");
    printf("+----------------------+----------+----------+----------+----------+\n");
    printf("|      Servizio        |  Utenti  |  Media   | Servizi  |  Media   |\n");
    printf("|                      |  Serviti |  Utenti  | Non Erog.|  Servizi |\n");
    printf("|                      |  Totale  |  Al      | Totale   |  Al      |\n");
    printf("|                      |          |  Giorno  |          |  Giorno  |\n");
    printf("+----------------------+----------+----------+----------+----------+\n");
    
    for (int i = 0; i < SERVICE_COUNT; i++) {
        int total_users_served_service = 0;
        int total_services_not_provided_service = 0;
        long total_service_time_service = 0;
        int total_service_count_service = 0;
        
        // Calcola statistiche per tutta la simulazione
        for (int day = 0; day < SIM_DURATION; day++) {
            total_users_served_service += shm->users_served_per_service_per_day[i][day];
            total_services_not_provided_service += shm->services_not_provided_per_service_per_day[i][day];
            total_service_time_service += shm->total_service_time_per_service_per_day[i][day];
            total_service_count_service += shm->service_count_per_service_per_day[i][day];
        }
        
        // Calcola la media giornaliera degli utenti serviti
        double avg_users_served_per_day = days_completed > 0 ? (double)total_users_served_service / days_completed : 0.0;
        
        // Calcola la media giornaliera dei servizi non erogati
        double avg_services_not_provided_per_day = days_completed > 0 ? (double)total_services_not_provided_service / days_completed : 0.0;
        
        printf("| %-20s | %8d | %8.2f | %8d | %8.2f |\n",
               SERVICE_NAMES[i], 
               total_users_served_service,
               avg_users_served_per_day,        // Media utenti serviti al giorno
               total_services_not_provided_service,
               avg_services_not_provided_per_day); // Media servizi non erogati al giorno
    }
    
    printf("+----------------------+----------+----------+----------+----------+\n");
    
    // TABELLA 3: RAPPORTO OPERATORI/SPORTELLI PER SERVIZIO (ultima giornata)
    printf("\n+----------------------+--------------------+--------------------+--------------------+--------------------+--------------------+\n");
    printf("| RAPPORTO OPERATORI/SPORTELLI PER SERVIZIO (ultima giornata)                                                               |\n");
    printf("+----------------------+--------------------+--------------------+--------------------+--------------------+--------------------+\n");
    printf("|      Servizio        |     Sportelli      |     Operatori      |      Rapporto      |   Op. Attivi       |   Op. Attivi       |\n");
    printf("|                      |                    |                    |                    |   Ultimo Giorno    |   Tutti i Giorni   |\n");
    printf("+----------------------+--------------------+--------------------+--------------------+--------------------+--------------------+\n");
    
    // Calcola statistiche generali degli operatori
    int total_operators_active_last_day = 0;
    int total_operators_active_simulation = 0;
    int total_pauses_last_day = 0;
    int total_pauses_simulation = 0;
    
    if (days_completed > 0) {
        total_operators_active_last_day = shm->operators_active_per_day[days_completed - 1];
        // Le pause dell'ultimo giorno sono già calcolate correttamente come differenziali
        total_pauses_last_day = shm->pauses_per_day[days_completed - 1];
    }
    
    // Calcola totale operatori attivi e pause in tutta la simulazione
    for (int day = 0; day < days_completed; day++) {
        total_operators_active_simulation += shm->operators_active_per_day[day];
        total_pauses_simulation += shm->pauses_per_day[day];
    }
    
    for (int service = 0; service < SERVICE_COUNT; service++) {
        // Array per memorizzare gli sportelli e operatori per questo servizio
        int counters_for_service[NOF_WORKER_SEATS];
        int operators_for_service[NOF_WORKERS];
        int counter_count = 0;
        int operator_count = 0;
        int active_operators_for_service_last_day = 0;
        int active_operators_for_service_all_days = 0;
        
        // Trova tutti gli sportelli assegnati a questo servizio per la giornata corrente
        for (int i = 0; i < NOF_WORKER_SEATS; i++) {
            if (shm->counters[i].active && (int)shm->counters[i].current_service == service) {
                counters_for_service[counter_count++] = i;
            }
        }
        
        // Trova tutti gli operatori che hanno questo servizio come servizio fisso
        for (int i = 0; i < NOF_WORKERS; i++) {
            if (shm->operators[i].active && (int)shm->operators[i].current_service == service) {
                operators_for_service[operator_count++] = i;
                // Conta quelli che hanno effettivamente servito utenti nell'ultimo giorno
                if (shm->operators[i].total_served > 0) {
                    active_operators_for_service_last_day++;
                }
            }
        }
        
        // Calcola operatori attivi per questo servizio da tutta la simulazione
        active_operators_for_service_all_days = shm->operators_active_per_service_total[service];
        
        // Crea stringhe per sportelli e operatori
        char counters_str[200] = "";
        char operators_str[200] = "";
        
        // Formatta la lista degli sportelli
        for (int i = 0; i < counter_count; i++) {
            char temp[10];
            sprintf(temp, "%d", counters_for_service[i]);
            strcat(counters_str, temp);
            if (i < counter_count - 1) strcat(counters_str, ",");
        }
        if (counter_count == 0) strcpy(counters_str, "Nessuno");
        
        // Formatta la lista degli operatori
        for (int i = 0; i < operator_count; i++) {
            char temp[10];
            sprintf(temp, "%d", operators_for_service[i]);
            strcat(operators_str, temp);
            if (i < operator_count - 1) strcat(operators_str, ",");
        }
        if (operator_count == 0) strcpy(operators_str, "Nessuno");
        
        // Calcola il rapporto
        double ratio = counter_count > 0 ? (double)operator_count / counter_count : 0.0;
        
        printf("| %-20s | %-18s | %-18s | %18.2f | %18d | %18d |\n",
               SERVICE_NAMES[service], 
               counters_str,
               operators_str,
               ratio,
               active_operators_for_service_last_day,
               active_operators_for_service_all_days);
    }
    printf("+----------------------+--------------------+--------------------+--------------------+--------------------+--------------------+\n");
    
    // Riga separata per le pause totali
    printf("| %-20s | %-18s | %-18s | %18s | %18d | %18d |\n",
           "PAUSE TOTALI", 
           "-",
           "-", 
           "-",
           total_pauses_last_day,
           total_pauses_simulation);
    printf("+----------------------+--------------------+--------------------+--------------------+--------------------+--------------------+\n");
    
    // TABELLA 4: STATISTICHE GENERALI OPERATORI E PAUSE
    printf("\n+--------------------------------+--------------------+--------------------+--------------------+\n");
    printf("| STATISTICHE OPERATORI E PAUSE                                                              |\n");
    printf("+--------------------------------+--------------------+--------------------+--------------------+\n");
    printf("| Descrizione                    | Ultimo Giorno      | Totale Simulazione | Media al Giorno    |\n");
    printf("+--------------------------------+--------------------+--------------------+--------------------+\n");
    printf("| Operatori Attivi               | %-18d | %-18d | %-18.2f |\n", 
           total_operators_active_last_day, total_operators_active_simulation,
           days_completed > 0 ? (double)total_operators_active_simulation / days_completed : 0.0);
    printf("| Pause Totali                   | %-18d | %-18d | %-18.2f |\n", 
           total_pauses_last_day, total_pauses_simulation,
           days_completed > 0 ? (double)total_pauses_simulation / days_completed : 0.0);
    printf("| Media Pause per Operatore      | %-18.2f | %-18.2f | %-18.2f |\n", 
           total_operators_active_last_day > 0 ? (double)total_pauses_last_day / total_operators_active_last_day : 0.0,
           total_operators_active_simulation > 0 ? (double)total_pauses_simulation / total_operators_active_simulation : 0.0,
           days_completed > 0 && total_operators_active_simulation > 0 ? (double)total_pauses_simulation / days_completed / (total_operators_active_simulation / days_completed) : 0.0);
    printf("+--------------------------------+--------------------+--------------------+--------------------+\n");
    
    // TABELLA 5: STATISTICHE TEMPI DI ATTESA
    printf("\n+----------------------+----------+----------+----------+----------+----------+----------+\n");
    printf("| STATISTICHE TEMPI DI ATTESA (TOTALI SIMULAZIONE)                                      |\n");
    printf("+----------------------+----------+----------+----------+----------+----------+----------+\n");
    printf("|      Servizio        | Tempo    | Tempo    | Tempo    | Tempo    | Tempo    | Tempo    |\n");
    printf("|                      | Min      | Min      | Max      | Max      | Medio    | Medio    |\n");
    printf("|                      | (ms)     | (minuti) | (ms)     | (minuti) | (ms)     | (minuti) |\n");
    printf("+----------------------+----------+----------+----------+----------+----------+----------+\n");
    
    long overall_min_wait = LONG_MAX;
    long overall_max_wait = 0;
    long total_wait_time_all = 0;
    int total_wait_count_all = 0;
    
    for (int i = 0; i < SERVICE_COUNT; i++) {
        if (shm->wait_count[i] > 0) {
            double min_ms = shm->min_wait_time[i] / 1000000.0;
            double max_ms = shm->max_wait_time[i] / 1000000.0;
            double avg_ms = (shm->total_wait_time[i] / 1000000.0) / shm->wait_count[i];
            
            // Converti in minuti simulati
            double min_min = nanoseconds_to_simulated_minutes(shm->min_wait_time[i]);
            double max_min = nanoseconds_to_simulated_minutes(shm->max_wait_time[i]);
            double avg_min = nanoseconds_to_simulated_minutes(shm->total_wait_time[i] / shm->wait_count[i]);
            
            printf("| %-20s | %8.1f | %8.3f | %8.1f | %8.3f | %8.1f | %8.3f |\n",
                   SERVICE_NAMES[i],
                   min_ms, min_min,
                   max_ms, max_min,
                   avg_ms, avg_min);
            
            total_wait_time_all += shm->total_wait_time[i];
            total_wait_count_all += shm->wait_count[i];
            
            if (shm->min_wait_time[i] < overall_min_wait) {
                overall_min_wait = shm->min_wait_time[i];
            }
            if (shm->max_wait_time[i] > overall_max_wait) {
                overall_max_wait = shm->max_wait_time[i];
            }
        } else {
            printf("| %-20s | %8s | %8s | %8s | %8s | %8s | %8s |\n",
                   SERVICE_NAMES[i], "N/A", "N/A", "N/A", "N/A", "N/A", "N/A");
        }
    }
    
    printf("+----------------------+----------+----------+----------+----------+----------+----------+\n");
    
    // Calculate simulation-wide average for the "Media" row
    long simulation_total_wait_time = 0;
    int simulation_total_wait_count = 0;
    
    for (int day = 0; day < shm->simulation_day; day++) {
        if (shm->wait_count_per_day[day] > 0) {
            simulation_total_wait_time += shm->total_wait_time_per_day[day];
            simulation_total_wait_count += shm->wait_count_per_day[day];
        }
    }
    
    if (simulation_total_wait_count > 0) {
        // Media calcolata su TUTTI gli utenti serviti di tutta la simulazione
        double simulation_avg_ms = (simulation_total_wait_time / 1000000.0) / simulation_total_wait_count;
        double simulation_avg_min = nanoseconds_to_simulated_minutes(simulation_total_wait_time / simulation_total_wait_count);
        
        printf("| Media                | %8s | %8s | %8s | %8s | %8.1f | %8.3f |\n",
               "-", "-", "-", "-", simulation_avg_ms, simulation_avg_min);
    } else {
        printf("| Media                | %8s | %8s | %8s | %8s | %8s | %8s |\n",
               "N/A", "N/A", "N/A", "N/A", "N/A", "N/A");
    }
    printf("+----------------------+----------+----------+----------+----------+----------+----------+\n");
    
    printf("\n");
    printf("================================================================================\n");
}

// Funzione per inizializzare le variabili statistiche
void initialize_statistics(SharedMemory *shm) {
    // Inizializza le variabili per la simulazione (attesa)
    for (int i = 0; i < SERVICE_COUNT; i++) {
        shm->min_wait_time[i] = LONG_MAX;
        shm->max_wait_time[i] = 0;
        shm->total_wait_time[i] = 0;
        shm->wait_count[i] = 0;
        
        // Inizializza anche le statistiche tempi (servizio)
        shm->min_service_time[i] = LONG_MAX;
        shm->max_service_time[i] = 0;
        shm->total_service_time[i] = 0;
        shm->service_count[i] = 0;
        
        // Inizializza gli array delle statistiche giornaliere
        for (int day = 0; day < SIM_DURATION; day++) {
            shm->users_served_per_service_per_day[i][day] = 0;
            shm->services_not_provided_per_service_per_day[i][day] = 0;
            shm->total_wait_time_per_service_per_day[i][day] = 0;
            shm->wait_count_per_service_per_day[i][day] = 0;
            shm->total_service_time_per_service_per_day[i][day] = 0;
            shm->service_count_per_service_per_day[i][day] = 0;
        }
    }
    
    // Inizializza le statistiche aggregate per la simulazione
    shm->total_users_served_simulation = 0;
    shm->total_services_provided_simulation = 0;
    shm->total_services_not_provided_simulation = 0;
    shm->total_pauses_simulation = 0;
    
    // Inizializza le somme totali degli operatori attivi per servizio
    for (int i = 0; i < SERVICE_COUNT; i++) {
        shm->operators_active_per_service_total[i] = 0;
    }
    
    // Inizializza gli array delle statistiche giornaliere aggregate
    for (int day = 0; day < SIM_DURATION; day++) {
        shm->users_served_per_day[day] = 0;
        shm->services_not_provided_per_day[day] = 0;
        shm->total_wait_time_per_day[day] = 0;
        shm->wait_count_per_day[day] = 0;
        shm->total_service_time_per_day[day] = 0;
        shm->service_count_per_day[day] = 0;
        shm->pauses_per_day[day] = 0;
        shm->operators_active_per_day[day] = 0;
        
        // Inizializza le medie cumulative
        shm->cumulative_avg_users_served[day] = 0.0;
        shm->cumulative_avg_services_provided[day] = 0.0;
        shm->cumulative_avg_services_not_provided[day] = 0.0;
    }
}

// Funzione per inizializzare i semafori
void initialize_semaphores(int semid, int shmid, SharedMemory *shm) {
    // Inizializza i valori iniziali dei semafori
    unsigned short init_values[NUM_SEMS];
    init_values[SEM_MUTEX] = 1;         // Mutex per accesso alla memoria condivisa
    init_values[SEM_QUEUE] = 1;         // Mutex per accesso alla coda
    init_values[SEM_TICKET_REQ] = 0;    // Imposto a 0, ticket richiesti
    init_values[SEM_TICKET_READY] = 0;  // Imposto a 0, ticket pronti
    init_values[SEM_COUNTERS] = 1;      // Mutex per accesso agli sportelli
    init_values[SEM_SYNC] = 0;          // Sincronizzazione inizio giornata
    init_values[SEM_DAY_START] = 0;     // Giorno inizio segnalazione
    init_values[SEM_TICKET_WAIT] = 0;   // Mutex per attesa ticket
    
    // Inizializza i semafori di lock per i servizi (tutti sbloccati all'inizio)
    init_values[SEM_SERVICE_LOCK_PACKAGES] = 1;   // Pacchi sbloccato
    init_values[SEM_SERVICE_LOCK_LETTERS] = 1;    // Lettere sbloccato
    init_values[SEM_SERVICE_LOCK_BANCOPOST] = 1;  // Bancoposta sbloccato
    init_values[SEM_SERVICE_LOCK_BILLS] = 1;      // Bollette sbloccato
    init_values[SEM_SERVICE_LOCK_FINANCIAL] = 1;  // Finanza sbloccato
    init_values[SEM_SERVICE_LOCK_WATCHES] = 1;    // Orologi sbloccato

    // Inizializza la struttura per semctl
    union semun
    {
        int val;
        struct semid_ds *buf;
        unsigned short *array;
        struct seminfo *__buf;
    } arg;
    arg.array = init_values;

    if (semctl(semid, 0, SETALL, arg) == -1)
    {
        perror("semctl SETALL failed");
        // Pulizia in caso di errore
        semctl(semid, 0, IPC_RMID);
        shmdt(shm);
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }
}

// Funzione per resettare lo stato giornaliero
void reset_daily_state(SharedMemory *shm, int semid) {
    // Resetta i contatori giornalieri
    memset(shm->daily_tickets_served, 0, sizeof(int) * SERVICE_COUNT);
    memset(shm->daily_users_home, 0, sizeof(int) * SERVICE_COUNT);
    memset(shm->daily_users_timeout, 0, sizeof(int) * SERVICE_COUNT);
    memset(shm->daily_users_no_ticket, 0, sizeof(int) * SERVICE_COUNT);
    shm->total_tickets_served = 0;
    shm->total_users_home = 0;
    shm->total_users_timeout = 0;
    shm->total_users_no_ticket = 0;
    
    // Resetta le statistiche sui tempi di attesa
    for (int i = 0; i < SERVICE_COUNT; i++) {
        shm->min_wait_time[i] = LONG_MAX; // Valore massimo possibile
        shm->max_wait_time[i] = 0;
        shm->total_wait_time[i] = 0;
        shm->wait_count[i] = 0;
        
        // Resetta anche le statistiche giornaliere sui tempi di attesa
        shm->daily_total_wait_time[i] = 0;
        shm->daily_wait_count[i] = 0;
        
        // Resetta anche le statistiche sui tempi di servizio
        shm->min_service_time[i] = LONG_MAX;
        shm->max_service_time[i] = 0;
        shm->total_service_time[i] = 0;
        shm->service_count[i] = 0;
        
        // Azzera anche i contatori giornalieri
        shm->daily_users_timeout[i] = 0;
    }
    shm->total_users_timeout = 0;
    
    // Resetta anche le statistiche aggregate sui tempi di attesa
    shm->daily_total_wait_time_all = 0;
    shm->daily_wait_count_all = 0;
    
    // Reset anche del next_request_index per il giorno successivo
    shm->next_request_index = 0;

    // Reset the day start semaphore for the next day
    struct sembuf reset_day_start;
    reset_day_start.sem_num = SEM_DAY_START;
    reset_day_start.sem_op = -semctl(semid, SEM_DAY_START, GETVAL);  // Reset to 0
    reset_day_start.sem_flg = 0;
    if (semop(semid, &reset_day_start, 1) < 0)
    {
        perror("Failed to reset day start semaphore");
    }
    
    // Reset the ticket ready semaphore for the next day
    struct sembuf reset_ticket_ready;
    reset_ticket_ready.sem_num = SEM_TICKET_READY;
    reset_ticket_ready.sem_op = -semctl(semid, SEM_TICKET_READY, GETVAL);  // Reset to 0
    reset_ticket_ready.sem_flg = 0;
    if (semop(semid, &reset_ticket_ready, 1) < 0)
    {
        perror("Failed to reset ticket ready semaphore");
    }
}

int main()
{
    // Imposta i gestori dei segnali
    signal(SIGINT, cleanup_handler);   // Ctrl+C
    signal(SIGTERM, cleanup_handler);  // Terminazione forzata
    signal(SIGALRM, alarm_handler);    // Allarme per fine giornata simulata

    // Inizializza la memoria condivisa con una chiave fissa
    shmid = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid < 0)
    {
        perror("shmget");
        exit(EXIT_FAILURE);
    }

    // Attacca la memoria condivisa
    shared_memory = (SharedMemory *)shmat(shmid, NULL, 0);
    if (shared_memory == (void *)-1)
    {
        perror("shmat");
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }
    
    // Inizializza la memoria condivisa
    memset(shared_memory, 0, sizeof(SharedMemory)); // Azzera tutta la memoria condivisa
    
    // Inizializza le variabili statistiche
    initialize_statistics(shared_memory);

    // Inizializza i semafori
    semid = semget(SEM_KEY, NUM_SEMS, IPC_CREAT | 0666);
    if (semid < 0)
    {
        perror("semget failed");
        shmdt(shared_memory);
        shmctl(shmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    // Inizializza i semafori
    initialize_semaphores(semid, shmid, shared_memory);

    // Crea i processi necessari
    create_ticket_process(shared_memory);
    sleep(1);
    create_operators(shared_memory);
    sleep(1);
    create_users(shared_memory);
    sleep(1);

    // -----------------------------------------------------------------------------------------------------------------------------
    // LOOP PRINCIPALE DEL DIRETTORE
    // -----------------------------------------------------------------------------------------------------------------------------
    printf("Director running... Press Ctrl+C to exit.\n");


    for (int day = 0; day < SIM_DURATION; day++)
    {
        // Imposta il giorno corrente nella memoria condivisa
        shared_memory->simulation_day = day + 1;

        printf("Day %d simulation started.\n", day + 1);

        // Seed random per la giornata
        srand(time(NULL) + day);

        initialize_counters_for_day(shared_memory);

        // Notifica a tutti i processi l'inizio della giornata
        notify_all_processes(shared_memory, SIGUSR1);

        // Imposta il flag day_in_progress 
        shared_memory->day_in_progress = 1;

        // Semaforo contatore per iniziare la giornata
        struct sembuf barrier_release;
        barrier_release.sem_num = SEM_DAY_START;  
        barrier_release.sem_op = NOF_USERS + NOF_WORKERS + 1;
        barrier_release.sem_flg = 0;
        
        if (semop(semid, &barrier_release, 1) < 0) {
            perror("Failed to release day start semaphore");
        }

        // -----------------------------------------------------------------
        // Inizia la simulazione della GIORNATA lavorativa
        // -----------------------------------------------------------------
        printf("Simulazione giornata lavorativa %d (durata: %d secondi)...\n", day + 1, DAY_SIMULATION_TIME);
        
        // Usiamo alarm per attendere esattamente DAY_SIMULATION_TIME
        int elapsed_seconds = 0;
        while (elapsed_seconds < DAY_SIMULATION_TIME) {

            alarm_triggered = 0;
            
            // Configura un timer di alarm per un secondo
            alarm(1);
            
            // Prepara un set di segnali per l'attesa
            sigset_t wait_mask;
            sigfillset(&wait_mask);
            sigdelset(&wait_mask, SIGALRM);
            sigdelset(&wait_mask, SIGINT);  // Permetti anche Ctrl+C
            sigdelset(&wait_mask, SIGTERM); // Permetti anche SIGTERM
            
            // Blocca tutti i segnali tranne quelli nel wait_mask
            sigset_t orig_mask;
            sigprocmask(SIG_SETMASK, &wait_mask, &orig_mask);
            
            while (!alarm_triggered) {
                // Esci dall'attesa se arriva un segnale specifico
                sigsuspend(&wait_mask);
            }
            sigprocmask(SIG_SETMASK, &orig_mask, NULL);
            
            // Incrementa il contatore e stampa lo stato
            elapsed_seconds++;
            if (elapsed_seconds % 1 == 0) {
                printf("Giorno %d: %d secondi passati\n", day + 1, elapsed_seconds);
            }

            // Guarda se c'è una condizione di esplosione
            handle_explode_condition(shared_memory);
        }
        
        printf("Giorno %d Completato dopo %d secondi.\n", day + 1, DAY_SIMULATION_TIME);

        // Notifica tutti i processi della fine della giornata
        printf("Notifying all users about day %d end...\n", day + 1);
        shared_memory->day_in_progress = 0;  

        // Breve attesa per stampare le statistiche
        sleep(2);

        // Conta i ticket rimasti in coda alla fine della giornata
        count_remaining_tickets(shared_memory);

        // Svuota tutte le code alla fine della giornata
        clear_all_queues_at_day_end(shared_memory);

        // Stampa il riepilogo giornaliero (sulla giornata, non sulla simulazione)
        print_daily_summary(shared_memory);

        // Raccogli le statistiche giornaliere
        collect_daily_statistics(shared_memory, day);
        
        // Stampa tutte le statistiche complete alla fine di ogni giorno
        print_comprehensive_statistics(shared_memory, day + 1);
        
        // Stampa la tabella separata dei tempi di servizio
        print_service_timing_statistics_table(shared_memory, day + 1);

        // Resetta lo stato per il giorno successivo
        reset_daily_state(shared_memory, semid);

        // Notifica a tutti i processi la fine della giornata
        notify_all_processes(shared_memory, SIGUSR2);
        
        printf("Giorno %d, simulazione finita.\n", day + 1);
        
        // Attesa di qualche secondo prima del giorno successivo
        sleep(3);
    }

    printf("Simulazione finita; pulizia...\n");
    
    // Pulizia finale
    cleanup_handler(0);
    
    printf("Simulazione finita in condizioni normali. (timeout)\n");
    return 0;
}
