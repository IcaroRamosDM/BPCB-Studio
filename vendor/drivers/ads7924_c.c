#include "ads7924_c.h"
#include <errno.h>
/* Sequencia manual equivalente ao utilitario usado na bancada.
 * O chamador fornece transporte e espera; isso permite validar a sequencia
 * sem acesso USB. As quatro amostras so sao publicadas apos sucesso completo. */
int ads7924_read_all(const ads_bus *b,unsigned raw[4]) {
    if(!b||!raw||!b->write_reg||!b->read_reg||!b->wait_ms) { errno=EINVAL; return -1; }
    unsigned values[4]={0};
    if(b->write_reg(b->context,0x16,0xaa)<0 ||
       b->write_reg(b->context,0x00,0x80)<0 ||
       b->write_reg(b->context,0x14,31)<0 ||
       b->write_reg(b->context,0x00,0x80)<0 ||
       b->wait_ms(b->context,10)<0 ||
       b->write_reg(b->context,0x00,0xc0)<0) return -1;
    for(unsigned ch=0;ch<4;ch++) {
        uint8_t selected=0,bytes[2]={0};
        if(b->write_reg(b->context,0x00,(uint8_t)(0xc0u|ch))<0 ||
           b->wait_ms(b->context,10)<0 ||
           b->read_reg(b->context,0x00,&selected,1)<0) return -1;
        if((selected&3u)!=ch) { errno=EIO; return -1; }
        if(b->read_reg(b->context,(uint8_t)(0x82u+ch*2u),bytes,2)<0) return -1;
        values[ch]=((unsigned)bytes[0]<<4)|((unsigned)bytes[1]>>4);
    }
    if(b->write_reg(b->context,0x00,0x83)<0 ||
       b->wait_ms(b->context,10)<0 ||
       b->write_reg(b->context,0x00,0x03)<0) return -1;
    for(unsigned ch=0;ch<4;ch++) raw[ch]=values[ch];
    return 0;
}
