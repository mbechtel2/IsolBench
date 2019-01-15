/**
 *
 * Copyright (C) 2012  Heechul Yun <heechul@illinois.edu>
 *               2012  Zheng <zpwu@uwaterloo.ca>
 *
 * This file is distributed under the University of Illinois Open Source
 * License. See LICENSE.TXT for details.
 *
 */

/* clang -S -mllvm --x86-asm-syntax=intel ./bandwidth.c */

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/

/**************************************************************************
 * Included Files
 **************************************************************************/
#define _GNU_SOURCE             /* See feature_test_macros(7) */
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <getopt.h>
#include <time.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/
#define CACHE_LINE_SIZE 64	   /* cache Line size is 64 byte */
#ifdef __arm__
#  define DEFAULT_ALLOC_SIZE_KB 4096
#else
#  define DEFAULT_ALLOC_SIZE_KB 16384
#endif

/**************************************************************************
 * Public Types
 **************************************************************************/
enum access_type { READ, WRITE};

struct periodic_info
{
	/* Opaque data */
	int sig;
	timer_t timer_id;	
	sigset_t alarm_sig;
	int wakeups_missed;	
};

/**************************************************************************
 * Global Variables
 **************************************************************************/
int g_mem_size = DEFAULT_ALLOC_SIZE_KB * 1024;	   /* memory size */
int *g_mem_ptr = 0;		   /* pointer to allocated memory region */

volatile uint64_t g_nread = 0;	           /* number of bytes read */
volatile unsigned int g_start;		   /* starting time */
int cpuid = 0;

/**************************************************************************
 * Public Functions
 **************************************************************************/
unsigned int get_usecs()
{
	struct timeval         time;
	gettimeofday(&time, NULL);
	return (time.tv_sec * 1000000 +	time.tv_usec);
}

void quit(int param)
{
	float dur_in_sec;
	float bw;
	float dur = get_usecs() - g_start;
	dur_in_sec = (float)dur / 1000000;
	printf("g_nread(bytes read) = %lld\n", (long long)g_nread);
	printf("elapsed = %.2f sec ( %.0f usec )\n", dur_in_sec, dur);
	bw = (float)g_nread / dur_in_sec / 1024 / 1024;
	printf("CPU%d: B/W = %.2f MB/s | ",cpuid, bw);
	printf("CPU%d: average = %.2f ns\n", cpuid, (dur*1000)/(g_nread/CACHE_LINE_SIZE));
	exit(0);
}

int64_t bench_read()
{
	int i;	
	int64_t sum = 0;
	for ( i = 0; i < g_mem_size/4; i+=(CACHE_LINE_SIZE/4) ) {
		sum += g_mem_ptr[i];
	}
	g_nread += g_mem_size;
	return sum;
}

int bench_write()
{
	register int i;
	char *ptr = (char *)g_mem_ptr;
	for ( i = 0; i < g_mem_size; i+=(CACHE_LINE_SIZE) ) {
		ptr[i] = 0xff;
	}
	g_nread += g_mem_size;
	return 1;
}

int make_periodic (int unsigned period, struct periodic_info *info)
{
	static int next_sig;
	int ret;
	unsigned int ns;
	unsigned int sec;
	struct sigevent sigev;

	struct itimerspec itval;

	/* Initialise next_sig first time through. We can't use static
	   initialisation because SIGRTMIN is a function call, not a constant */
	if (next_sig == 0)
		next_sig = SIGRTMIN;
	/* Check that we have not run out of signals */
	if (next_sig > SIGRTMAX)
		return -1;
	info->sig = next_sig;
	next_sig++;

	info->wakeups_missed = 0;

	/* Create the signal mask that will be used in wait_period */
	sigemptyset (&(info->alarm_sig));
	sigaddset (&(info->alarm_sig), info->sig);

	/* Create a timer that will generate the signal we have chosen */
	sigev.sigev_notify = SIGEV_SIGNAL;
	sigev.sigev_signo = info->sig;
	sigev.sigev_value.sival_ptr = (void *) &info->timer_id;
	ret = timer_create (CLOCK_MONOTONIC, &sigev, &info->timer_id);
	if (ret == -1)
		return ret;

	/* Make the timer periodic */
	sec = period/1000000;
	ns = (period - (sec * 1000000)) * 1000;
	itval.it_interval.tv_sec = sec;
	itval.it_interval.tv_nsec = ns;
	itval.it_value.tv_sec = sec;
	itval.it_value.tv_nsec = ns;
	ret = timer_settime (info->timer_id, 0, &itval, NULL);
	return ret;
}

void wait_period (struct periodic_info *info)
{
	int sig;
	sigwait (&(info->alarm_sig), &sig);
        info->wakeups_missed += timer_getoverrun (info->timer_id);
}

