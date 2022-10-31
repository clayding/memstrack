/*
 * memstrack.c
 *
 * Copyright (C) 2020 Red Hat, Inc., Kairui Song <kasong@redhat.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <malloc.h>

#include <sys/resource.h>

#include "backend/perf.h"
#include "memstrack.h"
#include "tracing.h"
#include "report.h"
#include "proc.h"

int m_debug = 1;

const char* m_report = "task_summary";
char* m_output_path = "memstack.log";
FILE* m_output;

int m_slab;
int m_page = 1;
int m_loop = 1;
int m_buf_size = 4 << 20;

struct pollfd *m_pollfds;
int m_pollfd_num;

int m_log(int level, const char *__restrict fmt, ...){
	if (!m_debug && level <= LOG_LVL_DEBUG) {
		return 0;
	}

	int ret;
	va_list args;
	va_start (args, fmt);
	if (level == LOG_LVL_INFO) {
		ret = vfprintf(m_output, fmt, args);
	} else {
		ret = vfprintf(stderr, fmt, args);
	}
	va_end (args);
	return ret;
}

static void do_exit() {
    do_report(m_report);
	perf_handling_clean();
}

void m_exit(int ret) {
	m_loop = 0;
	exit(ret);
}

static void on_signal(int signal) {
	m_loop = 0;
}

static void tune_glibc() {
	mallopt(M_TOP_PAD, 4096);
	mallopt(M_TRIM_THRESHOLD, 4096);
}

static void set_high_priority() {
	int which = PRIO_PROCESS;
	int priority = -20;
	int ret;
	id_t pid;

	pid = getpid();
	ret = setpriority(which, pid, priority);

	if (ret) {
		log_error("Failed to set high priority with %s.\n", strerror(ret));
	}
}

static void init(void) {
	int ui_fd_num;
	int ret;
    log_warn("Tracing memory allocations, Press ^C to interrupt ...\n");
    ui_fd_num = 0;

    ret = perf_handling_init();
    if (ret) {
        log_error("Failed initializing perf events\n");
        exit(ret);
    }

	m_pollfd_num = ui_fd_num + perf_event_ring_num;

	m_pollfds = malloc(m_pollfd_num * sizeof(struct pollfd));
	if (!m_pollfds) {
		log_error("Out of memory when try alloc fds\n");
	}

	perf_apply_fds(m_pollfds + ui_fd_num);
	
}

static void loop(void) {
	switch (poll(m_pollfds, m_pollfd_num, 250)) {
		// Resizing the terminal causes poll() to return -1
		// case -1:
		default:
		perf_handling_process();
	}
}


int main(int argc, char **argv) {
	tune_glibc();
	m_output = stdout;

	if (getuid() != 0) {
		log_error("This tool requires root permission to work.\n");
		exit(EPERM);
	}

	set_high_priority();

	if (mem_tracing_init()) {
		exit(1);
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	init();


	perf_handling_start();

	while (m_loop) {
		loop();
	}

	do_exit();

	return 0;
}
