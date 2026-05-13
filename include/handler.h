#pragma once

#include <sys/types.h>

#include "buff.h"


class ProtocolHandler {
    public:
        virtual void handle_packet(pkt_buff* pkt) = 0; // =0 means must compulsory implement handle_packet
        virtual ~ProtocolHandler() = default; // default destructor
    };