#include "model.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <string.h>
static void prepare(Model *m, int lifo)
{
    Config c={0};
    c.visitor_count=2;c.waiter_count=2;c.cook_count=2;c.menu_count=1;c.unit_ms=1;
    c.kitchen_cap=2;c.ready_cap=1;c.lifo=lifo;c.equipment[0]=1;
    strcpy(c.menu[0].name,"Soup");c.menu[0].count=2;
    c.menu[0].steps[0]=(OperationSpec){0,1};c.menu[0].steps[1]=(OperationSpec){0,1};
    c.dish_count[0]=c.dish_count[1]=1;
    model_init(m,&c);
    for(int i=0;i<2;++i) {
        assert(model_apply(m,(Request){HELLO,VISITOR,i,0}).code==0);
        model_apply(m,(Request){HELLO,WAITER,i,0});model_apply(m,(Request){HELLO,COOK,i,0});
        model_apply(m,(Request){ARRIVE,VISITOR,i,0});
        model_apply(m,(Request){WAITER_STEP,WAITER,i,0});
    }
    model_tick(m);assert(model_check(m)==0);
}
int main(void)
{
    Model m;
    for(int lifo=0;lifo<2;++lifo) {
        prepare(&m,lifo);
        Reply r=model_apply(&m,(Request){COOK_TAKE,COOK,0,0});
        assert(r.code==1 && m.jobs[r.value].order==(lifo?1:0));
        assert(model_apply(&m,(Request){COOK_TAKE,COOK,0,0}).code==-2);
        assert(model_apply(&m,(Request){COOK_TAKE,COOK,1,0}).code==0);
        assert(model_apply(&m,(Request){COOK_FINISH,COOK,1,r.value}).code==-2);
        assert(model_apply(&m,(Request){COOK_FINISH,COOK,0,r.value}).code==1);
        assert(m.jobs[r.value].step==1 && m.jobs[r.value].state==1);
        assert(model_check(&m)==0);
        ++m.queued;assert(model_check(&m)==-1);
    }
    return 0;
}
