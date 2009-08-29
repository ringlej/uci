/*
 * ucimap - library for mapping uci sections into data structures
 * Copyright (C) 2008 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <strings.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "ucimap.h"

struct uci_alloc {
	enum ucimap_type type;
	union {
		void **ptr;
	} data;
};

struct uci_fixup {
	struct list_head list;
	struct uci_sectmap *sm;
	const char *name;
	enum ucimap_type type;
	union ucimap_data *data;
};

struct uci_sectmap_data {
	struct list_head list;
	struct uci_map *map;
	struct uci_sectmap *sm;
	const char *section_name;

	/* list of allocations done by ucimap */
	struct uci_alloc *allocmap;
	unsigned long allocmap_len;

	/* map for changed fields */
	unsigned char *cmap;
	bool done;
};


#define ucimap_foreach_option(_sm, _o) \
	if (!(_sm)->options_size) \
		(_sm)->options_size = sizeof(struct uci_optmap); \
	for (_o = &(_sm)->options[0]; \
		 ((char *)(_o)) < ((char *) &(_sm)->options[0] + \
			(_sm)->options_size * (_sm)->n_options); \
		 _o = (struct uci_optmap *) ((char *)(_o) + \
			(_sm)->options_size))


static inline bool
ucimap_is_alloc(enum ucimap_type type)
{
	switch(type & UCIMAP_SUBTYPE) {
	case UCIMAP_STRING:
		return true;
	default:
		return false;
	}
}

static inline bool
ucimap_is_fixup(enum ucimap_type type)
{
	switch(type & UCIMAP_SUBTYPE) {
	case UCIMAP_SECTION:
		return true;
	default:
		return false;
	}
}

static inline bool
ucimap_is_simple(enum ucimap_type type)
{
	return ((type & UCIMAP_TYPE) == UCIMAP_SIMPLE);
}

static inline bool
ucimap_is_list(enum ucimap_type type)
{
	return ((type & UCIMAP_TYPE) == UCIMAP_LIST);
}

static inline union ucimap_data *
ucimap_get_data(struct uci_sectmap_data *sd, struct uci_optmap *om)
{
	void *data;

	data = (char *) sd + sizeof(struct uci_sectmap_data) + om->offset;
	return data;
}

int
ucimap_init(struct uci_map *map)
{
	INIT_LIST_HEAD(&map->sdata);
	INIT_LIST_HEAD(&map->fixup);
	return 0;
}

static void
ucimap_free_item(struct uci_alloc *a)
{
	switch(a->type & UCIMAP_TYPE) {
	case UCIMAP_SIMPLE:
	case UCIMAP_LIST:
		free(a->data.ptr);
		break;
	}
}

static void
ucimap_add_alloc(struct uci_sectmap_data *sd, void *ptr)
{
	struct uci_alloc *a = &sd->allocmap[sd->allocmap_len++];
	a->type = UCIMAP_SIMPLE;
	a->data.ptr = ptr;
}

static void
ucimap_free_section(struct uci_map *map, struct uci_sectmap_data *sd)
{
	void *section = sd;
	int i;

	section = (char *) section + sizeof(struct uci_sectmap_data);
	if (!list_empty(&sd->list))
		list_del(&sd->list);

	if (sd->sm->free_section)
		sd->sm->free_section(map, section);

	for (i = 0; i < sd->allocmap_len; i++) {
		ucimap_free_item(&sd->allocmap[i]);
	}

	free(sd->allocmap);
	free(sd);
}

void
ucimap_cleanup(struct uci_map *map)
{
	struct list_head *ptr, *tmp;

	list_for_each_safe(ptr, tmp, &map->sdata) {
		struct uci_sectmap_data *sd = list_entry(ptr, struct uci_sectmap_data, list);
		ucimap_free_section(map, sd);
	}
}

static void
ucimap_add_fixup(struct uci_map *map, union ucimap_data *data, struct uci_optmap *om, const char *str)
{
	struct uci_fixup *f;

	f = malloc(sizeof(struct uci_fixup));
	if (!f)
		return;

	INIT_LIST_HEAD(&f->list);
	f->sm = om->data.sm;
	f->name = str;
	f->type = om->type;
	f->data = data;
	list_add(&f->list, &map->fixup);
}

