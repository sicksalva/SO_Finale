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
	rm -f $(PROGS) *.o

run: direttore
	./direttore

.PHONY: all clean