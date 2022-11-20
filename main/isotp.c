#include <stdint.h>
#include "assert.h"
#include "isotp.h"

///////////////////////////////////////////////////////
///                 STATIC FUNCTIONS                ///
///////////////////////////////////////////////////////

static uint32_t stmin_to_us(uint32_t ms)
{
	if(ms <= 0x7F) {
		return ms * 1000;
	} else if(ms >= 0xF1 && ms <= 0xF9) {
		return (ms - 0xF0) * 100;
	}

	return 0;
}

static uint32_t us_to_stmin(uint32_t us)
{
	if(us <= 127000) {
		return us / 1000;
	} else if(us >= 100 && us <= 900) {
		return 0xF0 + (us / 100);
	}

	return 0;
}

static int isotp_send_flow_control(IsoTpLink* link, uint16_t flow_status, uint16_t block_size, uint16_t st_min_us) {
	isotp_user_debug("isotp_send_flow_control\n");
	IsoTpCanMessage message;
    int ret;
    /* setup message  */
    message.as.flow_control.type = ISOTP_PCI_TYPE_FLOW_CONTROL_FRAME;
	message.as.flow_control.FS = flow_status;
	message.as.flow_control.BS = block_size;
	message.as.flow_control.STmin = us_to_stmin(st_min_us);
	/* send message */
#ifdef ISO_TP_FRAME_PADDING
    (void) memset(message.as.flow_control.reserve, 0xAA, sizeof(message.as.flow_control.reserve));
    ret = isotp_user_send_can(link->send_arbitration_id, message.as.data_array.ptr, sizeof(message));
#else    
    ret = isotp_user_send_can(link->send_arbitration_id, message.as.data_array.ptr, 3);
#endif
    return ret;
}

static int isotp_send_single_frame(IsoTpLink* link, uint32_t id) {
    isotp_user_debug("isotp_send_single_frame\n");
    IsoTpCanMessage message;
    int ret;

    /* multi frame message length must greater than 7  */
    assert(link->send_size <= 7);

    /* setup message  */
    message.as.single_frame.type = ISOTP_PCI_TYPE_SINGLE;
	message.as.single_frame.SF_DL = link->send_size;
    (void) memcpy(message.as.single_frame.data, link->send_buffer, link->send_size);

    /* send message */
#ifdef ISO_TP_FRAME_PADDING
    (void) memset(message.as.single_frame.data + link->send_size, 0xAA, sizeof(message.as.single_frame.data) - link->send_size);
    ret = isotp_user_send_can(id, message.as.data_array.ptr, sizeof(message));
#else
    ret = isotp_user_send_can(id, message.as.data_array.ptr,link->send_size + 1);
#endif

    return ret;
}

static int isotp_send_first_frame(IsoTpLink* link, uint32_t id) {
    isotp_user_debug("isotp_send_first_frame\n");
    IsoTpCanMessage message;
    int ret;

    /* multi frame message length must greater than 7  */
    assert(link->send_size > 7);

    /* setup message  */
    message.as.first_frame.type = ISOTP_PCI_TYPE_FIRST_FRAME;
	message.as.first_frame.FF_DL_low = link->send_size;
	message.as.first_frame.FF_DL_high = 0x0F & (link->send_size >> 8);
    (void) memcpy(message.as.first_frame.data, link->send_buffer, sizeof(message.as.first_frame.data));

    /* send message */
    ret = isotp_user_send_can(id, message.as.data_array.ptr, sizeof(message));
	if (ISOTP_RET_OK == ret) {
        link->send_offset += sizeof(message.as.first_frame.data);
        link->send_sn = 1;
    }

    return ret;
}

