/*
 ============================================================================
 Name        : Cost.c
 Author      : 
 Version     :
 Copyright   : Your copyright notice
 Description : Hello World in C, Ansi-style
 ============================================================================
 */

#define _GNU_SOURCE
#define _POSIX_C_SOURCE 199309L

#include <stdio.h>
#include <stdlib.h>
#include <sched.h>
#include <unistd.h>
#include <time.h>

#define TIMES 1000
#define BILLION 1e9

int main(int argc, char *argv[]) {
    int pipefd_1[2], pipefd_2[2];

    struct timespec start, stop;
    clockid_t clk_id = CLOCK_REALTIME;

    // for child and parent process run on the same cpu
    cpu_set_t set;
    int parentCPU, childCPU;

    char testChar = 'a';        /* Use for test */

    if (argc != 3) {
        fprintf(stderr, "Usage: %s parent-cpu child-cpu\n",
                argv[0]);
        exit(EXIT_FAILURE);
    }

    parentCPU = atoi(argv[1]);
    childCPU = atoi(argv[2]);

    CPU_ZERO(&set);

    if (pipe(pipefd_1) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    if (pipe(pipefd_2) == -1) {
        perror("pipe");
        exit(EXIT_FAILURE);
    }

    switch (fork()) {
        case -1:    /* error */
            perror("fork");
            exit(EXIT_FAILURE);

        case 0:     /* child process */
            CPU_SET(childCPU, &set);

            if (sched_setaffinity(getpid(), sizeof(set), &set) == -1) {
                perror("set cpu for child process");
                exit(EXIT_FAILURE);
            }

            char readChar_c;

            close(pipefd_1[0]);     /* Close unused read end */
            close(pipefd_2[1]);     /* Close unused write end */

            for (int i = 0; i < TIMES; ++i) {
                while (read(pipefd_2[0], &readChar_c, 1) <= 0) {}       /* read to the first pipe */
                write(pipefd_1[1], &readChar_c, 1);                     /* write to the first pipe */
            }

            close(pipefd_2[0]);
            close(pipefd_1[1]);

            exit(EXIT_SUCCESS);

        default:    /* parent process */
            CPU_SET(parentCPU, &set);

            if (sched_setaffinity(getpid(), sizeof(set), &set) == -1) {
                perror("set cpu for parent process");
                exit(EXIT_FAILURE);
            }

            char readChar_p;

            close(pipefd_2[0]);     /* Close unused read end */
            close(pipefd_1[1]);     /* Close unused write end */

            clock_gettime(clk_id, &start);
            for (int i = 0; i < TIMES; ++i) {
                write(pipefd_2[1], &testChar, 1);                   /* write to the second pipe */
                while (read(pipefd_1[0], &readChar_p, 1) <= 0) {}   /* read to the first pipe */
            }
            clock_gettime(clk_id, &stop);

            close(pipefd_2[1]);
            close(pipefd_1[0]);

            printf("the average cost of context switching is: %lf nsec\n", ((stop.tv_sec - start.tv_sec) * BILLION
                         + stop.tv_nsec - start.tv_nsec) / TIMES);
    }

    exit(EXIT_SUCCESS);
}
