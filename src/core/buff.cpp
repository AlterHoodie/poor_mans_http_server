#include <cstdint>
#include <cstdlib>

#include "buff.h"

uint8_t *push_pointer(uint8_t *ptr, int len){
    return ptr+len;
}

pkt_buff* create_buffer(){
    pkt_buff *buff = (pkt_buff*)malloc(sizeof(pkt_buff));
    uint8_t *mem = (uint8_t *)malloc(PKT_ALLOCATION_SIZE);

    buff->head = mem;
    buff->data = push_pointer(mem, 128);
    buff->tail = buff->data;
    buff->end = push_pointer(mem, PKT_ALLOCATION_SIZE);

    return buff;
}

void delete_buffer(pkt_buff *buff){
    if(buff){
        free(buff->head);
        free(buff);
    }
}

bool pull(pkt_buff *b, std::size_t len){
    if (len > b->len()){
        return false;
    }
    b->data += len;
    return true;
}

bool push(pkt_buff *b, std::size_t len){
    if (len > b->headroom()){
        return false;
    }
    b->data-=len;
    return true;
}