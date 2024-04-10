#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <semaphore.h>

#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))
#define ASSERT(msg,source) \
		if (!(source)) (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(msg), kill(0, SIGKILL), exit(EXIT_FAILURE));


#define MAX_NAME 16

typedef struct {
	int fd;
	char name[MAX_NAME];
	sem_t sem;
} Mutex;

Mutex * mtx_open(char const * name) {
	int fd = shm_open(name,O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	ASSERT("shm_open",fd!=-1);
	ASSERT("ftruncate",ftruncate(fd,sizeof(Mutex))==0);
	Mutex * mtx = (Mutex*)mmap(NULL,sizeof(Mutex),PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ASSERT("mmap",mtx!=MAP_FAILED);
	strncpy(mtx->name, name, MAX_NAME);
	sem_init(&mtx->sem,1,1);
	return mtx;
}

void mtx_lock(Mutex * mtx) {
	sem_wait(&mtx->sem);
}

void mtx_unlock(Mutex * mtx) {
	sem_post(&mtx->sem);
}

void mtx_close(Mutex * mtx) {
	sem_destroy(&mtx->sem);
	munmap(mtx,sizeof(Mutex));
	shm_unlink(mtx->name);
}

typedef struct {
	int fd;
	char name[MAX_NAME];
	int strength;
	int count;
	sem_t mutex;
	sem_t turnstile_pre;
	sem_t turnstile_post;
} Barrier;

Barrier * br_open(char const * name, int strength) {
	int fd = shm_open(name,O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
	ASSERT("shm_open",fd!=-1);
	ASSERT("ftruncate",ftruncate(fd,sizeof(Barrier))==0);
	Barrier * barrier = (Barrier*)mmap(NULL,sizeof(Barrier),PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ASSERT("mmap",barrier!=MAP_FAILED);
	strncpy(barrier->name, name, MAX_NAME);
	printf("Barrier: %s\n",barrier->name);
	barrier->strength = strength;
	barrier->count = 0;
	sem_init(&barrier->mutex,1,1);
	sem_init(&barrier->turnstile_pre,1,0);
	sem_init(&barrier->turnstile_post,1,0);
	return barrier;
};

void br_join(Barrier * barrier) {
	// Synchronize
	sem_wait(&barrier->mutex);
		barrier->count+=1;
		if (barrier->count==barrier->strength) {
			for (int i = 0; i<barrier->strength; i++) {
				sem_post(&barrier->turnstile_pre);
			}
		}
	sem_post(&barrier->mutex);
	sem_wait(&barrier->turnstile_pre);
	// Return to valid state
	sem_wait(&barrier->mutex);
		barrier->count-=1;
		if (barrier->count==0) {
			for (int i = 0; i<barrier->strength; i++) {
				sem_post(&barrier->turnstile_post);
			}
		}
	sem_post(&barrier->mutex);
	sem_wait(&barrier->turnstile_post);	
}

void br_close(Barrier * barrier) {
	sem_destroy(&barrier->mutex);
	sem_destroy(&barrier->turnstile_pre);
	sem_destroy(&barrier->turnstile_post);
	shm_unlink(barrier->name);
	munmap(barrier,sizeof(Barrier));
}

/// TASK PART

#define SHM_SIZE sizeof(int)*5
#define MAX_CARDS 10
#define MAX_PLAYERS 5

void usage(char* name) {
    fprintf(stderr, "USAGE: %s N M\n", name);
    exit(EXIT_FAILURE);
}

void read_arguments(int argc, char* argv[], int*n,int*m) {
  if (argc!=3) usage(argv[0]);
  (*n)=atoi(argv[1]);
  (*m)=atoi(argv[2]);
  if (*n<1 || 5<*n || *m<3 || *m>10) usage(argv[0]);
}

typedef struct {
	char name[MAX_NAME];
	int players,cards;
	int buf[MAX_PLAYERS];
} Server;

Server *server_open(char const * name, int n, int m) {
	int fd = shm_open(name, O_CREAT | O_RDWR | O_TRUNC | O_EXCL, S_IRUSR | S_IWUSR);
	ASSERT("shm_open",fd!=-1);
	ASSERT("ftruncate",ftruncate(fd,sizeof(Server))==0);
	Server * server = (Server*)mmap(NULL,sizeof(Server),PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	ASSERT("mmap",server!=MAP_FAILED);
	strncpy(server->name, name, MAX_NAME);
	server->players=n;
	server->cards=m;
	return server;
}

void server_close(Server * server) {
	shm_unlink(server->name);
	munmap(server,sizeof(Server));
}

typedef struct {
	Server * server;
	Barrier * barrier;
	sem_t * announce;
} Data;

typedef struct {
	int index;
	int cards[MAX_CARDS+1];
	int remaining;
} Worker;

void worker_run(Worker * worker, Data * data);

void worker_new(int index, Data * data) {
	Worker worker;
	worker.index = index;
	worker.remaining = data->server->cards;
	for (int i = 1; i<=worker.remaining; i++) {
		worker.cards[i]=i;
	}
	pid_t  pid = fork();
	ASSERT("fork",pid!=-1);
	if (pid==0) {
		srand(time(NULL)*getpid());
		worker_run(&worker,data);
		exit(EXIT_SUCCESS);
	}
}

int worker_draw(Worker * worker) {
	int index = 1+rand()%worker->remaining;
	int card = worker->cards[index];
	worker->cards[index]=worker->cards[worker->remaining];
	worker->remaining--;
	return card;
}

void worker_run(Worker * worker, Data * data) {
	for (int i = 0; i<data->server->cards; i++) {
		sem_wait(data->announce);
		if (i>0) printf("%d: received %d points!\n",worker->index,data->server->buf[worker->index]);
		data->server->buf[worker->index]=worker_draw(worker);
		br_join(data->barrier);
	}
		sem_wait(data->announce);
		printf("%d: received %d points!\n",worker->index,data->server->buf[worker->index]);
}

void server_announce(Data * data) {
	for (int i = 0; i<data->server->players; i++) {
		sem_post(data->announce);
	}
}

void server_run(Data * data) {
	int results[MAX_PLAYERS];
	int max;
	int cnt;
	for (int i = 0; i<MAX_PLAYERS; i++) {
		results[i]=0;
	} 
	server_announce(data);
	for (int i = 0; i<data->server->cards; i++) {
		br_join(data->barrier);
		max = 0;
		cnt = 0;
		for (int j = 0; j<data->server->players; j++) {
			if (data->server->buf[j]>max) {
				max = data->server->buf[j];
				cnt = 1;
			} else if (data->server->buf[j]==max) {
				cnt++;
			}
		}
		for (int j = 0; j<data->server->players; j++) {
			if (data->server->buf[j]==max) {
				printf("*%d* ",data->server->buf[j]);
				results[j]+=data->server->players/cnt;
				data->server->buf[j]=data->server->players/cnt;
			} else {
				printf("%d ",data->server->buf[j]);
				data->server->buf[j]=0;
			}
		}
		printf("\n");
		server_announce(data);
	}
}

int main(int argc, char *argv[]) {
	int n,m;
	read_arguments(argc, argv, &n, &m);
	char name[MAX_NAME];
	memset(name,'\0',MAX_NAME);
	snprintf(name,MAX_NAME,"/%dserv", getpid());
	Server * server = server_open(name, n, m);
	memset(name,'\0',MAX_NAME);
	snprintf(name,MAX_NAME,"/%dbar", getpid());
	Barrier * barrier = br_open(name, n+1);
	memset(name,'\0',MAX_NAME);
	snprintf(name,MAX_NAME,"/%dsem", getpid());
	sem_t * announce = sem_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR, 0);
	printf("%s\n",barrier->name);
	Data data;
	data.server = server;
	data.barrier = barrier;
	data.announce = announce;
	
	for (int i = 0; i<n; i++) {
		worker_new(i, &data);
	}
	
	server_run(&data);
	
	for (int i = 0; i<n; i++) {
		wait(NULL);
	}
	sem_unlink(name);
	br_close(barrier);
	server_close(server);
	return 0;
}
