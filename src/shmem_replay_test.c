#define _GNU_SOURCE
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NUM_PROC        16
#define NUM_SIGNALS   9999
#define NUM_MAPS        16
#define PAGE_SIZE         4096
#define MAIN_FD            3
#define MIN_MAP_SIZE   (PAGE_SIZE * 2)
#define MAX_MAP_SIZE  (PAGE_SIZE * 24)
#define NUM_ACCESS     9999999
#define MAX_SYSCALLS     999

#define INFO_SIZE  ((((sizeof(struct Info_t) + sizeof(sig_atomic_t)) / PAGE_SIZE) + 1) * PAGE_SIZE)

typedef struct Info_t
{
    pid_t	observed;
    int32_t fds[NUM_MAPS];
    int32_t sizes[NUM_MAPS];
} Info;

static uint64_t urand(void)
{
    return (uint64_t)(uint32_t)rand();
}

static void usr1(int sig, siginfo_t *siginfo, void *context)
{
    (void)sig;
    (void)siginfo;
    (void)context;
}

void wait_ne(sig_atomic_t* v, sig_atomic_t s)
{
	while(1)
	{
		sig_atomic_t r;
		__atomic_load(v, &r, __ATOMIC_SEQ_CST);
			if(r == s)
				break;
	}
}

int32_t main(int32_t argc, char** argv)
{
    struct timeval tv;
    char tmp[PATH_MAX];
    realpath("/proc/self/exe", tmp);
    if(argc == 1)
    {
        pid_t observePid;
        int32_t mainFd = memfd_create("main_fd", 0);
        gettimeofday(&tv, NULL);
        srand(tv.tv_usec);
        int32_t r = urand() % NUM_PROC;
        
        if(ftruncate(mainFd, INFO_SIZE) == -1)
            return EXIT_FAILURE;
        sig_atomic_t* init = (sig_atomic_t*)mmap(NULL, INFO_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, mainFd, 0);
        if(mainFd != MAIN_FD || init == (sig_atomic_t*)MAP_FAILED)
            return EXIT_FAILURE;
        madvise(init, INFO_SIZE, MADV_DONTFORK);
        for(int32_t i = 0; i < NUM_MAPS; i++)
        {
            struct Info_t* info = (struct Info_t*)(init + 1);
            info->fds[i] = memfd_create("main_fd", 0);
            info->sizes[i] = (urand() % (MAX_MAP_SIZE - MIN_MAP_SIZE)) + MIN_MAP_SIZE;
            if(ftruncate(info->fds[i],  info->sizes[i]) == -1)
                return EXIT_FAILURE;
        }
        
        for(int32_t i = 0; i < NUM_PROC; i++)
        {
            pid_t pid;
            if((pid = fork()) == 0)
            {
                char cseed[64];
                char* args[] = { argv[0], NULL, NULL, NULL };
                usleep(50000);
                gettimeofday(&tv, NULL);
                sprintf(cseed, "%li", tv.tv_usec);
                args[1] = cseed;
                if(r == i) 
                {
                    args[2] = "1";
                    strcat(tmp, "_record");
                }
                execv(tmp, args);
            }
            else
            if(pid == -1)
            {
                return EXIT_FAILURE;
            }
            if(r == i)
                observePid = pid;
        }

        wait_ne(init, NUM_PROC);
        
        sig_atomic_t v = 0;
        __atomic_store (init, &v, __ATOMIC_SEQ_CST);

        for(int i = 0; i < NUM_SIGNALS; i++)
        {
            kill(observePid, SIGUSR1);
            usleep(urand() % 20);
        }
        
        for(int i = 0; i < NUM_PROC; i++)
        {
            wait(NULL);
        }
    }
    else
    {
        int32_t syscalls = 0;
        FILE* out = NULL;
        int32_t observed = argc >= 3;
        char* maps[NUM_MAPS];
        sig_atomic_t* init = (sig_atomic_t*)mmap(NULL, INFO_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, MAIN_FD, 0);
        if(init == (sig_atomic_t*)MAP_FAILED)
        {
            fprintf(stderr, "failed to map mainFd %d\n", getpid());
            return EXIT_FAILURE;
        }
        
        struct Info_t* info = (struct Info_t*)(init + 1);
        for(int32_t i = 0; i < NUM_MAPS; i++)
        {
            maps[i] = (char*)mmap(NULL, info->sizes[i], PROT_READ|PROT_WRITE, MAP_SHARED, info->fds[i], 0);
            if(maps[i] == (char*)MAP_FAILED)
            {
                return EXIT_FAILURE;
            }
        }

        if(observed)
        {
            struct sigaction shn;
            memset (&shn, '\0', sizeof(shn));
            shn.sa_sigaction = &usr1;
            if(sigaction(SIGUSR1, &shn, NULL) == -1)
            {
                return EXIT_FAILURE;
            }
        }
        
        __atomic_add_fetch(init, 1, __ATOMIC_SEQ_CST);

        int64_t seed = strtoll(argv[1], NULL, 10);
        srand(seed);
        
        if(observed)
        {
            out = fopen("test.txt", "w");
            fprintf(stderr, "observed process %d %li\n", getpid(), seed);
        }
        else
        {
            fprintf(stderr, "process %d %li\n", getpid(), seed);
        }

        wait_ne(init, 0);

        for(int32_t i = 0; i < NUM_ACCESS; i++)
        {
            // read random memory pos x
            int32_t value;
            int32_t idx = urand() % NUM_MAPS;
            char* addr = maps[idx] + (urand() % (info->sizes[idx] - 8));
            memcpy(&value, addr , sizeof(value));
            if(out)
            {
                char tmp[128];
                size_t len = sprintf(tmp, "%p %i\n", addr, value);
                fwrite(tmp, len, 1, out);

                if(syscalls++ < MAX_SYSCALLS)
                {
                    struct timespec t;
                    clock_gettime(CLOCK_MONOTONIC, &t);
                }
            }

            // write random + x memory pos
            idx = rand() % NUM_MAPS;
            for(int32_t j = 0; j < 16 && j < (value & (16 - 1)); j++)
                value++;
            addr = maps[idx] + ((urand() + value) % (info->sizes[idx] - 8));
            value = urand();
            memcpy(addr, &value, sizeof(value));
        }

        if(out)
            fclose(out);
    }
    
    return EXIT_SUCCESS;
}
