/*
 * Copyright © 2012 Intel Corporation
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include "config.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include "weston-test-runner.h"

extern const struct weston_test __start_test_section, __stop_test_section;

static const struct weston_test *
find_test(const char *name)
{
	const struct weston_test *t;

	for (t = &__start_test_section; t < &__stop_test_section; t++)
		if (strcmp(t->name, name) == 0)
			return t;

	return NULL;
}

static void
run_test(const struct weston_test *t, void *data)
{
	t->run(data);
	exit(EXIT_SUCCESS);
}

static void
list_tests(void)
{
	const struct weston_test *t;

	fprintf(stderr, "Available test names:\n");
	for (t = &__start_test_section; t < &__stop_test_section; t++)
		fprintf(stderr, "	%s\n", t->name);
}

static int
exec_and_report_test(const struct weston_test *t, void *test_data, int iteration)
{
	int success = 0;
	int hardfail = 0;
	siginfo_t info;

	pid_t pid = fork();
	assert(pid >= 0);

	if (pid == 0)
		run_test(t, test_data); /* never returns */

	if (waitid(P_ALL, 0, &info, WEXITED)) {
		fprintf(stderr, "waitid failed: %m\n");
		abort();
	}

	if (test_data)
		fprintf(stderr, "test \"%s/%i\":\t", t->name, iteration);
	else
		fprintf(stderr, "test \"%s\":\t", t->name);

	switch (info.si_code) {
	case CLD_EXITED:
		fprintf(stderr, "exit status %d", info.si_status);
		if (info.si_status == EXIT_SUCCESS)
			success = 1;
		break;
	case CLD_KILLED:
	case CLD_DUMPED:
		fprintf(stderr, "signal %d", info.si_status);
		if (info.si_status != SIGABRT)
			hardfail = 1;
		break;
	}

	if (t->must_fail)
		success = !success;

	if (success && !hardfail) {
		fprintf(stderr, ", pass.\n");
		return 1;
	} else { 
		fprintf(stderr, ", fail.\n");
		return 0;
	}
}

/* Returns number of tests and number of pass / fail in param args */
static int
iterate_test(const struct weston_test *t, int *passed)
{
	int i;
	void *current_test_data = (void *) t->table_data;
	for (i = 0; i < t->n_elements; ++i, current_test_data += t->element_size)
	{
		if (exec_and_report_test(t, current_test_data, i))
			++(*passed);
	}

	return t->n_elements;
}

int main(int argc, char *argv[])
{
	const struct weston_test *t;
	int total = 0;
	int pass = 0;

	if (argc == 2) {
		const char *testname = argv[1];
		if (strcmp(testname, "--help") == 0 ||
		    strcmp(testname, "-h") == 0) {
			fprintf(stderr, "Usage: %s [test-name]\n", program_invocation_short_name);
			list_tests();
			exit(EXIT_SUCCESS);
		}

		t = find_test(argv[1]);
		if (t == NULL) {
			fprintf(stderr, "unknown test: \"%s\"\n", argv[1]);
			list_tests();
			exit(EXIT_FAILURE);
		}

		int number_passed_in_test = 0;
		total += iterate_test(t, &number_passed_in_test);
		pass += number_passed_in_test;
	} else {
		for (t = &__start_test_section; t < &__stop_test_section; t++) {
			int number_passed_in_test = 0;
			total += iterate_test(t, &number_passed_in_test);
			pass += number_passed_in_test;
		}
	}

	fprintf(stderr, "%d tests, %d pass, %d fail\n",
		total, pass, total - pass);

	return pass == total ? EXIT_SUCCESS : EXIT_FAILURE;
}
