/* 
 * A slightly obfuscated implementation of skip lists without using ordering and height counters.
 * For a better implementation, see, e.g., http://eternallyconfuzzled.com/tuts/datastructures/jsw_tut_skip.aspx
 * or http://ck.kolivas.org/patches/bfs/test/bfs406-skiplists.patch
 *
 * We assume the height to be fixed to 3 and we always have the maximum height at the head and tail
 * nodes---in other words, we do not let the height grow/shrink. Also, we do not consider a dynamic
 * number of next pointers in the nodes.
 *
 */

#include <verifier-builtins.h>
#include <stdlib.h>

#define __nondet ___sl_get_nondet_int

// a skip list node with three next pointers
struct sl_item {
    struct sl_item *n1, *n2, *n3;
};

// a skip list
struct sl {
    struct sl_item *head, *tail;
};

struct sl_item* alloc_or_die(void)
{
    struct sl_item *pi = malloc(sizeof(*pi));
    if (pi)
        return pi;
    else
        abort();
}

struct sl* create_sl_with_head_and_tail(void)
{
    struct sl *sl = malloc(sizeof(*sl));
    if (sl) {
        sl->head = alloc_or_die();
        sl->tail = alloc_or_die();
        
        sl->head->n3 = sl->head->n2 = sl->head->n1 = sl->tail;
        sl->tail->n3 = sl->tail->n2 = sl->tail->n1 = NULL;

        return sl;
    } 
    else
        abort();
}

// The function inserts one node of a random height to a randomly chosen position in between of 
// the head and tail.
void sl_random_insert(struct sl *sl)
{
    // a1, a2, a3 remember the nodes before the inserted one at the particular levels
    struct sl_item *a1, *a2, *a3;
    struct sl_item *new;

    // moving randomly on the 3rd level
    a3 = sl->head;
    while (a3->n3 != sl->tail && __nondet())
        a3 = a3->n3;

    // moving randomly on the 2nd level, not going behind a3->n3
    a2 = a3; 
    while (a2->n2 != a3->n3 && __nondet())
        a2 = a2->n2;

    // moving randomly on the 1st level, not going behind a2->n2
    a1 = a2; 
    while (a1->n1 != a2->n2 && __nondet())
        a1 = a1->n1;
        
    // allocation and insertion of a new node
    new = alloc_or_die();    
    // always insert at level 1
    new->n1 = a1->n1;
    a1->n1 = new;
    // choose whether to insert at level 2
    if (__nondet()) {
        new->n2 = a2->n2;
        a2->n2 = new;
        // choose whether to insert at level 3
        if (__nondet()) {
            new->n3 = a3->n3;
            a3->n3 = new;
        }
    }
}

void destroy_sl(struct sl *sl)
{
    struct sl_item *tmp;

    while (sl->head) {
        tmp = sl->head;
        sl->head = sl->head->n1;
        free(tmp);
    }
    free(sl);
}

int main() 
{
    struct sl *sl = create_sl_with_head_and_tail();

    ___sl_plot("00");

    sl_random_insert(sl);

    ___sl_plot("01");

    sl_random_insert(sl);

    ___sl_plot("02");

    sl_random_insert(sl);

    ___sl_plot("03");

    destroy_sl(sl);

    ___sl_plot("04");

    return 0;
}