static int isotp_send_consecutive_frame(IsoTpLink* link) {
    isotp_user_debug("isotp_send_consecutive_frame\n");
    IsoTpCanMessage message;
    uint16_t data_length;
    int ret;

    /* multi frame message length must greater than 7  */
    assert(link->send_size > 7);

    /* setup message  */
    message.as.consecutive_frame.type = ISOTP_PCI_TYPE_CONSECUTIVE_FRAME;
    message.as.consecutive_frame.SN = link->send_sn;
    data_length = link->send_size - link->send_offset;
    if (data_length > sizeof(message.as.consecutive_frame.data)) {
        data_length = sizeof(message.as.consecutive_frame.data);
    }
    (void) memcpy(message.as.consecutive_frame.data, link->send_buffer + link->send_offset, data_length);

    /* send message */
#ifdef ISO_TP_FRAME_PADDING
    (void) memset(message.as.consecutive_frame.data + data_length, 0xAA, sizeof(message.as.consecutive_frame.data) - data_length);
    ret = isotp_user_send_can(link->send_arbitration_id, message.as.data_array.ptr, sizeof(message));
#else
    ret = isotp_user_send_can(link->send_arbitration_id,
            message.as.data_array.ptr,
            data_length + 1);
#endif
    if (ISOTP_RET_OK == ret) {
        link->send_offset += data_length;
        if (++(link->send_sn) > 0x0F) {
            link->send_sn = 0;
        }
    }
    
    return ret;
}

static int isotp_receive_single_frame(IsoTpLink *link, IsoTpCanMessage *message, uint16_t len) {
    isotp_user_debug("isotp_receive_single_frame\n");
    /* check data length */
    if ((0 == message->as.single_frame.SF_DL) || (message->as.single_frame.SF_DL > (len - 1))) {
        isotp_user_debug("isotp_receive_single_frame: Single-frame length too small.\n");
        return ISOTP_RET_LENGTH;
    }

    /* copying data */
    (void) memcpy(link->receive_buffer, message->as.single_frame.data, message->as.single_frame.SF_DL);
    link->receive_size = message->as.single_frame.SF_DL;
    
    return ISOTP_RET_OK;
}

static int isotp_receive_first_frame(IsoTpLink *link, IsoTpCanMessage *message, uint16_t len) {
    isotp_user_debug("isotp_receive_first_frame\n");
    uint16_t payload_length;

    if (8 != len) {
        isotp_user_debug("isotp_receive_first_frame: First frame should be 8 bytes in length.\n");
        return ISOTP_RET_LENGTH;
    }

    /* check data length */
    payload_length = message->as.first_frame.FF_DL_high;
    payload_length = (payload_length << 8) + message->as.first_frame.FF_DL_low;

    /* should not use multiple frame transmition */
    if (payload_length <= 7) {
        isotp_user_debug("isotp_receive_first_frame: Should not use multiple frame transmission.\n");
        return ISOTP_RET_LENGTH;
    }
    
    if (payload_length > link->receive_buf_size) {
        isotp_user_debug("isotp_receive_first_frame: Multi-frame response too large for receiving buffer.\n");
        return ISOTP_RET_OVERFLOW;
    }
    
    /* copying data */
    (void) memcpy(link->receive_buffer, message->as.first_frame.data, sizeof(message->as.first_frame.data));
    link->receive_size = payload_length;
    link->receive_offset = sizeof(message->as.first_frame.data);
    link->receive_sn = 1;

    return ISOTP_RET_OK;
}

static int isotp_receive_consecutive_frame(IsoTpLink *link, IsoTpCanMessage *message, uint16_t len) {
    isotp_user_debug("isotp_receive_consecutive_frame\n");
    uint16_t remaining_bytes;
    
    /* check sn */
    if (link->receive_sn != message->as.consecutive_frame.SN) {
        return ISOTP_RET_WRONG_SN;
    }

    /* check data length */
    remaining_bytes = link->receive_size - link->receive_offset;
    if (remaining_bytes > sizeof(message->as.consecutive_frame.data)) {
        remaining_bytes = sizeof(message->as.consecutive_frame.data);
    }
    if (remaining_bytes > len - 1) {
        isotp_user_debug("isotp_receive_consecutive_frame: Consecutive frame too short.\n");
        return ISOTP_RET_LENGTH;
    }

    /* copying data */
    (void) memcpy(link->receive_buffer + link->receive_offset, message->as.consecutive_frame.data, remaining_bytes);

    link->receive_offset += remaining_bytes;
    if (++(link->receive_sn) > 0x0F) {
        link->receive_sn = 0;
    }

    return ISOTP_RET_OK;
}

static int isotp_receive_flow_control_frame(IsoTpLink *link, IsoTpCanMessage *message, uint16_t len) {
    isotp_user_debug("isotp_receive_flow_control_frame\n");
    /* check message length */
    if (len < 3) {
        isotp_user_debug("isotp_receive_flow_control_frame: Flow control frame too short.\n");
        return ISOTP_RET_LENGTH;
    }

    return ISOTP_RET_OK;
}