static void
ucimap_add_value(union ucimap_data *data, struct uci_optmap *om, struct uci_sectmap_data *sd, const char *str)
{
	union ucimap_data *tdata = data;
	char *eptr = NULL;
	char *s;
	int val;

	if (ucimap_is_list(om->type) && !ucimap_is_fixup(om->type))
		tdata = &data->list->item[data->list->n_items++];

	switch(om->type & UCIMAP_SUBTYPE) {
	case UCIMAP_STRING:
		if ((om->data.s.maxlen > 0) &&
			(strlen(str) > om->data.s.maxlen))
			return;

		s = strdup(str);
		tdata->s = s;
		ucimap_add_alloc(sd, s);
		break;
	case UCIMAP_BOOL:
		val = -1;
		if (strcmp(str, "on"))
			val = true;
		else if (strcmp(str, "1"))
			val = true;
		else if (strcmp(str, "enabled"))
			val = true;
		else if (strcmp(str, "off"))
			val = false;
		else if (strcmp(str, "0"))
			val = false;
		else if (strcmp(str, "disabled"))
			val = false;
		if (val == -1)
			return;

		tdata->b = val;
		break;
	case UCIMAP_INT:
		val = strtol(str, &eptr, om->data.i.base);
		if (!eptr || *eptr == '\0')
			tdata->i = val;
		else
			return;
		break;
	case UCIMAP_SECTION:
		ucimap_add_fixup(sd->map, data, om, str);
		break;
	}
}


static int
ucimap_parse_options(struct uci_map *map, struct uci_sectmap *sm, struct uci_sectmap_data *sd, struct uci_section *s)
{
	struct uci_element *e, *l;
	struct uci_option *o;
	union ucimap_data *data;

	uci_foreach_element(&s->options, e) {
		struct uci_optmap *om = NULL, *tmp;

		ucimap_foreach_option(sm, tmp) {
			if (strcmp(e->name, tmp->name) == 0) {
				om = tmp;
				break;
			}
		}
		if (!om)
			continue;

		data = ucimap_get_data(sd, om);
		o = uci_to_option(e);
		if ((o->type == UCI_TYPE_STRING) && ucimap_is_simple(om->type)) {
			ucimap_add_value(data, om, sd, o->v.string);
		} else if ((o->type == UCI_TYPE_LIST) && ucimap_is_list(om->type)) {
			uci_foreach_element(&o->v.list, l) {
				ucimap_add_value(data, om, sd, l->name);
			}
		}
	}

	return 0;
}


static int
ucimap_parse_section(struct uci_map *map, struct uci_sectmap *sm, struct uci_section *s)
{
	struct uci_sectmap_data *sd = NULL;
	struct uci_optmap *om;
	char *section_name;
	void *section;
	int n_alloc = 2;
	int err;

	sd = malloc(sm->alloc_len + sizeof(struct uci_sectmap_data));
	if (!sd)
		return UCI_ERR_MEM;

	memset(sd, 0, sm->alloc_len + sizeof(struct uci_sectmap_data));
	INIT_LIST_HEAD(&sd->list);

	ucimap_foreach_option(sm, om) {
		if (ucimap_is_list(om->type)) {
			union ucimap_data *data;
			struct uci_element *e;
			int n_elements = 0;
			int size;

			data = ucimap_get_data(sd, om);
			uci_foreach_element(&s->options, e) {
				struct uci_option *o = uci_to_option(e);
				struct uci_element *tmp;

				if (strcmp(e->name, om->name) != 0)
					continue;

				uci_foreach_element(&o->v.list, tmp) {
					n_elements++;
				}
				break;
			}
			n_alloc += n_elements + 1;
			size = sizeof(struct ucimap_list) +
				n_elements * sizeof(union ucimap_data);
			data->list = malloc(size);
			memset(data->list, 0, size);
		} else if (ucimap_is_alloc(om->type)) {
			n_alloc++;
		}
	}

	sd->map = map;
	sd->sm = sm;
	sd->allocmap = malloc(n_alloc * sizeof(struct uci_alloc));
	if (!sd->allocmap)
		goto error_mem;

	section_name = strdup(s->e.name);
	if (!section_name)
		goto error_mem;

	sd->section_name = section_name;

	sd->cmap = malloc(BITFIELD_SIZE(sm->n_options));
	if (!sd->cmap)
		goto error_mem;

	memset(sd->cmap, 0, BITFIELD_SIZE(sm->n_options));
	ucimap_add_alloc(sd, (void *)section_name);
	ucimap_add_alloc(sd, (void *)sd->cmap);
	ucimap_foreach_option(sm, om) {
		if (!ucimap_is_list(om->type))
			continue;

		ucimap_add_alloc(sd, ucimap_get_data(sd, om)->list);
	}

	section = (char *)sd + sizeof(struct uci_sectmap_data);

	err = sm->init_section(map, section, s);
	if (err)
		goto error;

	list_add(&sd->list, &map->sdata);
	err = ucimap_parse_options(map, sm, sd, s);
	if (err)
		goto error;

	return 0;

error_mem:
	if (sd->allocmap)
		free(sd->allocmap);
	free(sd);
	return UCI_ERR_MEM;

error:
	ucimap_free_section(map, sd);
	return err;
}

