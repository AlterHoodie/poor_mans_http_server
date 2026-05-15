#include <cstdint>
#include <cstdlib>

#include "buff.h"

uint8_t *push_pointer(uint8_t *ptr, int len){
    return ptr+len;
}

void delete_buffer(pkt_buff *buff){
    if(buff){
        if (buff->head) free(buff->head);
        free(buff);
    }
}

pkt_buff_ptr create_buffer(){

    // TODO: Change this pkt buff pool arenas, 
    pkt_buff *buff = static_cast<pkt_buff*>(malloc(sizeof(pkt_buff)));
    if (!buff) return pkt_buff_ptr(nullptr,delete_buffer);

    uint8_t *mem = static_cast<uint8_t*>(malloc(PKT_ALLOCATION_SIZE));
    if (!mem) {
        free(buff);
        return pkt_buff_ptr(nullptr,delete_buffer);
    }

    buff->head = mem;
    buff->data = push_pointer(mem, 128);
    buff->tail = buff->data;
    buff->end = push_pointer(mem, PKT_ALLOCATION_SIZE);

    return pkt_buff_ptr(buff, delete_buffer);
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