///////////////////////////////////////////////////////
///                 PUBLIC FUNCTIONS                ///
///////////////////////////////////////////////////////

int isotp_send(IsoTpLink *link, const uint8_t payload[], uint16_t size) {
    return isotp_send_with_id(link, link->send_arbitration_id, payload, size);
}

int isotp_send_with_id(IsoTpLink *link, uint32_t id, const uint8_t payload[], uint16_t size) {
    int ret;

    if (link == 0x0) {
        isotp_user_debug("isotp_send_with_id: Link is null!\n");
        return ISOTP_RET_ERROR;
    }

    if (link->send_arbitration_id == 0) {
        isotp_user_debug("isotp_send_with_id: send_arbitration_id is 0!\n");
        return ISOTP_RET_ERROR;
    }

    if (size > link->send_buf_size) {
        isotp_user_debug("isotp_send_with_id: Message size too large. Increase ISO_TP_MAX_MESSAGE_SIZE to set a larger buffer\n\n");
        return ISOTP_RET_OVERFLOW;
    }

	if (ISOTP_SEND_STATUS_INPROGRESS == link->send_status) {
        isotp_user_debug("isotp_send_with_id: Abort previous message, transmission in progress.\n\n");
        return ISOTP_RET_INPROGRESS;
    }

    /* copy into local buffer */
    link->send_size = size;
    link->send_offset = 0;
    (void) memcpy(link->send_buffer, payload, size);

    if (link->send_size < 8) {
        /* send single frame */
        ret = isotp_send_single_frame(link, id);
    } else {
        /* send multi-frame */
        ret = isotp_send_first_frame(link, id);

        /* init multi-frame control flags */
        if (ISOTP_RET_OK == ret) {
            uint64_t isotp_time = isotp_user_get_us();

            link->send_bs_remain = 0;
			link->send_st_min = 0;
            link->send_wtf_count = 0;
			link->send_timer_st = isotp_time;
			link->send_timer_bs = isotp_time + ISO_TP_DEFAULT_RESPONSE_TIMEOUT;
            link->send_protocol_result = ISOTP_PROTOCOL_RESULT_OK;
			link->send_status = ISOTP_SEND_STATUS_INPROGRESS;
        }
    }

    return ret;
}

