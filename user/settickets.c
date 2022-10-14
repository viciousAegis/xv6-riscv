#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[])
{
    int tickets;
    if (argc != 2)
    {
        fprintf(2, "usage: settickets number\n");
        exit(1);
    }

    tickets = atoi(argv[1]);

    settickets(tickets);
    exit(0);  
}
