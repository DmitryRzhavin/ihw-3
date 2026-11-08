#include "model.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) ++s;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) --e;
    *e = 0;
    return s;
}
static int number(const char *s, int *v)
{
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (!*s || *s == '-' || errno || *end || n < 0 || n > 1000000) return -1;
    *v = (int)n;
    return 0;
}
static int parse_dish(char *s, Config *c)
{
    if (c->menu_count == MAX_DISHES) return -1;
    char *bar = strchr(s, '|');
    if (!bar) return -1;
    *bar++ = 0;
    s = trim(s);
    if (!*s || strlen(s) >= 64) return -1;
    for (int i = 0; i < c->menu_count; ++i)
        if (!strcmp(s, c->menu[i].name)) return -1;
    DishSpec *d = &c->menu[c->menu_count];
    strcpy(d->name, s);
    const char *names[] = {"stove", "oven", "grill", "table"};
    char *token;
    while ((token = strsep(&bar, ",")) != NULL) {
        if (d->count == MAX_STEPS) return -1;
        char *colon = strchr(token, ':');
        if (!colon) return -1;
        *colon++ = 0;
        int e;
        for (e = 0; e < 4; ++e) if (!strcmp(trim(token), names[e])) break;
        OperationSpec *op = &d->steps[d->count++];
        if (e == 4 || number(trim(colon), &op->duration) || !op->duration || op->duration > 100000) return -1;
        op->equipment = e;
    }
    if (!d->count) return -1;
    ++c->menu_count;
    return 0;
}
static int setting(char *k, char *v, Config *c)
{
    if (!strcmp(k, "dish")) return parse_dish(v, c);
    if (!strcmp(k, "order")) {
        if (c->order_count == MAX_VISITORS) return -1;
        int o = c->order_count++;
        char *t;
        while ((t = strsep(&v, ",")) != NULL) {
            int i;
            for (i = 0; i < c->menu_count; ++i)
                if (!strcmp(trim(t), c->menu[i].name)) break;
            if (i == c->menu_count || c->dish_count[o] == MAX_ORDER_DISHES) return -1;
            c->dishes[o][c->dish_count[o]++] = i;
        }
        return c->dish_count[o] ? 0 : -1;
    }
    if (!strcmp(k, "arrival_intervals")) {
        char *t;
        while ((t = strsep(&v, ",")) != NULL) {
            if (c->gap_count == MAX_VISITORS || number(trim(t), &c->gaps[c->gap_count++])) return -1;
        }
        return 0;
    }
    if (!strcmp(k, "priority")) {
        if (strcmp(v, "fifo") && strcmp(v, "lifo")) return -1;
        c->lifo = !strcmp(v, "lifo");
        return 0;
    }
    int n;
    if (number(v, &n)) return -1;
    /* Конечное число посетителей задаётся единственным ключом visitors в конфигах */
    if (!strcmp(k, "visitors")) c->visitor_count = n;
    else if (!strcmp(k, "waiters")) c->waiter_count = n;
    else if (!strcmp(k, "cooks")) c->cook_count = n;
    else if (!strcmp(k, "arrival_interval")) c->interval = n;
    else if (!strcmp(k, "unit_ms")) c->unit_ms = n;
    else if (!strcmp(k, "kitchen_queue_capacity")) c->kitchen_cap = n;
    else if (!strcmp(k, "ready_queue_capacity")) c->ready_cap = n;
    else if (!strcmp(k, "dishes_per_order")) c->per_order = n;
    else if (!strcmp(k, "seed")) c->seed = (unsigned)n;
    else if (!strcmp(k, "equipment_stove")) c->equipment[0] = n;
    else if (!strcmp(k, "equipment_oven")) c->equipment[1] = n;
    else if (!strcmp(k, "equipment_grill")) c->equipment[2] = n;
    else if (!strcmp(k, "equipment_table")) c->equipment[3] = n;
    else return -1;
    return 0;
}
int config_load_file(const char *path, Config *c)
{
    memset(c, 0, sizeof(*c));
    c->unit_ms = 30; c->per_order = 2; c->seed = 42;
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }
    char line[2048]; int line_number = 0, bad = 0;
    while (fgets(line, sizeof(line), f)) {
        ++line_number;
        if (!strchr(line, '\n') && !feof(f)) { bad = 1; break; }
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        char *eq = strchr(s, '=');
        if (!eq) { bad = 1; break; }
        *eq++ = 0;
        if (setting(trim(s), trim(eq), c)) { bad = 1; break; }
    }
    if (ferror(f)) bad = 1;
    fclose(f);
    if (bad) { fprintf(stderr, "%s:%d: invalid configuration\n", path, line_number); return -1; }
    if (c->visitor_count < 1 || c->visitor_count > MAX_VISITORS ||
        c->waiter_count < 1 || c->waiter_count > MAX_WORKERS ||
        c->cook_count < 1 || c->cook_count > MAX_WORKERS || !c->menu_count ||
        c->unit_ms < 1 || c->unit_ms > 10000 ||
        c->kitchen_cap > MAX_JOBS || c->ready_cap > MAX_JOBS ||
        (c->gap_count && c->gap_count != c->visitor_count) ||
        (c->order_count && c->order_count != c->visitor_count)) goto invalid;
    for (int i = 0; i < 4; ++i) if (c->equipment[i] > MAX_WORKERS) goto invalid;
    if (!c->order_count) {
        if (c->per_order < 1 || c->per_order > MAX_ORDER_DISHES) goto invalid;
        unsigned x = c->seed ? c->seed : 0xA341316Cu;
        for (int o = 0; o < c->visitor_count; ++o) {
            c->dish_count[o] = c->per_order;
            for (int j = 0; j < c->per_order; ++j) {
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                c->dishes[o][j] = (int)(x % (unsigned)c->menu_count);
            }
        }
    }
    return 0;
invalid:
    fprintf(stderr, "%s: configuration outside documented limits\n", path);
    return -1;
}