void isotp_on_can_message(IsoTpLink *link, uint8_t *data, uint16_t len) {
    isotp_user_debug("isotp_on_can_message len = %d link->receive_status = %d\n", len, link->receive_status);
	for(uint16_t i = 0; i < len; i++) {
        isotp_user_debug("isotp_on_can_message [%02x] = %02x\n", i, data[i]);
    }

	IsoTpCanMessage message;
    int ret;
    
    if (len < 2 || len > 8) {
        return;
    }

    memcpy(message.as.data_array.ptr, data, len);
    memset(message.as.data_array.ptr + len, 0, sizeof(message.as.data_array.ptr) - len);

    switch (message.as.common.type) {
        case ISOTP_PCI_TYPE_SINGLE: {
            isotp_user_debug("isotp_on_can_message: ISOTP_PCI_TYPE_SINGLE\n");
            /* update protocol result */
            if (ISOTP_RECEIVE_STATUS_INPROGRESS == link->receive_status) {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_UNEXP_PDU;
            } else {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_OK;
            }

            /* handle message */
            ret = isotp_receive_single_frame(link, &message, len);
            
            if (ISOTP_RET_OK == ret) {
                /* change status */
                link->receive_status = ISOTP_RECEIVE_STATUS_FULL;
            }
            break;
        }
        case ISOTP_PCI_TYPE_FIRST_FRAME: {
            isotp_user_debug("isotp_on_can_message: ISOTP_PCI_TYPE_FIRST_FRAME\n");
            /* update protocol result */
            if (ISOTP_RECEIVE_STATUS_INPROGRESS == link->receive_status) {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_UNEXP_PDU;
            } else {
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_OK;
            }

            /* handle message */
            ret = isotp_receive_first_frame(link, &message, len);

            /* if overflow happened */
            if (ISOTP_RET_OVERFLOW == ret) {
                /* update protocol result */
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_BUFFER_OVFLW;
                /* change status */
                link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
                /* send error message */
				isotp_send_flow_control(link, PCI_FLOW_STATUS_OVERFLOW, 0, 0);
                break;
            }

            /* if receive successful */
            if (ISOTP_RET_OK == ret) {
                /* change status */
                link->receive_status = ISOTP_RECEIVE_STATUS_INPROGRESS;
                /* send fc frame */
                link->receive_bs_count = link->default_block_size;
				isotp_send_flow_control(link, PCI_FLOW_STATUS_CONTINUE, link->receive_bs_count, link->st_min);
                /* refresh timer cs */
				link->receive_timer_cr = isotp_user_get_us() + ISO_TP_DEFAULT_RESPONSE_TIMEOUT;
            }
            
            break;
        }
        case ISOTP_PCI_TYPE_CONSECUTIVE_FRAME: {
            isotp_user_debug("isotp_on_can_message: ISOTP_PCI_TYPE_CONSECUTIVE_FRAME\n");
            /* check if in receiving status */
            if (ISOTP_RECEIVE_STATUS_INPROGRESS != link->receive_status) {
                isotp_user_debug("isotp_on_can_message: ISOTP_PCI_TYPE_CONSECUTIVE_FRAME ISOTP_PROTOCOL_RESULT_UNEXP_PDU\n");
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_UNEXP_PDU;
                break;
            }

            /* handle message */
            ret = isotp_receive_consecutive_frame(link, &message, len);

            /* if wrong sn */
            if (ISOTP_RET_WRONG_SN == ret) {
                isotp_user_debug("isotp_on_can_message: ISOTP_PCI_TYPE_CONSECUTIVE_FRAME ISOTP_RET_WRONG_SN\n");
                link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_WRONG_SN;
                link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
                break;
            }

            /* if success */
			if (ISOTP_RET_OK == ret) {
                isotp_user_debug("isotp_on_can_message: ISOTP_PCI_TYPE_CONSECUTIVE_FRAME ISOTP_RET_OK\n");
                /* refresh timer cs */
				link->receive_timer_cr = isotp_user_get_us() + ISO_TP_DEFAULT_RESPONSE_TIMEOUT;
                
                /* receive finished */
                if (link->receive_offset >= link->receive_size) {
                    link->receive_status = ISOTP_RECEIVE_STATUS_FULL;
                } else {
                    /* send fc when bs reaches limit */
                    if (0 == --link->receive_bs_count) {
                        link->receive_bs_count = link->default_block_size;
						isotp_send_flow_control(link, PCI_FLOW_STATUS_CONTINUE, link->receive_bs_count, link->st_min);
                    }
                }
            }
            
            break;
        }
        case ISOTP_PCI_TYPE_FLOW_CONTROL_FRAME:
            isotp_user_debug("isotp_on_can_message: ISOTP_PCI_TYPE_FLOW_CONTROL_FRAME\n");
            /* handle fc frame only when sending in progress  */
            if (ISOTP_SEND_STATUS_INPROGRESS != link->send_status) {
                break;
            }

            /* handle message */
            ret = isotp_receive_flow_control_frame(link, &message, len);
            
            if (ISOTP_RET_OK == ret) {
                uint64_t isotp_time = isotp_user_get_us();

                /* refresh bs timer */
				link->send_timer_bs = isotp_time + ISO_TP_DEFAULT_RESPONSE_TIMEOUT;

                /* overflow */
                if (PCI_FLOW_STATUS_OVERFLOW == message.as.flow_control.FS) {
                    link->send_protocol_result = ISOTP_PROTOCOL_RESULT_BUFFER_OVFLW;
                    link->send_status = ISOTP_SEND_STATUS_ERROR;
                }

                /* wait */
                else if (PCI_FLOW_STATUS_WAIT == message.as.flow_control.FS) {
                    link->send_wtf_count += 1;
                    /* wait exceed allowed count */
                    if (link->send_wtf_count > ISO_TP_MAX_WFT_NUMBER) {
                        link->send_protocol_result  = ISOTP_PROTOCOL_RESULT_WFT_OVRN;
                        link->send_status           = ISOTP_SEND_STATUS_ERROR;
                    }
                }

                /* permit send */
                else if (PCI_FLOW_STATUS_CONTINUE == message.as.flow_control.FS) {
                    if (0 == message.as.flow_control.BS) {
                        link->send_bs_remain = ISOTP_INVALID_BS;
                    } else {
						link->send_bs_remain = message.as.flow_control.BS;
                    }
					link->send_st_min       = link->stmin_override ? link->stmin_override : stmin_to_us(message.as.flow_control.STmin);
                    link->send_timer_st     = isotp_time + link->send_st_min;
                    link->send_wtf_count    = 0;
                }
            }
            break;
        default:
            break;
    };
    
    return;
}

