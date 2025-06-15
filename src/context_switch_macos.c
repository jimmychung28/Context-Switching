/*
 * Context Switching Cost Measurement - macOS/Cross-platform version
 * Measures the overhead of context switches without CPU affinity
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

#define TIMES 1000
#define BILLION 1e9

int main(int argc, char *argv[]) {
    int pipefd_1[2], pipefd_2[2];
    struct timespec start, stop;
    char testChar = 'a';

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
            close(pipefd_1[0]);     /* Close unused read end */
            close(pipefd_2[1]);     /* Close unused write end */

            char readChar_c;
            for (int i = 0; i < TIMES; ++i) {
                while (read(pipefd_2[0], &readChar_c, 1) <= 0) {}
                write(pipefd_1[1], &readChar_c, 1);
            }

            close(pipefd_2[0]);
            close(pipefd_1[1]);
            exit(EXIT_SUCCESS);

        default:    /* parent process */
            close(pipefd_2[0]);     /* Close unused read end */
            close(pipefd_1[1]);     /* Close unused write end */

            char readChar_p;
            
            clock_gettime(CLOCK_MONOTONIC, &start);
            for (int i = 0; i < TIMES; ++i) {
                write(pipefd_2[1], &testChar, 1);
                while (read(pipefd_1[0], &readChar_p, 1) <= 0) {}
            }
            clock_gettime(CLOCK_MONOTONIC, &stop);

            close(pipefd_2[1]);
            close(pipefd_1[0]);

            double elapsed = ((stop.tv_sec - start.tv_sec) * BILLION + 
                             stop.tv_nsec - start.tv_nsec) / TIMES;
            
            printf("Average context switching time: %.2f nanoseconds\n", elapsed);
            printf("Total time for %d iterations: %.2f microseconds\n", 
                   TIMES, elapsed * TIMES / 1000);
    }

    exit(EXIT_SUCCESS);
}