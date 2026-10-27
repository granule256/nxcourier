#include "jobs.hpp"
#include <cassert>
#include <thread>
#include <cstdio>
using Queue = nxc::jobs::Queue<4, 128, 128>;
int main() {
    Queue q;
    auto* first = q.submit(0, 1, "slow", false);
    auto* second = q.submit(1, 2, "hello", false);
    assert(first && second && q.hasClient(0, 1));
    assert(q.take() == first);
    q.cancelClient(0, 1);
    assert(first->cancelled.load());
    first->output[0] = 'x'; first->outputBytes = 1;
    q.finish(*first);
    assert(q.completeFor(0, 2) == nullptr); // 新连接不能收到旧连接的回复
    assert(q.completeFor(0, 1) == first);
    q.retire(*first);
    assert(!q.hasClient(0, 1));
    assert(q.take() == second);
    q.finish(*second); q.retire(*second);
    // 重用槽位后仍按发布顺序执行，不能让低索引的新任务插队。
    auto* a=q.submit(0,3,"a",false); auto* b=q.submit(1,3,"b",false);
    assert(q.take()==a); q.finish(*a); q.retire(*a);
    auto* c=q.submit(2,3,"c",false); assert(c);
    assert(q.take()==b); q.finish(*b); q.retire(*b);
    assert(q.take()==c); q.finish(*c); q.retire(*c);
    // 单一客户端最多占用两个槽位，其余客户端仍能接入。
    auto* one=q.submit(0,8,"one",false); auto* two=q.submit(0,8,"two",false);
    assert(one && two && !q.submit(0,8,"three",false));
    auto* other=q.submit(1,8,"other",false); assert(other);
    while(auto* j=q.take()){ q.finish(*j); q.retire(*j); }
    // 实际生产者/消费者线程验证发布、应答和槽位重用。
    std::thread worker([&]{ for(int i=0;i<1000;) { if(auto* j=q.take()) { assert(j->input[0]=='x'); j->output[0]='y'; j->outputBytes=1; q.finish(*j); ++i; } else std::this_thread::yield(); } });
    for(int i=0;i<1000;++i) {
        while(!q.submit(0,4,"x",false)) std::this_thread::yield();
        Queue::Job* j=nullptr; while(!(j=q.completeFor(0,4))) std::this_thread::yield();
        assert(j->output[0]=='y' && j->outputBytes==1); q.retire(*j);
    }
    worker.join(); puts("job queue publication, cancellation, FIFO, generations: PASS");
}