int isotp_receive(IsoTpLink *link, uint8_t *payload, const uint16_t payload_size, uint16_t *out_size) {
    isotp_user_debug("isotp_receive\n");
    uint16_t copylen;
    
    if (ISOTP_RECEIVE_STATUS_FULL != link->receive_status) {
        return ISOTP_RET_NO_DATA;
    }

    copylen = link->receive_size;
    if (copylen > payload_size) {
        isotp_user_debug("isotp_receive: warning! copylen > payload_size\n");
        copylen = payload_size;
    }

    memcpy(payload, link->receive_buffer, copylen);
    *out_size = copylen;

    link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;

    return ISOTP_RET_OK;
}

void isotp_init_link(IsoTpLink *link, uint32_t send_arbitration_id, uint32_t receive_arbitration_id, uint8_t *sendbuf, uint16_t sendbufsize, uint8_t *recvbuf, uint16_t recvbufsize) {
	isotp_user_debug("isotp_init_link: send_arbitration_id = %08x receive_arbitration_id = %08x\n", send_arbitration_id, receive_arbitration_id);
    memset(link, 0, sizeof(*link));
    link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
    link->send_status = ISOTP_SEND_STATUS_IDLE;
    link->send_arbitration_id = send_arbitration_id;
    link->send_buffer = sendbuf;
    link->send_buf_size = sendbufsize;
    link->receive_arbitration_id = receive_arbitration_id;
    link->receive_buffer = recvbuf;
    link->receive_buf_size = recvbufsize;
	link->st_min = 0; // TODO: support changing this
	link->default_block_size = 0x20; // TODO: support changing this
    return;
}

void isotp_poll(IsoTpLink *link) {
    isotp_user_debug("isotp_poll\n");
    int ret;

    /* only polling when operation in progress */
    if (ISOTP_SEND_STATUS_INPROGRESS == link->send_status) {
        uint64_t isotp_time = isotp_user_get_us();
        /* continue send data */
        if (/* send data if bs_remain is invalid or bs_remain large than zero */
            (ISOTP_INVALID_BS == link->send_bs_remain || link->send_bs_remain > 0) &&
		    /* and if st_min is zero or go beyond interval time */
		    (0 == link->send_st_min || (0 != link->send_st_min && isotp_time > link->send_timer_st))) {

            ret = isotp_send_consecutive_frame(link);
            if (ISOTP_RET_OK == ret) {
                if (ISOTP_INVALID_BS != link->send_bs_remain) {
                    link->send_bs_remain -= 1;
                }

                isotp_time = isotp_user_get_us();
				link->send_timer_bs = isotp_time + ISO_TP_DEFAULT_RESPONSE_TIMEOUT;
				link->send_timer_st = isotp_time + link->send_st_min;

                /* check if send finish */
                if (link->send_offset >= link->send_size) {
                    link->send_status = ISOTP_SEND_STATUS_IDLE;
                }
            } else {
                link->send_status = ISOTP_SEND_STATUS_ERROR;
            }
        }

        /* check timeout */
		if(isotp_time > link->send_timer_bs) {
            link->send_protocol_result = ISOTP_PROTOCOL_RESULT_TIMEOUT_BS;
            link->send_status = ISOTP_SEND_STATUS_ERROR;
        }
    }

    /* only polling when operation in progress */
    if (ISOTP_RECEIVE_STATUS_INPROGRESS == link->receive_status) {
        
        /* check timeout */
		if(isotp_user_get_us() > link->receive_timer_cr) {
            isotp_user_debug("isotp_poll: ISOTP_PROTOCOL_RESULT_TIMEOUT_CR\n");
            link->receive_protocol_result = ISOTP_PROTOCOL_RESULT_TIMEOUT_CR;
            link->receive_status = ISOTP_RECEIVE_STATUS_IDLE;
        }
    }
}

