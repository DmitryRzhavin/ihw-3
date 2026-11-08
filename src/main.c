#include "model.h"
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef BACKEND
#define BACKEND 0
#endif
#define MAGIC 0x32032026u
typedef struct { unsigned magic; sem_t mutex; Model model; } Shared;
static Shared *shared;
static Model local_model;
static sem_t *gate=SEM_FAILED;
static mqd_t requests=(mqd_t)-1, responses=(mqd_t)-1;
static char shm_name[128], sem_name[128], req_name[128], rep_name[128];
static int owner, shm_created, sem_created, req_created, rep_created;
static int deadline_seconds=60;
static uint64_t deadline;
static void signal_handler(int signal_number) { interrupted=signal_number; }
static struct timespec after_ms(int milliseconds)
{
    struct timespec t; clock_gettime(CLOCK_REALTIME,&t);
    t.tv_nsec += milliseconds*1000000L;
    t.tv_sec += t.tv_nsec/1000000000L; t.tv_nsec %= 1000000000L;
    return t;
}
static int lock_gate(void)
{
    if (!sem_trywait(gate)) return 0;
    if (errno!=EAGAIN && errno!=EINTR) return -1;
    while (now_ms()<deadline) {
        struct timespec t=after_ms(100);
        if (!sem_timedwait(gate,&t)) return 0;
        if (errno!=EINTR && errno!=ETIMEDOUT) break;
    }
    return -1;
}
static int send_message(mqd_t q, const void *data, size_t size)
{
    do {
        struct timespec t=after_ms(100);
        if (!mq_timedsend(q,data,size,0,&t)) return 0;
        if (errno!=EINTR && errno!=ETIMEDOUT) break;
    } while (now_ms()<deadline);
    return -1;
}
static int receive_message(mqd_t q, void *data, size_t size, int once)
{
    do {
        struct timespec t=after_ms(100);
        ssize_t n=mq_timedreceive(q,data,size,NULL,&t);
        if (n==(ssize_t)size) return 0;
        if (n>=0 || (errno!=EINTR && errno!=ETIMEDOUT)) return -1;
        if (once) return 1;
    } while (now_ms()<deadline);
    return -1;
}
static void cleanup(void)
{
    if (requests!=(mqd_t)-1) mq_close(requests);
    if (responses!=(mqd_t)-1) mq_close(responses);
    if (BACKEND!=0 && gate!=SEM_FAILED) sem_close(gate);
    if (owner) {
        if (req_created) mq_unlink(req_name);
        if (rep_created) mq_unlink(rep_name);
        if (sem_created) sem_unlink(sem_name);
        if (shared && BACKEND==0 && shared->magic==MAGIC) sem_destroy(&shared->mutex);
        if (shm_created) shm_unlink(shm_name);
    }
    if (shared) munmap(shared,sizeof(*shared));
    if (log_fd>=0) close(log_fd);
}
static void fail(const char *what)
{
    perror(what); cleanup(); exit(1);
}
static int setup(const char *session, int create, const Config *config)
{
    if (!*session || strlen(session)>60 || strspn(session,"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-")!=strlen(session)) {
        fprintf(stderr,"Session: use 1..60 letters, digits, '_' or '-'\n"); return -1;
    }
    snprintf(shm_name,sizeof(shm_name),"/r32_%s_mem",session);
    snprintf(sem_name,sizeof(sem_name),"/r32_%s_gate",session);
    snprintf(req_name,sizeof(req_name),"/r32_%s_req",session);
    snprintf(rep_name,sizeof(rep_name),"/r32_%s_rep",session);
    owner=create;
    if (BACKEND!=3) {
        int fd=shm_open(shm_name,O_RDWR|(create?(O_CREAT|O_EXCL):0),0600);
        if (fd<0) return -1;
        if (create) {
            shm_created=1;
            if (ftruncate(fd,sizeof(Shared))) {close(fd);return -1;}
        } else {
            struct stat st;
            if (fstat(fd,&st) || st.st_size!=(off_t)sizeof(Shared)) {close(fd);return -1;}
        }
        shared=mmap(NULL,sizeof(*shared),PROT_READ|PROT_WRITE,MAP_SHARED,fd,0); close(fd);
        if (shared==MAP_FAILED) {shared=NULL;return -1;}
        if (create) {
            memset(shared,0,sizeof(*shared)); model_init(&shared->model,config);
            if (BACKEND==0 && sem_init(&shared->mutex,1,1)) return -1;
        } else if (shared->magic!=MAGIC) {errno=EAGAIN;return -1;}
        if (BACKEND==0) gate=&shared->mutex;
    } else if (create) model_init(&local_model,config);
    if (BACKEND!=0) {
        gate=create?sem_open(sem_name,O_CREAT|O_EXCL,0600,1):sem_open(sem_name,0);
        if (gate==SEM_FAILED) return -1;
        sem_created=create;
    }
    if (BACKEND==3) {
        struct mq_attr attr={.mq_maxmsg=10,.mq_msgsize=sizeof(Request)};
        requests=mq_open(req_name,O_RDWR|(create?(O_CREAT|O_EXCL):0),0600,&attr);
        if (requests==(mqd_t)-1) return -1;
        req_created=create;
        attr.mq_msgsize=sizeof(Reply);
        responses=mq_open(rep_name,O_RDWR|(create?(O_CREAT|O_EXCL):0),0600,&attr);
        if (responses==(mqd_t)-1) return -1;
        rep_created=create;
    }
    if (create && shared) shared->magic=MAGIC;
    return 0;
}
static Reply transaction(Request r)
{
    if (lock_gate()) return (Reply){-3,0,0};
    Reply out;
    if (BACKEND==3) {
        if (send_message(requests,&r,sizeof(r)) || receive_message(responses,&out,sizeof(out),0)) out=(Reply){-3,0,0};
    } else {
        out=model_apply(&shared->model,r);
        if (model_check(&shared->model)) {
            event(&shared->model,"INVARIANT_FAILURE"); shared->model.stop=1; out.code=-3;
        }
    }
    sem_post(gate);
    return out;
}
static int actor(int role, int id)
{
    Request r={HELLO,role,id,(int)getpid()};
    Reply out=transaction(r);
    if (out.code<0) return 1;
    int delay=out.duration, arrived=0, job=-1;
    uint64_t arrival=now_ms()+(unsigned)delay;
    for (;;) {
        if (interrupted) {
            r.action=STOP; r.value=interrupted; transaction(r); break;
        }
        if (now_ms()>=deadline) break;
        r.value=0;
        if (role==VISITOR) {
            if (!arrived && now_ms()>=arrival) {r.action=ARRIVE;arrived=1;}
            else r.action=VISIT_STATUS;
        } else if (role==WAITER) r.action=WAITER_STEP;
        else {r.action=job<0?COOK_TAKE:COOK_FINISH;r.value=job;}
        out=transaction(r);
        if (out.code<0 || out.code==2) break;
        if (role==COOK && out.code==1) {
            if (job<0) {
                job=out.value;
                int remaining=out.duration;
                while (remaining>0 && !interrupted && now_ms()<deadline) {
                    int slice=remaining>20?20:remaining; pause_ms(slice); remaining-=slice;
                    /* Периодическая проверка остановки не изменяет выполняемую операцию. */
                    Request probe={VISIT_STATUS,role,id,0};
                    Reply state=transaction(probe);
                    if (state.code<0) break;
                }
            } else job=-1;
        }
        pause_ms(2);
    }
    r.action=BYE; transaction(r);
    return out.code==-2 || out.code==-3 || now_ms()>=deadline ? 1 : 0;
}
static int coordinator(void)
{
    Model *m=BACKEND==3?&local_model:&shared->model;
    int total=m->config.visitor_count+m->config.waiter_count+m->config.cook_count;
    uint64_t grace=0;
    for (;;) {
        if (BACKEND!=3 && lock_gate()) return 1;
        if (interrupted) {m->stop=1;m->signal_number=interrupted;}
        if (now_ms()>=deadline) {m->stop=1;if(!m->signal_number)m->signal_number=1;}
        model_tick(m);
        if (model_check(m)) {event(m,"INVARIANT_FAILURE");m->stop=1;m->signal_number=1;}
        int registered=0;
        for (int r=0;r<3;++r) for (int i=0;i<MAX_VISITORS;++i) registered+=m->registered[r][i];
        int done=(registered==total && m->active==0);
        if (m->stop && !grace) grace=now_ms()+3000;
        if (m->stop && now_ms()>grace) done=1;
        if (BACKEND!=3) sem_post(gate);
        if (done) break;
        if (BACKEND==3) {
            Request r; int result=receive_message(requests,&r,sizeof(r),1);
            if (!result) {
                Reply out=model_apply(m,r);
                if (send_message(responses,&out,sizeof(out))) break;
            } else if (result<0) {m->stop=1;break;}
        } else pause_ms(2);
    }
    model_summary(m);
    return m->signal_number ? (m->signal_number==SIGINT?130:m->signal_number==SIGTERM?143:1) : 0;
}
static void usage(const char *name)
{
    printf("Usage: %s [--config FILE] [--session NAME] [--log FILE|--no-log]\n"
        "  [--unit-ms 1..10000] [--timeout 1..3600]\n"
        "  Independent modes: --serve | --role visitor|waiter|cook --id N\n"
        "  IDs start at 1; start --serve before actors. Backend=%d\n",name,BACKEND);
}
static int cli_number(const char *s, int low, int high)
{
    char *end; errno=0;long n=strtol(s,&end,10);
    return !*s || errno || *end || n<low || n>high ? -1:(int)n;
}
int main(int argc, char **argv)
{
    const char *config_path="configs/demo.conf", *session=NULL, *log_path="restaurant.log";
    int serve=0, role=-1, id=-1, unit=0;
    for (int i=1;i<argc;++i) {
        if (!strcmp(argv[i],"--help")) {usage(argv[0]);return 0;}
        if (!strcmp(argv[i],"--serve")) {serve=1;continue;}
        if (!strcmp(argv[i],"--no-log")) {log_path=NULL;continue;}
        if (i+1>=argc) {usage(argv[0]);return 2;}
        const char *key=argv[i], *value=argv[++i];
        if (!strcmp(key,"--config")) config_path=value;
        else if (!strcmp(key,"--session")) session=value;
        else if (!strcmp(key,"--log")) log_path=value;
        else if (!strcmp(key,"--unit-ms")) {unit=cli_number(value,1,10000);if(unit<0)return 2;}
        else if (!strcmp(key,"--timeout")) {deadline_seconds=cli_number(value,1,3600);if(deadline_seconds<0)return 2;}
        else if (!strcmp(key,"--id")) {id=cli_number(value,1,MAX_VISITORS)-1;if(id<0)return 2;}
        else if (!strcmp(key,"--role")) {
            role=!strcmp(value,"visitor")?VISITOR:!strcmp(value,"waiter")?WAITER:!strcmp(value,"cook")?COOK:-1;
            if(role<0)return 2;
        } else {usage(argv[0]);return 2;}
    }
    if (BACKEND<2 && (serve || role>=0)) {usage(argv[0]);return 2;}
    if (BACKEND>=2 && ((!serve && role<0) || (serve && role>=0) || (role>=0 && id<0) || !session)) {usage(argv[0]);return 2;}
    char automatic[64];snprintf(automatic,sizeof(automatic),"%ld_%d_%ld",(long)getuid(),BACKEND,(long)getpid());
    if (!session) session=automatic;
    struct sigaction sa={0};sa.sa_handler=signal_handler;sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,&sa,NULL);sigaction(SIGTERM,&sa,NULL);signal(SIGPIPE,SIG_IGN);
    deadline=now_ms()+(unsigned)deadline_seconds*1000;
    Config config;
    int create=BACKEND<2 || serve;
    if (create && config_load_file(config_path,&config)) return 2;
    if (create && unit) config.unit_ms=unit;
    if (setup(session,create,create?&config:NULL)) fail("IPC setup (check session/owner)");
    if (log_path) {
        log_fd=open(log_path,O_WRONLY|O_CREAT|O_APPEND,0644);
        if(log_fd<0)fail("log");
    }
    if (!create) {int status=actor(role,id);cleanup();return status;}
    Model *m=BACKEND==3?&local_model:&shared->model;
    event(m,"SESSION name=%s backend=%d visitors=%d waiters=%d cooks=%d",session,BACKEND,config.visitor_count,config.waiter_count,config.cook_count);
    pid_t children[MAX_VISITORS+2*MAX_WORKERS];int child_count=0;
    if (BACKEND<2) {
        for (int r=0;r<3;++r) {
            int count=r==VISITOR?config.visitor_count:r==WAITER?config.waiter_count:config.cook_count;
            for (int i=0;i<count;++i) {
                pid_t p=fork();
                if(p<0) {interrupted=1;break;}
                if(!p) {owner=0;int status=actor(r,i);cleanup();_exit(status);}
                children[child_count++]=p;
            }
            if(interrupted)break;
        }
    }
    int status=coordinator();
    for (int i=0;i<child_count;++i) {
        int s;
        while(waitpid(children[i],&s,0)<0 && errno==EINTR) {}
        if ((!WIFEXITED(s) || WEXITSTATUS(s)) && !status) status=1;
    }
    cleanup();return status;
}
