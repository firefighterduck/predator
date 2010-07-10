// Creating a DLL and destroying it forwards, then creating another one and deleting it backwards.

#include "../sl.h"
#include <stdlib.h>

struct item {
    struct item *next;
    struct item *prev;
};

struct item* alloc_or_die(void)
{
    struct item *pi = malloc(sizeof(*pi));
    if (pi)
        return pi;
    else
        abort();
}

struct item* alloc_and_zero(void)
{
    struct item *pi = alloc_or_die();
    pi->next = NULL;
    pi->prev = NULL;

    return pi;
}

struct item* create_item(struct item *end)
{
    struct item *pi = alloc_and_zero();
    pi->prev = end;
    end->next = pi;
    return pi;
}

struct item* create_dll(void)
{
    struct item *dll = alloc_and_zero();
    struct item *pi = create_item(dll);
    pi = create_item(pi);
    pi = create_item(pi);
    pi = create_item(pi);
    pi = create_item(pi);
    pi = create_item(pi);
    return dll;
}

struct item* fast_forward_core(struct item *dll)
{
    struct item *next;
    while ((next = dll->next)) {
        dll = next;
    }

    return dll;
}

void fast_forward(struct item **pDll)
{
    *pDll = fast_forward_core(*pDll);
}

void destroy_from_beg(struct item *dll)
{
    ___SL_PLOT_STACK_FRAME(destroy_from_beg, "f00");
    while (dll) {
        ___SL_PLOT_STACK_FRAME(destroy_from_beg, "f01");
        struct item *next = dll->next;
        ___SL_PLOT_STACK_FRAME(destroy_from_beg, "f02");
        free(dll);
        ___SL_PLOT_STACK_FRAME(destroy_from_beg, "f03");
        dll = next;
        ___SL_PLOT_STACK_FRAME(destroy_from_beg, "f04");
    }
    ___SL_PLOT_STACK_FRAME(destroy_from_beg, "f05");
}

void destroy_from_end(struct item *dll)
{
    ___SL_PLOT_STACK_FRAME(destroy_from_end, "r00");
    while (dll) {
        ___SL_PLOT_STACK_FRAME(destroy_from_end, "r01");
        struct item *prev = dll->prev;
        ___SL_PLOT_STACK_FRAME(destroy_from_end, "r02");
        free(dll);
        ___SL_PLOT_STACK_FRAME(destroy_from_end, "r03");
        dll = prev;
        ___SL_PLOT_STACK_FRAME(destroy_from_end, "r04");
    }
    ___SL_PLOT_STACK_FRAME(destroy_from_end, "r05");
}

int main()
{
    // create a DLL
    struct item *dll = create_dll();

    // destroy the list, starting from the "begin"
    destroy_from_beg(dll);

    // acquire a fresh instance of DLL
    dll = create_dll();

    // jump to the "end"
    fast_forward(&dll);

    // destroy the list, starting from the "end"
    destroy_from_end(dll);

    return 0;
}
