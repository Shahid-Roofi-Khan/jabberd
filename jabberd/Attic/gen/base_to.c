#include "jabberd.h"

result base_to_config(instance id, xmlnode x, void *arg)
{
    if(id == NULL)
    {
        printf("base_to_config validating configuration\n");
        return r_PASS;
    }

    printf("base_to_config performing configuration %s\n",xmlnode2str(x));
}

void base_to(void)
{
    printf("base_to loading...\n");

    register_config("to",base_to_config,NULL);
}