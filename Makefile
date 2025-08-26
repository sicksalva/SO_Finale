CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lrt -pthread

# File oggetto
OBJS = direttore.o
PROGS = direttore operatore ticket utente

all: $(PROGS)

direttore: direttore.o
	$(CC) direttore.o -o direttore $(LDFLAGS)

operatore: operatore.o
	$(CC) operatore.o -o operatore $(LDFLAGS)

ticket: ticket.o
	$(CC) ticket.o -o ticket $(LDFLAGS)

utente: utente.o
	$(CC) utente.o -o utente $(LDFLAGS)

%.o: %.c config.h config_reader.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(PROGS) *.o

# Esegui con configurazione specifica
run-explode: all
	@echo "=== Esecuzione con configurazione EXPLODE ==="
	./direttore explode

run-timeout: all
	@echo "=== Esecuzione con configurazione TIMEOUT ==="
	./direttore timeout

run: all
	@echo "=== Esecuzione con configurazione DEFAULT (timeout) ==="
	./direttore

# Target per testare tutte le configurazioni
test-all: all test-explode test-timeout

test-explode:
	@echo "=== Test configurazione EXPLODE ==="
	@if [ -f explode.conf ]; then \
		echo "Configurazione EXPLODE trovata!"; \
		echo "Preview configurazione:"; \
		head -20 explode.conf; \
	else \
		echo "ERRORE: explode.conf non trovato!"; \
		exit 1; \
	fi

test-timeout:
	@echo "=== Test configurazione TIMEOUT ==="
	@if [ -f timeout.conf ]; then \
		echo "Configurazione TIMEOUT trovata!"; \
		echo "Preview configurazione:"; \
		head -20 timeout.conf; \
	else \
		echo "ERRORE: timeout.conf non trovato!"; \
		exit 1; \
	fi

.PHONY: all clean run-explode run-timeout test-all test-explode test-timeout