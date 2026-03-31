CC      = gcc
CFLAGS  = -Wall -Wextra -g
LDFLAGS = -lm

TARGETS = controller worker

.PHONY: all clean

all: $(TARGETS)

controller: controller.c montecarlo.h
	$(CC) $(CFLAGS) -o controller controller.c $(LDFLAGS)

worker: worker.c montecarlo.h
	$(CC) $(CFLAGS) -o worker worker.c

clean:
	rm -f $(TARGETS)
