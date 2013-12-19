/*
 * Copyright © 2011 Intel Corporation
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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <ctype.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <wayland-util.h>
#include "config-parser.h"

#define container_of(ptr, type, member) ({				\
	const __typeof__( ((type *)0)->member ) *__mptr = (ptr);	\
	(type *)( (char *)__mptr - offsetof(type,member) );})

struct weston_config_entry {
	char *key;
	char *value;
	struct wl_list link;
};

struct weston_config_section {
	char *name;
	struct wl_list entry_list;
	struct wl_list link;
};

struct weston_config {
	struct wl_list section_list;
	char path[PATH_MAX];
};

static int
open_config_file(struct weston_config *c, const char *name)
{
	const char *config_dir  = getenv("XDG_CONFIG_HOME");
	const char *home_dir	= getenv("HOME");
	const char *config_dirs = getenv("XDG_CONFIG_DIRS");
	const char *p, *next;
	int fd;

	if (name[0] == '/') {
		snprintf(c->path, sizeof c->path, "%s", name);
		return open(name, O_RDONLY | O_CLOEXEC);
	}

	/* Precedence is given to config files in the home directory,
	 * and then to directories listed in XDG_CONFIG_DIRS and
	 * finally to the current working directory. */

	/* $XDG_CONFIG_HOME */
	if (config_dir) {
		snprintf(c->path, sizeof c->path, "%s/%s", config_dir, name);
		fd = open(c->path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}

	/* $HOME/.config */
	if (home_dir) {
		snprintf(c->path, sizeof c->path,
			 "%s/.config/%s", home_dir, name);
		fd = open(c->path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			return fd;
	}

	/* For each $XDG_CONFIG_DIRS: weston/<config_file> */
	if (!config_dirs)
		config_dirs = "/etc/xdg";  /* See XDG base dir spec. */

	for (p = config_dirs; *p != '\0'; p = next) {
		next = strchrnul(p, ':');
		snprintf(c->path, sizeof c->path,
			 "%.*s/weston/%s", (int)(next - p), p, name);
		fd = open(c->path, O_RDONLY | O_CLOEXEC);
		if (fd >= 0)
			return fd;

		if (*next == ':')
			next++;
	}

	/* Current working directory. */
	snprintf(c->path, sizeof c->path, "./%s", name);

	return open(c->path, O_RDONLY | O_CLOEXEC);
}

static struct weston_config_entry *
config_section_get_entry(struct weston_config_section *section,
			 const char *key)
{
	struct weston_config_entry *e;

	if (section == NULL)
		return NULL;
	wl_list_for_each(e, &section->entry_list, link)
		if (strcmp(e->key, key) == 0)
			return e;

	return NULL;
}

WL_EXPORT
struct weston_config_section *
weston_config_get_section(struct weston_config *config, const char *section,
			  const char *key, const char *value)
{
	struct weston_config_section *s;
	struct weston_config_entry *e;

	if (config == NULL)
		return NULL;
	wl_list_for_each(s, &config->section_list, link) {
		if (strcmp(s->name, section) != 0)
			continue;
		if (key == NULL)
			return s;
		e = config_section_get_entry(s, key);
		if (e && strcmp(e->value, value) == 0)
			return s;
	}

	return NULL;
}

WL_EXPORT
int
weston_config_section_get_int(struct weston_config_section *section,
			      const char *key,
			      int32_t *value, int32_t default_value)
{
	struct weston_config_entry *entry;
	char *end;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	*value = strtol(entry->value, &end, 0);
	if (*end != '\0') {
		*value = default_value;
		errno = EINVAL;
		return -1;
	}

	return 0;
}

WL_EXPORT
int
weston_config_section_get_uint(struct weston_config_section *section,
			       const char *key,
			       uint32_t *value, uint32_t default_value)
{
	struct weston_config_entry *entry;
	char *end;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	*value = strtoul(entry->value, &end, 0);
	if (*end != '\0') {
		*value = default_value;
		errno = EINVAL;
		return -1;
	}

	return 0;
}

WL_EXPORT
int
weston_config_section_get_double(struct weston_config_section *section,
				 const char *key,
				 double *value, double default_value)
{
	struct weston_config_entry *entry;
	char *end;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	*value = strtod(entry->value, &end);
	if (*end != '\0') {
		*value = default_value;
		errno = EINVAL;
		return -1;
	}

	return 0;
}

WL_EXPORT
int
weston_config_section_get_string(struct weston_config_section *section,
				 const char *key,
				 char **value, const char *default_value)
{
	struct weston_config_entry *entry;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		if (default_value)
			*value = strdup(default_value);
		else
			*value = NULL;
		errno = ENOENT;
		return -1;
	}

	*value = strdup(entry->value);

	return 0;
}