static int
ucimap_fill_ptr(struct uci_ptr *ptr, struct uci_section *s, const char *option)
{
	struct uci_package *p = s->package;

	memset(ptr, 0, sizeof(struct uci_ptr));

	ptr->package = p->e.name;
	ptr->p = p;

	ptr->section = s->e.name;
	ptr->s = s;

	ptr->option = option;
	return uci_lookup_ptr(p->ctx, ptr, NULL, false);
}

void
ucimap_set_changed(void *section, void *field)
{
	char *sptr = (char *)section - sizeof(struct uci_sectmap_data);
	struct uci_sectmap_data *sd = (struct uci_sectmap_data *) sptr;
	struct uci_sectmap *sm = sd->sm;
	struct uci_optmap *om;
	int ofs = (char *)field - (char *)section;
	int i;

	ucimap_foreach_option(sm, om) {
		if (om->offset == ofs) {
			SET_BIT(sd->cmap, i);
			break;
		}
	}
}

int
ucimap_store_section(struct uci_map *map, struct uci_package *p, void *section)
{
	char *sptr = (char *)section - sizeof(struct uci_sectmap_data);
	struct uci_sectmap_data *sd = (struct uci_sectmap_data *) sptr;
	struct uci_sectmap *sm = sd->sm;
	struct uci_section *s = NULL;
	struct uci_optmap *om;
	struct uci_element *e;
	struct uci_ptr ptr;
	int i = 0;
	int ret;

	uci_foreach_element(&p->sections, e) {
		if (!strcmp(e->name, sd->section_name)) {
			s = uci_to_section(e);
			break;
		}
	}
	if (!s)
		return UCI_ERR_NOTFOUND;

	ucimap_foreach_option(sm, om) {
		union ucimap_data *data;
		static char buf[32];
		const char *str = NULL;

		data = ucimap_get_data(sd, om);
		if (!TEST_BIT(sd->cmap, i))
			continue;

		ucimap_fill_ptr(&ptr, s, om->name);
		switch(om->type & UCIMAP_SUBTYPE) {
		case UCIMAP_STRING:
			str = data->s;
			break;
		case UCIMAP_INT:
			sprintf(buf, "%d", data->i);
			str = buf;
			break;
		case UCIMAP_BOOL:
			sprintf(buf, "%d", !!data->b);
			str = buf;
			break;
		}
		ptr.value = str;

		ret = uci_set(s->package->ctx, &ptr);
		if (ret)
			return ret;

		CLR_BIT(sd->cmap, i);
		i++;
	}

	return 0;
}

void *
ucimap_find_section(struct uci_map *map, struct uci_fixup *f)
{
	struct uci_sectmap_data *sd;
	struct list_head *p;
	void *ret;

	list_for_each(p, &map->sdata) {
		sd = list_entry(p, struct uci_sectmap_data, list);
		if (sd->sm != f->sm)
			continue;
		if (strcmp(f->name, sd->section_name) != 0)
			continue;
		ret = (char *)sd + sizeof(struct uci_sectmap_data);
		return ret;
	}
	return NULL;
}

void
ucimap_parse(struct uci_map *map, struct uci_package *pkg)
{
	struct uci_element *e;
	struct list_head *p, *tmp;
	int i;

	INIT_LIST_HEAD(&map->fixup);
	uci_foreach_element(&pkg->sections, e) {
		struct uci_section *s = uci_to_section(e);

		for (i = 0; i < map->n_sections; i++) {
			if (strcmp(s->type, map->sections[i]->type) != 0)
				continue;
			ucimap_parse_section(map, map->sections[i], s);
		}
	}
	list_for_each_safe(p, tmp, &map->fixup) {
		struct uci_fixup *f = list_entry(p, struct uci_fixup, list);
		void *ptr = ucimap_find_section(map, f);
		struct ucimap_list *list;

		if (!ptr)
			continue;

		switch(f->type & UCIMAP_TYPE) {
		case UCIMAP_SIMPLE:
			f->data->section = ptr;
			break;
		case UCIMAP_LIST:
			list = f->data->list;
			list->item[list->n_items++].section = ptr;
			break;
		}
		free(f);
	}
	list_for_each_safe(p, tmp, &map->sdata) {
		struct uci_sectmap_data *sd = list_entry(p, struct uci_sectmap_data, list);
		void *section;

		if (sd->done)
			continue;

		section = (char *) sd + sizeof(struct uci_sectmap_data);
		if (sd->sm->add_section(map, section) != 0)
			ucimap_free_section(map, sd);
	}
}
