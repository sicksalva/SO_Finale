#ifndef CONFIG_READER_H
#define CONFIG_READER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Costanti massime per le strutture dati
#define MAX_WORKERS 200
#define MAX_USERS 2000  
#define MAX_WORKER_SEATS 200
#define MAX_SIM_DURATION 10

// Struttura per contenere i parametri di configurazione
typedef struct {
    int WORK_DAY_HOURS;
    int DAY_SIMULATION_TIME;
    int SIM_DURATION;
    int BREAK_PROBABILITY;
    int NOF_WORKERS;
    int NOF_USERS;
    int NOF_WORKER_SEATS;
    int NOF_PAUSE;
    int P_SERV_MIN;
    int P_SERV_MAX;
    int EXPLODE_THRESHOLD;
    int OFFICE_OPEN_TIME;
    int OFFICE_CLOSE_TIME;
    
    // Parametri calcolati
    int WORK_DAY_MINUTES;
    int TOTAL_SIMULATION_TIME;
    long N_NANO_SECS;
} Config;

// Variabile globale per la configurazione
extern Config config;

// Funzioni per leggere la configurazione
int read_config(const char* config_file);
void set_default_config();
void calculate_derived_values();

// Implementazione delle funzioni

Config config = {0}; // Inizializzazione globale

void set_default_config() {
    config.WORK_DAY_HOURS = 8;
    config.DAY_SIMULATION_TIME = 5;
    config.SIM_DURATION = 5;
    config.BREAK_PROBABILITY = 0;
    config.NOF_WORKERS = 8;
    config.NOF_USERS = 150;
    config.NOF_WORKER_SEATS = 8;
    config.NOF_PAUSE = 3;
    config.P_SERV_MIN = 70;
    config.P_SERV_MAX = 100;
    config.EXPLODE_THRESHOLD = 1000;
    config.OFFICE_OPEN_TIME = 0;
    config.OFFICE_CLOSE_TIME = 480;
    calculate_derived_values();
}

void calculate_derived_values() {
    config.WORK_DAY_MINUTES = config.WORK_DAY_HOURS * 60;
    config.TOTAL_SIMULATION_TIME = config.SIM_DURATION * config.DAY_SIMULATION_TIME;
    config.N_NANO_SECS = (config.DAY_SIMULATION_TIME * 1000000000L) / config.WORK_DAY_MINUTES;
}

int read_config(const char* config_file) {
    FILE* file = fopen(config_file, "r");
    if (!file) {
        printf("Attenzione: Impossibile aprire %s, uso configurazione di default\n", config_file);
        set_default_config();
        return 0;
    }
    
    char line[256];
    char key[128];
    int value;
    
    // Imposta valori di default prima di leggere
    set_default_config();
    
    while (fgets(line, sizeof(line), file)) {
        // Ignora commenti e righe vuote
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        
        // Parse formato KEY=VALUE
        if (sscanf(line, "%[^=]=%d", key, &value) == 2) {
            if (strcmp(key, "WORK_DAY_HOURS") == 0) config.WORK_DAY_HOURS = value;
            else if (strcmp(key, "DAY_SIMULATION_TIME") == 0) config.DAY_SIMULATION_TIME = value;
            else if (strcmp(key, "SIM_DURATION") == 0) {
                if (value > MAX_SIM_DURATION) {
                    value = MAX_SIM_DURATION;
                }
                config.SIM_DURATION = value;
            }
            else if (strcmp(key, "BREAK_PROBABILITY") == 0) config.BREAK_PROBABILITY = value;
            else if (strcmp(key, "NOF_WORKERS") == 0) {
                if (value > MAX_WORKERS) {
                    value = MAX_WORKERS;
                }
                config.NOF_WORKERS = value;
            }
            else if (strcmp(key, "NOF_USERS") == 0) {
                if (value > MAX_USERS) {
                    value = MAX_USERS;
                }
                config.NOF_USERS = value;
            }
            else if (strcmp(key, "NOF_WORKER_SEATS") == 0) {
                if (value > MAX_WORKER_SEATS) {
                    value = MAX_WORKER_SEATS;
                }
                config.NOF_WORKER_SEATS = value;
            }
            else if (strcmp(key, "NOF_PAUSE") == 0) config.NOF_PAUSE = value;
            else if (strcmp(key, "P_SERV_MIN") == 0) config.P_SERV_MIN = value;
            else if (strcmp(key, "P_SERV_MAX") == 0) config.P_SERV_MAX = value;
            else if (strcmp(key, "EXPLODE_THRESHOLD") == 0) config.EXPLODE_THRESHOLD = value;
            else if (strcmp(key, "OFFICE_OPEN_TIME") == 0) config.OFFICE_OPEN_TIME = value;
            else if (strcmp(key, "OFFICE_CLOSE_TIME") == 0) config.OFFICE_CLOSE_TIME = value;
        }
    }
    
    fclose(file);
    calculate_derived_values();
    
    return 1;
}

#endif // CONFIG_READER_H