WL_EXPORT
int
weston_config_section_get_bool(struct weston_config_section *section,
			       const char *key,
			       int *value, int default_value)
{
	struct weston_config_entry *entry;

	entry = config_section_get_entry(section, key);
	if (entry == NULL) {
		*value = default_value;
		errno = ENOENT;
		return -1;
	}

	if (strcmp(entry->value, "false") == 0)
		*value = 0;
	else if (strcmp(entry->value, "true") == 0)
		*value = 1;
	else {
		*value = default_value;
		errno = EINVAL;
		return -1;
	}

	return 0;
}

static struct weston_config_section *
config_add_section(struct weston_config *config, const char *name)
{
	struct weston_config_section *section;

	section = malloc(sizeof *section);
	section->name = strdup(name);
	wl_list_init(&section->entry_list);
	wl_list_insert(config->section_list.prev, &section->link);

	return section;
}

static struct weston_config_entry *
section_add_entry(struct weston_config_section *section,
		  const char *key, const char *value)
{
	struct weston_config_entry *entry;

	entry = malloc(sizeof *entry);
	entry->key = strdup(key);
	entry->value = strdup(value);
	wl_list_insert(section->entry_list.prev, &entry->link);

	return entry;
}

struct weston_config *
weston_config_parse(const char *name)
{
	FILE *fp;
	char line[512], *p;
	struct weston_config *config;
	struct weston_config_section *section = NULL;
	int i, fd;

	config = malloc(sizeof *config);
	if (config == NULL)
		return NULL;

	wl_list_init(&config->section_list);

	fd = open_config_file(config, name);
	if (fd == -1) {
		free(config);
		return NULL;
	}

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		free(config);
		return NULL;
	}

	while (fgets(line, sizeof line, fp)) {
		switch (line[0]) {
		case '#':
		case '\n':
			continue;
		case '[':
			p = strchr(&line[1], ']');
			if (!p || p[1] != '\n') {
				fprintf(stderr, "malformed "
					"section header: %s\n", line);
				fclose(fp);
				weston_config_destroy(config);
				return NULL;
			}
			p[0] = '\0';
			section = config_add_section(config, &line[1]);
			continue;
		default:
			p = strchr(line, '=');
			if (!p || p == line || !section) {
				fprintf(stderr, "malformed "
					"config line: %s\n", line);
				fclose(fp);
				weston_config_destroy(config);
				return NULL;
			}

			p[0] = '\0';
			p++;
			while (isspace(*p))
				p++;
			i = strlen(p);
			while (i > 0 && isspace(p[i - 1])) {
				p[i - 1] = '\0';
				i--;
			}
			section_add_entry(section, line, p);
			continue;
		}
	}

	fclose(fp);

	return config;
}

const char *
weston_config_get_full_path(struct weston_config *config)
{
	return config == NULL ? NULL : config->path;
}

int
weston_config_next_section(struct weston_config *config,
			   struct weston_config_section **section,
			   const char **name)
{
	if (config == NULL)
		return 0;

	if (*section == NULL)
		*section = container_of(config->section_list.next,
					struct weston_config_section, link);
	else
		*section = container_of((*section)->link.next,
					struct weston_config_section, link);

	if (&(*section)->link == &config->section_list)
		return 0;

	*name = (*section)->name;

	return 1;
}

void
weston_config_destroy(struct weston_config *config)
{
	struct weston_config_section *s, *next_s;
	struct weston_config_entry *e, *next_e;

	if (config == NULL)
		return;

	wl_list_for_each_safe(s, next_s, &config->section_list, link) {
		wl_list_for_each_safe(e, next_e, &s->entry_list, link) {
			free(e->key);
			free(e->value);
			free(e);
		}
		free(s->name);
		free(s);
	}

	free(config);
}
