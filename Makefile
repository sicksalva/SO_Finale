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

%.o: %.c config.h
	$(CC) $(CFLAGS) -c $<

clean:
	rm -f $(PROGS) *.o config.h

# Target specifici per ogni configurazione
timeout:  
	@echo "=== Compilazione con configurazione TIMEOUT ==="
	@cp config_timeout.h config.h
	$(MAKE) all

explode:
	@echo "=== Compilazione con configurazione EXPLODE ==="
	@cp config_explode.h config.h
	$(MAKE) all

run: 
	@if [ ! -f config.h ]; then \
		echo "Errore: Nessuna configurazione attiva. Usa 'make timeout' o 'make explode' prima di eseguire."; \
		exit 1; \
	fi
	./direttore

# Target per testare tutte le configurazioni
test-all: test-timeout test-explode

test-timeout:
	@echo "=== Test configurazione TIMEOUT ==="
	$(MAKE) clean  
	$(MAKE) timeout
	@echo "Configurazione TIMEOUT compilata con successo!"

test-explode:
	@echo "=== Test configurazione EXPLODE ==="
	$(MAKE) clean
	$(MAKE) explode  
	@echo "Configurazione EXPLODE compilata con successo!"

.PHONY: all clean timeout explode test-all test-timeout test-explode