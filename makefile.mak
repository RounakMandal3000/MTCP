lib: msocket.o
	ar rcs libmsocket.a msocket.o

msocket.o: msocket.h msocket.c
	gcc -fPIC -Wall -c msocket.c

init: lib initmsocket.c
	gcc -Wall -o initmsocket initmsocket.c -L. -lmsocket -pthread

user: lib user1.c user2.c
	gcc -Wall -o user1 user1.c -L. -lmsocket
	gcc -Wall -o user2 user2.c -L. -lmsocket

clean: 
	rm -f *.o libmsocket.a initmsocket user1 user2
	ipcrm --all=shm
	ipcrm --all=sem

all: lib init user msocket.o
	