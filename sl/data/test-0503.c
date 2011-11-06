#include <verifier-builtins.h>
#include <stdlib.h>

#define __nondet ___sl_get_nondet_int

int main() {

    struct T {
        struct T* next;
        struct T* prev;
    };

    struct T* x = NULL;
    struct T* y = NULL;

    while (__nondet()) {
        y = malloc(sizeof(struct T));
        if (!y)
            abort();

        y->next = x;
        y->prev = NULL;
        if (x)
            x->prev = y;
        x = y;
    }

    struct T* sorted = NULL;
    struct T* pred = NULL;
    struct T* z = NULL;

    while (x) {
        y = x;
        x = x->next;
        z = sorted;
        if (x) x->prev = NULL;
        pred = NULL;

        ___sl_plot(NULL);

        while (z && __nondet()) {
            pred = z;
            z = z->next;
        }

        y->next = z;
        if (z) z->prev = y;
        y->prev = pred;		
        if (pred) pred->next = y;
        else sorted = y;
    }

    while (sorted != NULL) {
        y = sorted;
        sorted = sorted->next;
        free(y);
    }

    return 0;

}
