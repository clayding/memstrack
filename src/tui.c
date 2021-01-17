/*
 * tui.c
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
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <malloc.h>
#include <poll.h>
#include <ncurses.h>

#include <sys/timerfd.h>
#include <sys/resource.h>

#include "memstrack.h"
#include "tracing.h"

#define MISC_PAD 3

struct TracenodeViewData {
	bool expended;
};

static enum ui_type {
	UI_TYPE_TASK = 0,
	UI_TYPE_MODULE,
	UI_TYPE_MAX
} ui_type;

static struct pollfd *ui_fds;
static int gen_timerfd(unsigned int period)
{
	int fd;
	unsigned int ns;
	unsigned int sec;
	struct itimerspec itval = {0};

	fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (fd < 0) {
		log_error("Failed creating timer");
		return -1;
	}

	/* Make the timer periodic */
	sec = period / 1000000;
	ns = (period - (sec * 1000000)) * 1000;
	itval.it_interval.tv_sec = sec;
	itval.it_interval.tv_nsec = ns;
	itval.it_value.tv_sec = sec;
	itval.it_value.tv_nsec = ns;

	if (timerfd_settime(fd, 0, &itval, NULL)) {
		log_error("Failed setting timer period.kn");
		return -1;
	}

	return fd;
}

void tui_apply_fds(struct pollfd *fds) {
	fds[0].fd = STDIN_FILENO;
	fds[0].events = POLLIN;

	fds[1].fd = gen_timerfd(1000 * 1000);
	fds[1].events = POLLIN;

	ui_fds = fds;
}

static struct Tracenode **top_tracenodes;
static int tracenode_num;

static void update_top_tracenodes(void) {
	struct Record *record;

	if (top_tracenodes)
		free(top_tracenodes);

	// TODO: No need to free / alloc every time
	if (ui_type == UI_TYPE_TASK) {
		top_tracenodes = (struct Tracenode**) collect_tasks_sorted(1, &tracenode_num);
	} else {
		top_tracenodes = (struct Tracenode**) collect_modules_sorted(1);
		tracenode_num = module_map.size;
	}

	for (int i = 0; i < tracenode_num; ++i) {
		record = (top_tracenodes[i])->record;
		if (!record->blob)
			record->blob = calloc(1, sizeof(struct TracenodeViewData));
	}
};

/*
 * For tracenode info output
 * TODO: Use a window properly
 */
static int line_highlight;
static WINDOW *trace_win;

struct {
	int current;
	int offset;
	int limit;

	bool enabled;

	int line_len;
	char *line_buf;
} tui_info;

static int try_extend_tracenode(struct Tracenode *node, int *curr_line) {
	struct Tracenode** nodes;
	struct TracenodeViewData *view;
	int count, ret = 0;

	if (!curr_line)
		return -1;

	if (!node->record)
		return ret;

	if (!node->record->blob)
		node->record->blob = calloc(1, sizeof(struct TracenodeViewData));

	view = node->record->blob;

	if ((*curr_line)++ == line_highlight) {
		view->expended = !view->expended;
		// TODO: When should all sub records be freed?
		// if (!view->expended && !is_task) {
		// 	depopulate_tracenode(node);
		// }
		return -1;
	}

	if (view->expended && node->children) {
		nodes = collect_tracenodes_sorted(node->children, &count, 1);
		for (int i = 0; i < count; ++i) {
			ret = try_extend_tracenode(nodes[i], curr_line);
			if (ret)
				break;
		}
		free(nodes);
	}

	return ret;
}

static void expend_line(int line) {
	int curr_line = 0;

	for (int i = 0; i < tracenode_num; ++i) {
		if (try_extend_tracenode(top_tracenodes[i], &curr_line))
			break;
	}
}

