#include "model.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
volatile sig_atomic_t interrupted;
int log_fd = -1;
uint64_t now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}
void pause_ms(int ms)
{
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) && errno == EINTR && !interrupted) {}
}
static void write_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t k = write(fd, s, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) break;
        s += k; n -= (size_t)k;
    }
}
void event(Model *m, const char *format, ...)
{
    char b[1024];
    int n = snprintf(b, sizeof(b), "[%6llu ms pid=%ld] ",
        (unsigned long long)(now_ms() - m->epoch_ms), (long)getpid());
    va_list ap; va_start(ap, format);
    vsnprintf(b + n, sizeof(b) - (size_t)n, format, ap); va_end(ap);
    size_t len = strlen(b);
    b[len++] = '\n';
    write_all(STDOUT_FILENO, b, len);
    if (log_fd >= 0) write_all(log_fd, b, len);
}
void model_init(Model *m, const Config *c)
{
    memset(m, 0, sizeof(*m)); m->config = *c; m->epoch_ms = now_ms();
    for (int i = 0; i < MAX_WORKERS; ++i) m->cook_job[i] = -1;
    for (int e = 0; e < 4; ++e)
        for (int u = 0; u < MAX_WORKERS; ++u) m->equipment_owner[e][u] = -1;
    for (int i = 0; i < c->visitor_count; ++i) m->orders[i].waiter = -1;
}
static int better(Model *m, int a, int b)
{
    if (b < 0) return 1;
    Job *x = &m->jobs[a], *y = &m->jobs[b];
    uint64_t xs = m->orders[x->order].sequence, ys = m->orders[y->order].sequence;
    if (xs != ys) return m->config.lifo ? xs > ys : xs < ys;
    return x->sequence < y->sequence;
}
static int free_unit(Model *m, int e)
{
    for (int i = 0; i < m->config.equipment[e]; ++i)
        if (m->equipment_owner[e][i] < 0) return i;
    return -1;
}
void model_tick(Model *m)
{
    Config *c = &m->config;
    if (m->stop) return;
    for (int o = 0; o < c->visitor_count; ++o) {
        Order *order = &m->orders[o];
        if (order->state != 2) continue;
        int missing = 0;
        for (int d = 0; d < c->dish_count[o]; ++d) {
            DishSpec *spec = &c->menu[c->dishes[o][d]];
            for (int k = 0; k < spec->count; ++k)
                if (!c->equipment[spec->steps[k].equipment]) missing = 1;
        }
        if (missing) {
            order->state = 6; ++m->canceled; ++m->finished;
            event(m, "CANCELED order=%d reason=missing_equipment", o + 1);
            continue;
        }
        order->state = 3;
        for (int d = 0; d < c->dish_count[o]; ++d) {
            int j = m->job_count++;
            m->jobs[j] = (Job){.order=o, .dish=d, .state=1, .cook=-1, .unit=-1};
            DishSpec *spec = &c->menu[c->dishes[o][d]];
            for (int k = 0; k < spec->count; ++k)
                event(m, "CREATE order=%d dish=%d name='%s' step=%d", o+1,d+1,spec->name,k+1);
        }
    }
    /* Блюда забираются по одному: вместимость зоны не ограничивает размер заказа. */
    for (int j = 0; j < m->job_count; ++j) {
        Job *job = &m->jobs[j];
        if (job->state != 6) continue;
        job->state = 7; --m->ready;
        Order *o = &m->orders[job->order]; ++o->collected;
        event(m, "COLLECT order=%d dish=%d", job->order+1, job->dish+1);
        if (o->collected == c->dish_count[job->order]) {
            o->state = 4;
            event(m, "ASSEMBLED order=%d dishes=%d", job->order+1,o->collected);
        }
    }
    for (;;) {
        if (c->kitchen_cap && m->queued >= c->kitchen_cap) break;
        int best = -1;
        for (int j = 0; j < m->job_count; ++j)
            if (m->jobs[j].state == 1 && better(m,j,best)) best = j;
        if (best < 0) break;
        Job *j = &m->jobs[best]; j->state = 2; j->sequence = ++m->sequence;
        ++m->queued;
        if (m->queued > m->max_queued) m->max_queued = m->queued;
        event(m, "QUEUED job=%d order=%d dish=%d step=%d", best,j->order+1,j->dish+1,j->step+1);
    }
}
Reply model_apply(Model *m, Request r)
{
    Reply out = {0,0,0}; Config *c = &m->config;
    int limit = r.role == VISITOR ? c->visitor_count : r.role == WAITER ? c->waiter_count : c->cook_count;
    if (r.role < 0 || r.role > COOK || r.id < 0 || r.id >= limit) return (Reply){-2,0,0};
    if (r.action == HELLO) {
        if (m->registered[r.role][r.id]) return (Reply){-2,0,0};
        m->registered[r.role][r.id] = 1; ++m->active;
        event(m, "REGISTER role=%d id=%d actor_pid=%d",r.role,r.id+1,r.value);
        uint64_t arrival = (uint64_t)r.id * (unsigned)c->interval;
        if (c->gap_count) {
            arrival = 0;
            for (int i=0;i<=r.id;++i) arrival += (unsigned)c->gaps[i];
        }
        out.duration = (int)(arrival * (unsigned)c->unit_ms > 2147483647ULL ? 2147483647 : arrival * (unsigned)c->unit_ms);
        return out;
    }
    if (!m->registered[r.role][r.id]) return (Reply){-2,0,0};
    if (r.action == BYE) {
        if (!m->departed[r.role][r.id]) { m->departed[r.role][r.id] = 1; --m->active; }
        return out;
    }
    if (r.action == STOP) { m->stop = 1; if (!m->signal_number) m->signal_number = r.value; }
    if (m->stop) return (Reply){-1,0,0};
    if (r.action == ARRIVE && r.role == VISITOR) {
        Order *o = &m->orders[r.id];
        if (!o->state) {
            o->state = 1; o->sequence = ++m->sequence; ++m->arrived;
            event(m, "ARRIVAL visitor=%d order=%d",r.id+1,r.id+1);
            if (m->arrived == c->visitor_count) event(m, "ADMISSION_CLOSED visitors=%d",c->visitor_count);
        }
    } else if (r.action == VISIT_STATUS && r.role == VISITOR) {
        int s = m->orders[r.id].state;
        out.code = s == 5 || s == 6 ? 2 : 0;
    } else if (r.action == WAITER_STEP && r.role == WAITER) {
        int own = -1;
        for (int o=0;o<c->visitor_count;++o)
            if (m->orders[o].waiter == r.id && m->orders[o].state >= 2 && m->orders[o].state <= 4) own=o;
        if (own >= 0) {
            Order *o = &m->orders[own];
            if (o->state == 4) {
                o->state=5; ++m->delivered; ++m->finished;
                event(m,"DELIVER waiter=%d order=%d visitor=%d",r.id+1,own+1,own+1);
            }
        } else {
            for (int o=0;o<c->visitor_count;++o) if (m->orders[o].state == 1) {
                m->orders[o].state=2; m->orders[o].waiter=r.id; ++m->accepted;
                event(m,"ACCEPT waiter=%d order=%d visitor=%d",r.id+1,o+1,o+1); break;
            }
        }
        if (m->finished == c->visitor_count) out.code=2;
    } else if (r.action == COOK_TAKE && r.role == COOK) {
        if (m->cook_job[r.id] >= 0) return (Reply){-2,0,0};
        if (m->finished == c->visitor_count) return (Reply){2,0,0};
        int best=-1;
        for (int j=0;j<m->job_count;++j) {
            Job *job=&m->jobs[j];
            DishSpec *spec=&c->menu[c->dishes[job->order][job->dish]];
            if (job->state==2 && free_unit(m,spec->steps[job->step].equipment)>=0 && better(m,j,best)) best=j;
        }
        if (best>=0) {
            Job *j=&m->jobs[best];
            OperationSpec op=c->menu[c->dishes[j->order][j->dish]].steps[j->step];
            int u=free_unit(m,op.equipment);
            m->equipment_owner[op.equipment][u]=r.id;
            m->cook_job[r.id]=best; j->unit=u; j->cook=r.id; j->state=3; --m->queued;
            j->started_ms=now_ms(); ++m->started;
            event(m,"EQUIPMENT cook=%d type=%d unit=%d",r.id+1,op.equipment,u+1);
            event(m,"START cook=%d job=%d order=%d dish=%d step=%d",r.id+1,best,j->order+1,j->dish+1,j->step+1);
            out=(Reply){1,best,op.duration*c->unit_ms};
        }
    } else if (r.action == COOK_FINISH && r.role == COOK) {
        int id=m->cook_job[r.id];
        if (id<0 || id!=r.value) return (Reply){-2,0,0};
        Job *j=&m->jobs[id]; DishSpec *spec=&c->menu[c->dishes[j->order][j->dish]];
        if (j->state==3) {
            m->equipment_owner[spec->steps[j->step].equipment][j->unit]=-1;
            m->busy_ms += now_ms()-j->started_ms; ++m->completed;
            event(m,"END cook=%d job=%d order=%d dish=%d step=%d",r.id+1,id,j->order+1,j->dish+1,j->step+1);
            ++j->step;
            if (j->step < spec->count) {
                j->state=1; m->cook_job[r.id]=-1; j->cook=-1;
                return (Reply){1,0,0};
            }
            j->state=5; ++m->dishes_ready;
            event(m,"DISH_READY order=%d dish=%d",j->order+1,j->dish+1);
        }
        if (j->state==5 && (!c->ready_cap || m->ready<c->ready_cap)) {
            j->state=6; ++m->ready; m->cook_job[r.id]=-1; j->cook=-1;
            if (m->ready>m->max_ready) m->max_ready=m->ready;
            event(m,"READY_AREA order=%d dish=%d",j->order+1,j->dish+1);
            out.code=1;
        }
    }
    return out;
}
int model_check(Model *m)
{
    int q=0,ready=0,running[4][MAX_WORKERS]={{0}},busy[MAX_WORKERS]={0};
    Config *c=&m->config;
    for (int i=0;i<m->job_count;++i) {
        Job *j=&m->jobs[i]; DishSpec *s=&c->menu[c->dishes[j->order][j->dish]];
        if (j->step<0 || j->step>s->count) return -1;
        q += j->state==2; ready += j->state==6;
        if (j->state==3) {
            if (j->step>=s->count || j->cook<0 || j->cook>=c->cook_count || j->unit<0) return -1;
            int e=s->steps[j->step].equipment;
            if (j->unit>=c->equipment[e] || ++running[e][j->unit]>1 || ++busy[j->cook]>1 ||
                m->equipment_owner[e][j->unit]!=j->cook || m->cook_job[j->cook]!=i) return -1;
        }
        if (j->state>=5 && j->step!=s->count) return -1;
    }
    for (int e=0;e<4;++e) for (int u=0;u<c->equipment[e];++u)
        if ((m->equipment_owner[e][u]>=0) != (running[e][u]==1)) return -1;
    for (int cook=0;cook<c->cook_count;++cook) {
        int j=m->cook_job[cook];
        if (j>=0 && (j>=m->job_count || m->jobs[j].cook!=cook ||
            (m->jobs[j].state!=3 && m->jobs[j].state!=5))) return -1;
    }
    if (m->delivered+m->canceled!=m->finished) return -1;
    if (q!=m->queued || ready!=m->ready || (c->kitchen_cap && q>c->kitchen_cap) ||
        (c->ready_cap && ready>c->ready_cap)) return -1;
    for (int o=0;o<c->visitor_count;++o)
        if ((m->orders[o].state==4 || m->orders[o].state==5) && m->orders[o].collected!=c->dish_count[o]) return -1;
    ++m->invariant_checks;
    return 0;
}
void model_summary(Model *m)
{
    event(m,"SUMMARY termination=%s arrived=%d accepted=%d delivered=%d canceled=%d total=%d started=%d completed=%d dishes_ready=%d max_kitchen=%d max_ready=%d checks=%d busy_ms=%llu",
        m->stop?"interrupted":"normal",m->arrived,m->accepted,m->delivered,m->canceled,
        m->config.visitor_count,m->started,m->completed,m->dishes_ready,m->max_queued,m->max_ready,
        m->invariant_checks,(unsigned long long)m->busy_ms);
}
