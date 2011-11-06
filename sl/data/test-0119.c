#include <verifier-builtins.h>
#include <stdlib.h>
#define NEW(type) (type *) malloc(sizeof(type))

struct item {
    struct item *head;
    struct item *next;
};

// create SLL of length 3 with head pointers
static struct item* create_sll(void)
{
    struct item *head       = NEW(struct item);
    head->head              = head;
    head->next              = NEW(struct item);
    head->next->head        = head;
    head->next->next        = NEW(struct item);
    head->next->next->head  = head;
    head->next->next->next  = NULL;
    return head;
}

// insert one node right after head and return its address
static struct item* create_longer_sll(void)
{
    struct item *head       = create_sll();
    struct item *next       = head->next;
    head->next              = NEW(struct item);
    head->next->head        = head;
    head->next->next        = next;
    return head->next;
}

// destroy head
int main(void)
{
    struct item *sll = create_longer_sll();
    free(sll->head);
    ___sl_plot(NULL);
    return 0;
}
