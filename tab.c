#include <string.h>
#include "tab.h"

static int hash(char *s)
{
	unsigned h = 0x12345678;
	while (*s) {
		h ^= (h >> ((h & 0xf) + 1));
		h += *s++;
		h ^= (h << ((h & 0xf) + 5));
	}
	h &= (TABITEMS - 1);
	return h ? h : 1;
}

void tab_add(struct tab *t, char *s)
{
	int h = hash(s);
	int i = t->n++;
	if (!i)
		i = t->n++;
	t->next[i] = t->head[h];
	t->head[h] = i;
	t->data[i] = s;
}

char *tab_get(struct tab *t, char *s)
{
	int h = t->head[hash(s)];
	while (h && t->data[h]) {
		if (!strcmp(s, t->data[h]))
			return t->data[h];
		h = t->next[h];
	}
	return NULL;
}

void tab_del(struct tab *t, char *s)
{
	int h = hash(s);
	int prev = -1;
	while (h && t->data[h]) {
		if (!strcmp(s, t->data[h])) {
			if (prev)
				t->next[prev] = t->next[h];
			else
				t->head[h] = t->next[h];
			t->next[h] = 0;
			t->data[h] = NULL;
			return;
		}
		prev = h;
		h = t->next[h];
	}
}
