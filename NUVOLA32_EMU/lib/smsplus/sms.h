
#ifndef _SMS_H_
#define _SMS_H_

#define TYPE_OVERSEAS   (0)
#define TYPE_DOMESTIC   (1)

#include <stdint.h>

/* SMS context */
typedef struct
{
    uint8_t *dummy; //JMD: Point this into outher space plz.
    uint8_t ram[0x2000];
//    uint8 sram[0x8000];
    uint8_t *sram;
    uint8_t fcr[4];
    uint8_t paused;
    uint8_t save;
    uint8_t country;
    uint8_t port_3F;
    uint8_t port_F2;
    uint8_t use_fm;
    uint8_t irq;
    uint8_t psg_mask;
}t_sms;

/* Global data */
extern t_sms sms;

/* Function prototypes */
void sms_frame(int skip_render);
void sms_init(void);
void sms_reset(void);
int  sms_irq_callback(int param);
void sms_mapper_w(int address, int data);
void cpu_reset(void);

#endif /* _SMS_H_ */
