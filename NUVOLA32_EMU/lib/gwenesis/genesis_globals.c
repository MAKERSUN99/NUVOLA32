#include <stdint.h>
#include "controller.h"

int sn76489_index = 0;
int sn76489_clock = 0;

uint16_t gwenesis_io_get_buttons(void)
{
    return 0xFFFF;
    //genesis_controller_poll();
}