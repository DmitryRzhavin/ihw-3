#ifndef MODEL_H
#define MODEL_H
#include <stdint.h>
#include <signal.h>
#define MAX_VISITORS 64
#define MAX_WORKERS 16
#define MAX_DISHES 16
#define MAX_ORDER_DISHES 8
#define MAX_STEPS 8
#define MAX_JOBS (MAX_VISITORS * MAX_ORDER_DISHES)
typedef struct { int equipment, duration; } OperationSpec;
typedef struct { char name[64]; int count; OperationSpec steps[MAX_STEPS]; } DishSpec;
typedef struct {
    int visitor_count, waiter_count, cook_count, interval, unit_ms;
    int kitchen_cap, ready_cap, lifo, menu_count, order_count, per_order;
    unsigned seed;
    int equipment[4], gaps[MAX_VISITORS], gap_count;
    DishSpec menu[MAX_DISHES];
    int dish_count[MAX_VISITORS], dishes[MAX_VISITORS][MAX_ORDER_DISHES];
} Config;
typedef struct { int state, waiter, collected; uint64_t sequence; } Order;
typedef struct {
    int order, dish, step, state, cook, unit;
    uint64_t sequence, started_ms;
} Job;
typedef struct {
    Config config;
    Order orders[MAX_VISITORS]; Job jobs[MAX_JOBS];
    int job_count, queued, ready, finished, arrived, accepted, delivered, canceled;
    int started, completed, dishes_ready, stop, signal_number;
    int equipment_owner[4][MAX_WORKERS], cook_job[MAX_WORKERS];
    int registered[3][MAX_VISITORS], departed[3][MAX_VISITORS], active;
    int max_queued, max_ready, invariant_checks;
    uint64_t sequence, epoch_ms, busy_ms;
} Model;
enum { VISITOR, WAITER, COOK };
enum { HELLO, ARRIVE, VISIT_STATUS, WAITER_STEP, COOK_TAKE, COOK_FINISH, BYE, STOP };
typedef struct { int action, role, id, value; } Request;
typedef struct { int code, value, duration; } Reply;
extern volatile sig_atomic_t interrupted;
extern int log_fd;
uint64_t now_ms(void);
void pause_ms(int milliseconds);
void event(Model *m, const char *format, ...);
int config_load_file(const char *path, Config *config);
void model_init(Model *m, const Config *c);
Reply model_apply(Model *m, Request r);
void model_tick(Model *m);
int model_check(Model *m);
void model_summary(Model *m);
#endif
