
all:
	gcc -o mq main.c -lrt -lconfig -g

clean:
	rm -f mq