void usage(int argc, char *argv[])
{
	printf("Usage: $ %s [<option>]*\n\n", argv[0]);
	printf("-m: memory size in KB. deafult=8192\n");
	printf("-a: access type - read, write. default=read\n");
	printf("-n: addressing pattern - Seq, Row, Bank. default=Seq\n");
	printf("-t: time to run in sec. 0 means indefinite. default=5. \n");
	printf("-c: CPU to run.\n");
	printf("-i: iterations. 0 means intefinite. default=0\n");
	printf("-p: priority\n");
	printf("-l: log label. use together with -f\n");
	printf("-f: log file name\n");
	printf("-h: help\n");
	printf("\nExamples: \n$ bandwidth -m 8192 -a read -t 1 -c 2\n  <- 8MB read for 1 second on CPU 2\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int64_t sum = 0;
	unsigned finish = 5;
	int prio = 0;        
	int num_processors;
	int acc_type = READ;
	int opt;
	cpu_set_t cmask;
	int iterations = 0;
	int jobs = 0;
	int period = 0; /* in ms */
	int verbose = 0;
	int i, j;
	struct sched_param param;
	struct periodic_info info;
	sigset_t alarm_sig;
	
	static struct option long_options[] = {
		{"period",  required_argument, 0,  'l' },
		{"verbose", required_argument, 0,  'v' },
		{0,         0,                 0,  0 }
	};
	int option_index = 0;
	/*
	 * get command line options 
	 */
	while ((opt = getopt_long(argc, argv, "m:a:t:c:r:p:i:j:l:hv:",
				  long_options, &option_index)) != -1) {
		switch (opt) {
		case 'm': /* set memory size */
			g_mem_size = 1024 * strtol(optarg, NULL, 0);
			break;
		case 'a': /* set access type */
			if (!strcmp(optarg, "read"))
				acc_type = READ;
			else if (!strcmp(optarg, "write"))
				acc_type = WRITE;
			else
				exit(1);
			break;
			
		case 't': /* set time in secs to run */
			finish = strtol(optarg, NULL, 0);
			break;
			
		case 'c': /* set CPU affinity */
			cpuid = strtol(optarg, NULL, 0);
			num_processors = sysconf(_SC_NPROCESSORS_CONF);
			CPU_ZERO(&cmask);
			CPU_SET(cpuid % num_processors, &cmask);
			if (sched_setaffinity(0, num_processors, &cmask) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned to cpu %d\n", cpuid);
			break;

		case 'r':
			prio = strtol(optarg, NULL, 0);
			param.sched_priority = prio; /* 1(low)- 99(high) for SCHED_FIFO or SCHED_RR
						        0 for SCHED_OTHER or SCHED_BATCH */
			if(sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
				perror("sched_setscheduler failed");
			}
			break;
		case 'p': /* set priority */
			prio = strtol(optarg, NULL, 0);
			if (setpriority(PRIO_PROCESS, 0, prio) < 0)
				perror("error");
			else
				fprintf(stderr, "assigned priority %d\n", prio);
			break;
		case 'i': /* iterations of a job */
			iterations = strtol(optarg, NULL, 0);
			break;
		case 'j': /* #of jobs */
			jobs = strtol(optarg, NULL, 0);
			break;
		case 'l': /* period -> determine P (ms)*/
			period = strtol(optarg, NULL, 0);
			sigemptyset (&alarm_sig);
			for (i = SIGRTMIN; i <= SIGRTMAX; i++)
				sigaddset (&alarm_sig, i);
			sigprocmask (SIG_BLOCK, &alarm_sig, NULL);
			break;
		case 'h': 
			usage(argc, argv);
			break;
		case 'v':
			verbose = strtol(optarg, NULL, 0);
			break;
		}
	}

	/*
	 * allocate contiguous region of memory 
	 */ 
	g_mem_ptr = (int *)malloc(g_mem_size);

	memset((char *)g_mem_ptr, 1, g_mem_size);

	for (i = 0; i < g_mem_size / sizeof(int); i++)
		g_mem_ptr[i] = i;

	/* print experiment info before starting */
	printf("memsize=%d KB, type=%s, cpuid=%d, iterations=%d, period=%d\n",
	       g_mem_size/1024,
	       ((acc_type==READ) ?"read": "write"),
	       cpuid,
	       iterations,
	       period);
	printf("stop at %d\n", finish);

	/* set signals to terminate once time has been reached */
	signal(SIGINT, &quit);
	if (finish > 0) {
		signal(SIGALRM, &quit);
		alarm(finish);
	}

	/*
	 * actual memory access
	 */
	if (period > 0) make_periodic(period * 1000, &info);
	g_start = get_usecs();
	for (j = 0;; j++) {
		unsigned int l_start, l_end, l_duration;
		l_start = get_usecs();
		for (i = 0;; i++) {
			switch (acc_type) {
			case READ:
				sum += bench_read();
				break;
			case WRITE:
				sum += bench_write();
				break;
			}
			if (verbose > 1) printf(".");
			if (iterations > 0 && i+1 >= iterations)
				break;
		}
		l_end = get_usecs();
		l_duration = l_end - l_start;
		if (period > 0) wait_period (&info);
		if (verbose) fprintf(stderr, "\nTook %d us\n", l_duration);
		if (jobs == 0 || j+1 >= jobs)
			break;
	}
	printf("total sum = %ld\n", (long)sum);
	quit(0);
	return 0;
}

