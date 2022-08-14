#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/wait.h>


// struct for the semaphore
struct mySemaphore {
  int val;
  sem_t gate;
  sem_t mutex;
};
typedef struct mySemaphore mySemaphore;

typedef struct printer {
  int pid;
  int size;
  struct timespec start_time;
}
printer;

// global variables to connect to shared memory
typedef struct global {
  int completedProducers;
  int bufferIndex;
  int totalProdCreated;
  int totalConsRemoved;
  int numBytesProd;
  int numBytesCons;
  int numProducers;

  mySemaphore full_sem;
  mySemaphore empty_sem;
  mySemaphore binary_sem;
  mySemaphore safety;
  printer printerJobBuffer[30];

  pid_t producerChildId[100];
  pthread_t producerThreads[100];
  pthread_t consumerThreads[100];

}
global;

global * globalShared;
int shmid;
pid_t parent;
pthread_t * threads;
int numProcess;
int numThread;
int producerId[100];
int consumerId[100];
pthread_t consumerThread[100];

// inputs
int numProducers;
int numConsumers;

// time variables
struct timespec start;
struct timespec end;
double totalWaitTime;

float time_diff(struct timespec * start, struct timespec * end) {
  return (end -> tv_sec - start -> tv_sec) + 1e-9 * (end -> tv_nsec - start -> tv_nsec);
}

// initialize the semaphores
void mysem_init(struct mySemaphore * sem, int k) {
  sem -> val = k; // value of csem

  if (sem -> val > 0)
    sem_init( & sem -> gate, 1, 1); // 1 if val > 0
  else
    sem_init( & sem -> gate, 1, 0); // 0 if val = 0

  sem_init( & sem -> mutex, 1, 1); // protects val

}

// wait function
void mysem_wait(struct mySemaphore * sem) {
  sem_wait( & sem -> gate);
  sem_wait( & sem -> mutex);

  sem -> val = sem -> val - 1;
  if (sem -> val > 0)
    sem_post( & sem -> gate);

  sem_post( & sem -> mutex);

}

// post function
void mysem_post(struct mySemaphore * sem) {
  sem_wait( & sem -> mutex);

  sem -> val = sem -> val + 1;
  if (sem -> val == 1)
    sem_post( & sem -> gate);

  sem_post( & sem -> mutex);

}

// destroy semaphores function
void mysem_destroy(struct mySemaphore * sem) {
  sem -> val = 0;
  sem_destroy( & sem -> gate);
  sem_destroy( & sem -> mutex);
}

//, global* buffer
void insertbuffer(printer job) {
  if (globalShared -> bufferIndex < 30) {
    globalShared -> printerJobBuffer[globalShared -> bufferIndex++] = job;
  } else {
    printf("Buffer overflow\n");
    fflush(stdout);
  }
}

printer dequeuebuffer() {
  if (globalShared -> bufferIndex > 0) {
    return globalShared -> printerJobBuffer[--globalShared -> bufferIndex];
  } else {
    printf("Buffer underflow\n");
    fflush(stdout);
    exit(1);
  }
}

void producer() {
  printer job;
  job.pid = getpid();

  // random num of producers generated
  int producerLoops = rand() % 30 + 1;
  globalShared -> totalProdCreated += producerLoops;

  // set printer job struct vars
  int a;
  for (a = 0; a < producerLoops; a++) {
    job.size = rand() % (1000 - 100 + 1) + 100;
    globalShared -> numBytesProd += job.size;

    mysem_wait( & globalShared -> full_sem);

    // protect critical section
    mysem_wait( & globalShared -> binary_sem);
    clock_gettime(CLOCK_REALTIME, & job.start_time);
    insertbuffer(job);
    mysem_post( & globalShared -> binary_sem);

    mysem_post( & globalShared -> empty_sem);

    printf("Producer <%d> added <%d> to the buffer\n", job.pid, job.size);
    fflush(stdout);

    // sleep for time to give some time for the producers
    if (a % 2 == 0)
      usleep((rand() % (1000 - 100 + 1)) + 100);

  }
  // increment number of completed producers
  mysem_wait( & globalShared -> safety);
  globalShared -> completedProducers++;
  mysem_wait( & globalShared -> safety);
  exit(0);
}

