
all:
	gcc -o mq main.c -lrt -lconfig

clean:
	rm -f mq

