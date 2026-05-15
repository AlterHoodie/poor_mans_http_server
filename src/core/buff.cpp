#include <cstdint>
#include <cstdlib>
#include <rte_mbuf.h>
#include <rte_mbuf_core.h>
#include <stdexcept>

#include "buff.h"

static rte_mempool* g_pool = nullptr;


uint8_t *push_pointer(uint8_t *ptr, int len){
    return ptr+len;
}

void delete_buffer(pkt_buff *buff){
    if(buff){
        if (buff->native_handle) rte_pktmbuf_free(buff->native_handle);
        delete buff;
    }
}

void buff_pool_init(rte_mempool* pool) {
    g_pool = pool;
}

pkt_buff_ptr create_buffer(){
    if (!g_pool) throw std::runtime_error("DPDK Memory Pool not initialized");

    rte_mbuf* m_buf = rte_pktmbuf_alloc(g_pool);
    if (!m_buf) return {nullptr, delete_buffer};

    pkt_buff *buff = new pkt_buff{};
    if (!buff) return pkt_buff_ptr(nullptr,delete_buffer);

    uint8_t* base = static_cast<uint8_t*>(m_buf->buf_addr);
    buff->head = base;
    buff->data = rte_pktmbuf_mtod(m_buf, uint8_t*);
    buff->tail = buff->data + m_buf->data_len;
    buff->end = buff->data + m_buf->buf_len;
    buff->native_handle = m_buf;

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