void * consumer(void * thread_n) {
  struct timespec end_time;
  int threadNum = * (int * ) thread_n;
  printer job;

  // keep consuming until the num of producers completed = the num of processes total
  mysem_wait( & globalShared -> safety);
  int flag = 1;
  while (flag == 1) {
    // if it is done and gets stuck, break out
    if (globalShared -> totalProdCreated == globalShared -> totalConsRemoved)
      break;

    globalShared -> totalConsRemoved += 1;
    mysem_wait( & globalShared -> empty_sem);

    // protect critical section
    mysem_wait( & globalShared -> binary_sem);
    clock_gettime(CLOCK_REALTIME, & end_time);
    job = dequeuebuffer(job);
    globalShared -> numBytesCons += job.size;
    mysem_post( & globalShared -> binary_sem);

    mysem_post( & globalShared -> full_sem);

    totalWaitTime += time_diff( & job.start_time, & end_time);
    printf("Consumer <%d> dequed <%d, %d> from buffer\n", getpid(), job.pid, job.size);
    fflush(stdout);
    // once the consumer has removed the number of producer loops that were created, it is done consuming
    if (globalShared -> totalProdCreated == globalShared -> totalConsRemoved)
      flag = 0;
  }
  mysem_post( & globalShared -> safety);
  pthread_exit(0);

}

void terminationH(int num) {
  int b, n;

  // destory semaphores
  mysem_destroy( & globalShared -> binary_sem);
  mysem_destroy( & globalShared -> full_sem);
  mysem_destroy( & globalShared -> empty_sem);
  mysem_destroy( & globalShared -> safety);

  // cancel all the threads
  for (b = 0; b < numThread; b++) {
    n = pthread_cancel(threads[b]);
  }
  // detach the shared memory
  if (shmdt(globalShared) == -1)
    printf("Unable to detach\n");
  shmctl(shmid, IPC_RMID, NULL);

  exit(0);

}

int main(int argc, char * argv[]) {
  printf("Starting\n");
  fflush(stdout);

  // initialize signal
  signal(SIGINT, terminationH);

  // key
  key_t key = 6324;

  // connect to shared memory
  if ((shmid = shmget(key, sizeof(global), IPC_CREAT | 0666)) == -1) {
    perror("shmget");
    fflush(stdout);
    exit(1);
  }
  /*
   * Now we attach the segment to our data space.
   */
  if ((globalShared = shmat(shmid, NULL, 0)) == (global * ) - 1) {
    perror("shmat");
    fflush(stdout);
    exit(1);
  }

  // get the pid
  parent = getpid();

  // input from user
  numProcess = atoi(argv[1]);
  globalShared -> numProducers = numProcess;
  numThread = atoi(argv[2]);

  // initialize globals
  globalShared -> bufferIndex = 0;
  globalShared -> completedProducers = 0;
  globalShared -> totalProdCreated = 0;
  globalShared -> totalConsRemoved = 0;

  // initialize semaphores
  mysem_init( & globalShared -> binary_sem, 1);
  mysem_init( & globalShared -> full_sem, 30);
  mysem_init( & globalShared -> empty_sem, 0);
  mysem_init( & globalShared -> safety, 1);

  // set time
  totalWaitTime = 0;
  clock_gettime(CLOCK_REALTIME, & start);

  // produce and consume
  int i;
  for (i = 0; i < numProcess; i++) {
    if (parent == getpid()) // create producer processes
      if (fork() == 0) { // child process do nothing
      }
  }
  // parent process create the threads, consume, and join them
  if (parent == getpid()) {
    int s;
    for (s = 0; s < numThread; s++) {
      consumerId[s] = s;
      pthread_create(consumerThread + s, NULL, consumer, consumerId + s);
    }
    threads = consumerThread;
    int h;
    for (h = 0; h < numThread; h++)
      pthread_join(consumerThread[h], NULL);
  }
  // create the producers 
  else
    producer();

  // end the time tracking
  clock_gettime(CLOCK_REALTIME, & end);

  // print execution time and average job time
  printf("Total time taken: %f\n", time_diff( & start, & end));
  printf("Average job time: %f\n", totalWaitTime / globalShared -> totalConsRemoved);

  // finish program, destroy semaphores
  printf("Finished\n");
  fflush(stdout);
  mysem_destroy( & globalShared -> binary_sem);
  mysem_destroy( & globalShared -> empty_sem);
  mysem_destroy( & globalShared -> full_sem);
  mysem_destroy( & globalShared -> safety);

  // cancel all the threads
  int b, n;
  for (b = 0; b < numThread; b++) {
    n = pthread_cancel(threads[b]);
  }

  // detach shared memory
  if (shmdt(globalShared) == -1) {
    printf("error detaching\n");
    fflush(stdout);
  }
  shmctl(shmid, IPC_RMID, NULL);

  return 0;
}