static int tui_print_tracenode(WINDOW *tracewin, struct Tracenode *node, int indent) {
	struct Tracenode** nodes;
	struct TracenodeViewData *view;

	int count, ret = 0;
	char expand_sym;

	if (!node->record)
		return ret;

	if (!node->record->blob)
		node->record->blob = calloc(1, sizeof(struct TracenodeViewData));

	view = node->record->blob;

	if (view->expended)
		expand_sym = '|';
	else
		expand_sym = '+';

	if (indent == 0) {
		/* It's a task / module */
		if (ui_type == UI_TYPE_TASK) {
			struct Task *task = container_of(node, struct Task, tracenode);
			snprintf(tui_info.line_buf, tui_info.line_len,
				"%c %7ld | %10ld | %s\n",
				expand_sym, task->pid, task->tracenode.record->pages_alloc, task->task_name);
		} else {
			struct Module *module = container_of(node, struct Module, tracenode);
			snprintf(tui_info.line_buf, tui_info.line_len,
				 "%c %10ld |%s\n",
				 expand_sym, module->tracenode.record->pages_alloc, module->name);
		}
	} else {
		/* It's sub tracenode */
		int i; for (i = 0; i < indent && i < tui_info.line_len / 2; ++i)
			tui_info.line_buf[i] = ' ';

		snprintf(tui_info.line_buf + i, tui_info.line_len - i,
			   "%c | %10ld | %s\n", expand_sym, node->record->pages_alloc, get_tracenode_symbol(node));
	}

	tui_info.line_buf[tui_info.line_len] = '\0';

	if (tui_info.current == line_highlight) {
		wattron(tracewin, A_REVERSE);
		mvwprintw(tracewin, tui_info.current + 1, 1,  "%s", tui_info.line_buf);
		wattroff(tracewin, A_REVERSE);
	} else {
		mvwprintw(tracewin, tui_info.current + 1, 1,  "%s", tui_info.line_buf);
	}

	if (tui_info.current++ > tui_info.limit)
		return -1;

	if (view->expended && node->children) {
		nodes = collect_tracenodes_sorted(node->children, &count, 1);
		for (int i = 0; i < count; ++i) {
			ret = tui_print_tracenode(tracewin, nodes[i], indent + 1);
			if (ret)
				break;
		}
		free(nodes);
	}

	return ret;
}

static void update_tracewin(WINDOW *tracewin) {
	/* Clean up the window */
	werase(trace_win);
	box(trace_win, 0, 0);

	/* Window title bar */
	if (ui_type == UI_TYPE_TASK) {
		mvwprintw(tracewin, 0, 1, "    PID   |    Pages   |    Process Command Line\n");
	} else {
		mvwprintw(tracewin, 0, 1, "    Pages    |    Module Name   \n");
	}

	tui_info.current = 0;

	// TODO: only work when it's single thread
	for (int task_n = 0; task_n < tracenode_num; ++task_n) {
		if (tui_print_tracenode(tracewin, top_tracenodes[task_n], 0))
			return;
	}
}

static void update_ui(WINDOW *trace_win) {
	if (!tui_info.enabled) {
		mvprintw(0, 0, "Console is too small\n");
		refresh();
		return;
	}

	mvprintw(0, 0,  "'q': quit, 'r': reload symbols, 'm': switch processes/modules\n");
	mvprintw(1, 0, "Events captured: %lu\n", trace_count);
	mvprintw(2, 0, "Pages being tracked: %lu (%luMB)\n",
			(page_alloc_counter - page_free_counter),
			(page_alloc_counter - page_free_counter) * page_size / SIZE_MB);
	refresh();

	update_tracewin(trace_win);
	wrefresh(trace_win);
}

int tui_update_size(void) {
	int win_startx, win_starty, win_width, win_height;

	if (COLS < 16 || LINES < 8) {
		tui_info.enabled = 0;
		return -1;
	} else {
		tui_info.enabled = 1;
	}

	win_height = LINES - MISC_PAD; // 4 line above for info
	win_width = COLS;
	win_starty = MISC_PAD;
	win_startx = 0;

	if (trace_win)
		delwin(trace_win);

	trace_win = newwin(win_height, win_width, win_starty, win_startx);

	tui_info.line_len = COLS - 3;
	tui_info.line_buf = malloc(tui_info.line_len + 1);

	tui_info.limit = LINES - MISC_PAD - 4;
	tui_info.offset = MISC_PAD + 1;

	return 0;
}

void tui_init(void) {
	need_page_free_always_backtrack();

	load_kallsyms();
	initscr();
	keypad(stdscr, TRUE);
	timeout(-1);
	curs_set(0);
	noecho();
	raw();

	tui_update_size();
}

void tui_loop(void) {
	int ch;
	int ret;

	if (ui_fds[0].revents & POLLIN) {
		/* On UI event */
		ch = getch();
		switch (ch) {
			case 'q':
			case 'Q':
				endwin();
				m_exit(0);
				return;

			case 'm':
			case 'M':
				ui_type++;
				if (ui_type >= UI_TYPE_MAX)
					ui_type = 0;
				break;

			case 'r':
			case 'R':
				load_kallsyms();
				break;

			case ' ':
				expend_line(line_highlight + MISC_PAD);
				break;

			case KEY_UP:
				line_highlight--;
				if (line_highlight < 0)
					line_highlight = 0;
				break;

			case KEY_DOWN:
				line_highlight++;
				if (line_highlight > LINES - MISC_PAD)
					line_highlight = LINES - MISC_PAD;
				break;
		}

		update_top_tracenodes();
		update_ui(trace_win);
	}

	if (ui_fds[1].revents & POLLIN) {
		uint64_t time;
		ret = read(ui_fds[1].fd, &time, sizeof(time));
		if (ret) {
			update_top_tracenodes();
			update_ui(trace_win);
		}
	}
}
