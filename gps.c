#include "gps.h"

static uint8_t	GPS_FIFO[GPS_FIFOSIZE];

static struct   i2c_m_sync_desc	gps_i2c_desc;
static struct   _i2c_m_msg		gps_i2c_msg;
static          UBXMsg          ubx_msg;
static          UBXMsgBuffer    ubx_buf;
static          UBXMsgBuffer    ack_buf;


uint8_t gps_init_i2c(struct i2c_m_sync_desc* const I2C_DESC) 
{
	gps_i2c_desc = *I2C_DESC;
    /* enable and set up the i2c device     */
    if (i2c_m_sync_enable(&gps_i2c_desc)) {
        return GPS_FAILURE;
    }
	i2c_m_sync_set_slaveaddr(&gps_i2c_desc, M8Q_SLAVE_ADDR, I2C_M_SEVEN);
    return GPS_SUCCESS;
}

GPS_ERROR gps_checkconfig() // TODO uncomment after fixing and adding support for saved configurations
{
    GPS_ERROR result = GPS_FAILURE;
//     result = gps_verifyprt();
// 
//     /* if the i2c port is not responsive attempt to configure */
//     if (GPS_FAILURE == result && GPS_SUCCESS == gps_cfgprts()) {
//         result = gps_verifyprt();
//     }
//     gps_readfifo();
//     /* if the ports are not configured load the saved values and check */
//     if (GPS_INVALID == result && GPS_SUCCESS == gps_loadcfg(0xFFFF)) {
//         result = gps_verifyprt();
//     }
    
    /* will return failure on serial error, success, or invalid */
    return result;
}

GPS_ERROR gps_reconfig()
{
    /* do not bother checking error as the buffer will be populated by unwanted messages */
    /* disable NMEA message spam (6 messages) */
    if (GPS_SUCCESS != gps_disable_nmea()) {
        return GPS_FAILURE;
    }
    /* enable NAV message as only message */
    if (GPS_SUCCESS != gps_init_msgs()) {
        return GPS_FAILURE;
    }
    /* set satellites to GPS and no GLONASS */
    if (GPS_SUCCESS != gps_init_channels()) {
        return GPS_FAILURE;
    }
    /* configure the serial ports to i2c only */
    if (GPS_SUCCESS != gps_cfgprts()) {
        return GPS_FAILURE;
    }

    /* clear the fifo of any ACK messages */
    gps_readfifo();
    
    /* check that the ports were configured */
    if (GPS_FAILURE == gps_verifyprt()) {
        return GPS_FAILURE; // TODO - this is the only one that can currently fail
    }        

    return GPS_SUCCESS;
}

GPS_ERROR gps_cfgprts()
{
    UBXMsg      *msg;
    GPS_ERROR   result  = GPS_FAILURE;
    uint16_t    timeout = 0;
        
    /* utilizes UBX-Protocol format */
    int payloadSize = sizeof(UBXCFG_PRT) * 3;
    ubx_buf = createBuffer(payloadSize);
    msg = (UBXMsg*)ubx_buf.data;
    initMsg(msg, payloadSize, UBXMsgClassCFG, UBXMsgIdCFG_PRT);
    /* disable other ports (Default: UART,USB,DDC enabled; SPI disabled) */
    msg->payload.CFG_PRT.portID			            = UBXPRTUART;
    msg->payload.CFG_PRT.reserved0                  = 0;
    msg->payload.CFG_PRT.txReady.en				    = 0;
    msg->payload.CFG_PRT.txReady.pin			    = 8;
    msg->payload.CFG_PRT.txReady.thres			    = 0;
    msg->payload.CFG_PRT.txReady.pol			    = 0;
    msg->payload.CFG_PRT.mode.UBX_UART.charLen      = UBXPRTMode8BitCharLen;
    msg->payload.CFG_PRT.mode.UBX_UART.nStopBits    = UBXPRTMode1StopBit;
    msg->payload.CFG_PRT.mode.UBX_UART.parity       = UBXPRTModeOddParity;
    msg->payload.CFG_PRT.mode.UBX_UART.reserved0    = 0;
    msg->payload.CFG_PRT.inProtoMask		        = 0U;
    msg->payload.CFG_PRT.outProtoMask	            = 0U;
    msg->payload.CFG_PRT.option.UARTbaudRate        = 0x2580;
    
    msg = (UBXMsg*)&ubx_buf.data[20];
    msg->payload.CFG_PRT.portID			            = UBXPRTUSB;
    msg->payload.CFG_PRT.inProtoMask		        = 0U;
    msg->payload.CFG_PRT.outProtoMask	            = 0U;
    
    msg = (UBXMsg*)&ubx_buf.data[40];
    /* configure the device to communicate over DDC (i2c) with a FIFO interrupt */
    msg->payload.CFG_PRT.portID					    = UBXPRTDDC;
    msg->payload.CFG_PRT.txReady.en				    = 1U;					/* 0 - disabled, 1 - enabled			*/
    msg->payload.CFG_PRT.txReady.pol			    = M8Q_TXR_POL;			/* 0 - High-active, 1 - Low-active		*/
    msg->payload.CFG_PRT.txReady.pin			    = M8Q_TXR_PIO;			/* PIO06 is TxD on the SAM-M8Q			*/
    msg->payload.CFG_PRT.txReady.thres		        = M8Q_TXR_CNT >> 3;	    /* Given value is multiplied by 8 bytes */
    msg->payload.CFG_PRT.mode.UBX_DDC.slaveAddr     = M8Q_SLAVE_ADDR;
    msg->payload.CFG_PRT.inProtoMask                = UBXPRTInProtoInUBX;
    msg->payload.CFG_PRT.outProtoMask               = UBXPRTOutProtoOutUBX;
    msg->payload.CFG_PRT.flags                      = UBXPRTExtendedTxTimeout;
    
    completeMsg(&ubx_buf, payloadSize);
    
    do {
        /* send message to configure the DDC serial port and verify */
        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
                result = gps_savecfg(0xFFFF);
                //result = GPS_SUCCESS;
            } else {
                result = GPS_FAILURE;
            }
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
    
    return result;
}    

uint8_t gps_disable_nmea() 
{
	int i;
	uint8_t         result;
	uint16_t        timeout;
	uint8_t rates[6] = {0,0,0,0,0,0};
	
	/* disable default (NMEA) messages */
	for(i = 0; i < 6; i++) {
	    ubx_buf = getCFG_MSG_RATES(UBXMsgClassNMEA, (UBXMessageId)i, rates);
		do {
			/* send message disable particular message on all IO ports */
			ubx_buf = getCFG_MSG_RATES(UBXMsgClassNMEA, (UBXMessageId)i, rates);
			if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
//                 if (GPS_SUCCESS == gps_ack()) {
//                    result = gps_savecfg(0xFFFE);
//                    //result = gps_ack();;
//                }
                result = GPS_SUCCESS;
			}
			/* repeat until timeout or successful acknowledge from device */
		} while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT*10);
    }        
    
	i = 0; result = GPS_FAILURE; timeout = 0;
    do {
        if(GPS_SUCCESS == gps_ack()) {
            i++;
        }
    }while(i < 6 && timeout++ < GPS_CFG_TIMEOUT);
    
    if(i == 6) {
        result = GPS_SUCCESS;
    } else {
        result = GPS_FAILURE;
    }                
	return result;
}
// 
// uint8_t gps_disable_nmea() 
// {
// 	int i;
// 	uint8_t         result;
// 	uint16_t        timeout;
// 	uint8_t rates[6] = {0,0,0,0,0,0};
// 	
// 	/* disable default (NMEA) messages */
// 	for(i = 0; i < 6; i++) {
// 		
// 		timeout = 0;
// 		result = GPS_FAILURE;
// 		
// 		do {
// 			/* send message disable particular message on all IO ports */
// 			ubx_buf = getCFG_MSG_RATES(UBXMsgClassNMEA, (UBXMessageId)i, rates);
// 			if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
//                 if (GPS_SUCCESS == gps_ack()) {
//                    result = gps_savecfg(0xFFFE);
//                    //result = gps_ack();;
//                }
// 			}
// 			/* repeat until timeout or successful acknowledge from device */
// 		} while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT*10);
//     }        
// 	
// 	return result;
// }

GPS_ERROR gps_init_channels()
{
    uint8_t     result  = GPS_FAILURE;
    uint16_t    timeout = 0;
    UBXMsg      *msg;
    /* Hard-coded message to dissable GLONASS while keeping GPS from u-blox */
    // TODO remove and code in actual satelite configs
    uint8_t     buf[0x3E] = {0x00,0x00,0x20,0x07,0x00,0x08,
                             0x10,0x00,0x01,0x00,0x01,0x01,
                             0x01,0x01,0x03,0x00,0x01,0x00,
                             0x01,0x01,0x02,0x04,0x08,0x00,
                             0x00,0x00,0x01,0x01,0x03,0x08,
                             0x10,0x00,0x00,0x00,0x01,0x01,
                             0x04,0x00,0x08,0x00,0x00,0x00,
                             0x01,0x01,0x05,0x00,0x03,0x00,
                             0x01,0x00,0x01,0x01,0x06,0x08,
                             0x0E,0x00,0x00,0x00,0x01,0x01, 0x2e, 0x85};
    
    UBXCFG_GNSS_PART part[7];

    ubx_buf = getCFG_GNSS(0,0,32,7,part);
    msg = (UBXMsg*)ubx_buf.data;
    memcpy(&ubx_buf.data[6], (char*)buf, msg->hdr.length + 2);
    do {
        /* send message disable particular message on all IO ports */
        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
               /* result = gps_savecfg(0xFFFE);*/
                result = GPS_SUCCESS;
            } else {
                result = GPS_FAILURE;
            }                
            	
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
    
    return result;
}

uint8_t gps_init_msgs()
{
	uint8_t         result  = GPS_FAILURE;
	uint16_t        timeout = 0;	

	/* set up NAV messages */
	do {
		/* send message disable particular message on all IO ports */
		ubx_buf = getCFG_MSG_RATE(UBXMsgClassNAV,UBXMsgIdNAV_PVT,1);
		if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
                result = gps_savecfg(0xFFFF);
               // result = GPS_SUCCESS;
            } else {
                result = GPS_FAILURE;
		    }         
        }            
		/* repeat until timeout or successful acknowledge from device */
	} while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
	
	return result;
}

uint8_t gps_cfgprt(const UBXMsg MSG)
{
	uint8_t         result  = GPS_FAILURE;
	uint16_t        timeout = 0;
	
	do {
		/* send message to configure the DDC serial port and verify */
		ubx_buf = setCFG_PRT(MSG.payload.CFG_PRT);
		if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
                //result = gps_savecfg(0xFFFF);
                 result = GPS_SUCCESS;
            }
		}
		/* repeat until timeout or successful acknowledge from device */
	} while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
	
	return result;
}

uint8_t gps_readfifo()
{
    int32_t ret;
	uint16_t timeout = 0;

    /* set up the i2c packet */
    gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
    gps_i2c_msg.len     = GPS_FIFOSIZE;
    gps_i2c_msg.flags   = I2C_M_STOP | I2C_M_RD;
    gps_i2c_msg.buffer  = GPS_FIFO;

    /* send, repeat until successful or timeout */
    ret = _i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg);
    while (ret) {
        if (timeout++ == GPS_I2C_TIMEOUT*4) {
            return GPS_FAILURE;
        }
        ret = _i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg);
    }
	
    return GPS_SUCCESS;
}


uint8_t gps_write_i2c(const uint8_t *DATA, const uint16_t SIZE)
{
	uint16_t timeout = 0;
	
    /* set up the i2c packet */
	gps_i2c_msg.addr	= M8Q_SLAVE_ADDR;
	gps_i2c_msg.len		= SIZE;
	gps_i2c_msg.flags	= I2C_M_STOP;
	gps_i2c_msg.buffer	= (uint8_t*)DATA;
	
    /* send, repeat until successful or timeout */
	while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
		if (timeout++ == GPS_I2C_TIMEOUT) {
			return GPS_FAILURE;
		}
	}
	
	return GPS_SUCCESS;
}

uint8_t gps_read_i2c(uint8_t *data, const uint16_t SIZE)
{
	uint16_t timeout = 0;

    /* set up the i2c packet */
    gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
    gps_i2c_msg.len     = SIZE;
    gps_i2c_msg.flags   = I2C_M_STOP | I2C_M_RD;
    gps_i2c_msg.buffer  = data;

    /* send, repeat until successful or timeout */
    while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
        if (timeout++ == GPS_I2C_TIMEOUT) {
            return GPS_FAILURE;
        }
    }
    return GPS_SUCCESS;
}

uint8_t gps_read_i2c_poll(uint8_t *data, const uint16_t SIZE)
{
	uint16_t timeout = 0;
	
	/* set up the first i2c packet */
	gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
	gps_i2c_msg.len     = 1;
	gps_i2c_msg.flags   = I2C_M_RD;
	gps_i2c_msg.buffer  = data;
	
	do { /* attempt reads until the data returned is not 0xFF (no data) */
		while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
			if (timeout++ == GPS_I2C_TIMEOUT) {
           		_i2c_m_sync_send_stop(&gps_i2c_desc.device);
				return GPS_FAILURE;
			}
		}
	} while (data[0] == 0xFF && timeout++ < GPS_CFG_TIMEOUT);

	if (data[0] != 0xB5 && GPS_FAILURE == gps_read_i2c_clear()) {
		_i2c_m_sync_send_stop(&gps_i2c_desc.device); 
		return GPS_INVALID;
	}
    
	/* set up the remaining i2c packet */
    timeout             = 0;
	gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
	gps_i2c_msg.len     = SIZE - 1;
	gps_i2c_msg.flags   |= I2C_M_STOP;
	gps_i2c_msg.buffer  = &data[1];

	/* send, repeat until successful or timeout */
	while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
		if (timeout++ == GPS_I2C_TIMEOUT) {
			return GPS_FAILURE;
		}
	}

	return GPS_SUCCESS;
}

uint8_t gps_read_i2c_clear()
{
    uint8_t  data[2];
    uint16_t timeout = 0;
	
	/* set up the first i2c packet */
	gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
	gps_i2c_msg.len     = 1;
	gps_i2c_msg.flags   = I2C_M_RD;
	gps_i2c_msg.buffer  = data;
	
	do { /* attempt reads until the data returned is not 0xFF (no data) */
		while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
			if (timeout++ == GPS_I2C_TIMEOUT) {
           		_i2c_m_sync_send_stop(&gps_i2c_desc.device);
				return GPS_FAILURE;
			}
		}
	} while (data[0] != 0xFF && timeout++ < GPS_MAX_MESSAGE);
    
    return timeout < GPS_MAX_MESSAGE ? GPS_SUCCESS : GPS_FAILURE;
}

uint8_t gps_parsefifo(gps_log_t *log, const uint16_t LOG_SIZE)
{
	uint8_t i;			/* log entry index				*/
	uint16_t offset;	/* position to read from FIFO	*/
	UBXMsg *ubx_msg_p;	/* pointer to message in FIFO	*/
	
	i = 0;
	offset = 0;
	ubx_msg_p = NULL;
	
	/* parse messages until  the LOG array is full, or the FIFO is empty */
	while (i < LOG_SIZE && offset < GPS_FIFOSIZE) {
		/* find the first UBX message and update the position */
		offset += alignUBXmessage(&ubx_msg_p, &GPS_FIFO[offset], GPS_FIFOSIZE - offset);
	
		/* extract the data from the message according to the message type */
		if (ubx_msg_p != NULL) {
			switch (ubx_msg_p->hdr.msgId) {
				case UBXMsgIdNAV_PVT :
					log[i].position.fixOk	= (bool)ubx_msg_p->payload.NAV_PVT.flags.gnssFixOk;
					log[i].position.lat		= ubx_msg_p->payload.NAV_PVT.lat;
					log[i].position.lon		= ubx_msg_p->payload.NAV_PVT.lon;
                    #if (GPS_VERBOSE_LOG == 1)
                    log[i].position.fixType = ubx_msg_p->payload.NAV_PVT.fixType;
                    log[i].position.hAcc    = ubx_msg_p->payload.NAV_PVT.hAcc;
                    log[i].position.vAcc    = ubx_msg_p->payload.NAV_PVT.vAcc;
                    #endif
					log[i].time.year		= ubx_msg_p->payload.NAV_PVT.year;
					log[i].time.month		= ubx_msg_p->payload.NAV_PVT.month;
					log[i].time.day			= ubx_msg_p->payload.NAV_PVT.day;
					log[i].time.hour		= ubx_msg_p->payload.NAV_PVT.hour;
					log[i].time.minute		= ubx_msg_p->payload.NAV_PVT.min;
					log[i].time.second		= ubx_msg_p->payload.NAV_PVT.sec;
					log[i].time.nano		= ubx_msg_p->payload.NAV_PVT.nano;
					i++;
					break;
				case UBXMsgIdNAV_TIMEUTC :
					log[i].position.fixOk	= false;
					log[i].position.lat		= (UBXI4_t)GPS_INVALID_LAT;
					log[i].position.lon		= (UBXI4_t)GPS_INVALID_LON;
                    #if (GPS_VERBOSE_LOG == 1)
                    log[i].position.fixType = UBXGPSTimeOnlyFix;
                    log[i].position.hAcc    = 0;
                    log[i].position.vAcc    = 0;
                    #endif
					log[i].time.year		= ubx_msg_p->payload.NAV_TIMEUTC.year;
					log[i].time.month		= ubx_msg_p->payload.NAV_TIMEUTC.month;
					log[i].time.day			= ubx_msg_p->payload.NAV_TIMEUTC.day;
					log[i].time.hour		= ubx_msg_p->payload.NAV_TIMEUTC.hour;
					log[i].time.minute		= ubx_msg_p->payload.NAV_TIMEUTC.min;
					log[i].time.second		= ubx_msg_p->payload.NAV_TIMEUTC.sec;
					log[i].time.nano		= ubx_msg_p->payload.NAV_TIMEUTC.nano;
					i++;
					break;
				/* add cases to support other messages */
				default:
					/* discard */
					break;
			}
		}
		/* increment the index and update the offset */
		offset += ubx_msg_p->hdr.length;
		ubx_msg_p = NULL;
	}
	
	return i;
}

GPS_ERROR gps_verifyprt()
{
	GPS_ERROR   result;
	UBXMsg		*msg;
    uint16_t    timeout = 0;
    
	do {
		/* poll the device for port configuration */
		ubx_buf = getCFG_PRT_POLL_OPT(UBXPRTDDC);
		if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
                //result = gps_savecfg(0xFFFF);
                 result = GPS_SUCCESS;
            }
		}
		/* repeat until timeout or successful acknowledge from device */
	} while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
    
    if(timeout >= GPS_CFG_TIMEOUT) {
        return GPS_FAILURE;
    }        
    timeout = 0;

    ubx_buf = getCFG_PRT();
    delay_ms(5);
    if (GPS_SUCCESS == gps_read_i2c((uint8_t*)ubx_buf.data, ubx_buf.size + 10)) {
        msg = (UBXMsg*)ubx_buf.data;
        if (UBXMsgClassACK == msg->hdr.msgClass) {
            msg = (UBXMsg*)&ubx_buf.data[10];
        }            
	} else {
        return GPS_FAILURE;
    }        
    
	/* verify the FIFO and watermark settings */
	if ((msg->payload.CFG_PRT.flags & UBXPRTExtendedTxTimeout) &&
        (msg->payload.CFG_PRT.txReady.en    == 1U			 ) &&
		(msg->payload.CFG_PRT.txReady.pol   == M8Q_TXR_POL	 ) &&
		(msg->payload.CFG_PRT.txReady.pin   == M8Q_TXR_PIO	 ) &&
		(msg->payload.CFG_PRT.txReady.thres == (M8Q_TXR_CNT >> 3))) 
	{
		result = GPS_SUCCESS;
	} else {
		result = GPS_INVALID;
	}
	
	return result;
}

int32_t gps_checkfifo()
{
	uint8_t buf[2];
	uint16_t timeout = 0;
	int32_t	 retval = 0;
	
	buf[0] = M8Q_BYTES_HI_ADDR;

	/* set up the i2c packet */
	gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
	gps_i2c_msg.len     = 1;
	gps_i2c_msg.flags   = 0;
	gps_i2c_msg.buffer  = buf;

	/* send, repeat until successful or timeout */
	while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
		if (timeout++ == GPS_I2C_TIMEOUT) {
            _i2c_m_sync_send_stop(&gps_i2c_desc.device);
			return -GPS_FAILURE;
		}
	}
	
	/* set up the i2c packet */
	gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
	gps_i2c_msg.len     = 1;
	gps_i2c_msg.flags   = I2C_M_RD | I2C_M_STOP;
	gps_i2c_msg.buffer  = buf;
	
	/* send, repeat until successful or timeout */
	while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
		if (timeout++ == GPS_I2C_TIMEOUT) {
			return -GPS_FAILURE;
		}
	}
	
	buf[1] = M8Q_BYTES_LO_ADDR;

	/* set up the i2c packet */
	gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
	gps_i2c_msg.len     = 1;
	gps_i2c_msg.flags   = 0;
	gps_i2c_msg.buffer  = &buf[1];

	/* send, repeat until successful or timeout */
	while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
		if (timeout++ == GPS_I2C_TIMEOUT) {
       		_i2c_m_sync_send_stop(&gps_i2c_desc.device);
			return -GPS_FAILURE;
		}
	}
		
	/* set up the i2c packet */
	gps_i2c_msg.addr    = M8Q_SLAVE_ADDR;
	gps_i2c_msg.len     = 1;
	gps_i2c_msg.flags   = I2C_M_RD | I2C_M_STOP;
	gps_i2c_msg.buffer  = &buf[1];
		
	/* send, repeat until successful or timeout */
	while (_i2c_m_sync_transfer(&gps_i2c_desc.device, &gps_i2c_msg)) {
		if (timeout++ == GPS_I2C_TIMEOUT) {
			return -GPS_FAILURE;
		}
	}
		
	retval = (buf[0] << 8) | (buf[1] << 0);
		
	return retval;
}

GPS_ERROR gps_cfgpsmoo_magic(uint32_t period)
{
    int32_t result = GPS_FAILURE;
    uint16_t timeout = 0;
    uint8_t magic[56] = {0xb5, 0x62, 0x06, 0x3b, 0x30, 0x00, 0x02, 0x06,
                         0x0a, 0x00, 0x20, 0x91, 0x40, 0x01, 0x30, 0x75,
                         0x00, 0x00, 0x60, 0xea, 0x00, 0x00, 0x00, 0x00,
                         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2c, 0x01,
                         0x00, 0x00, 0x4f, 0xc1, 0x03, 0x00, 0x87, 0x02,
                         0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x64, 0x40,
                         0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xd1, 0x48};
                         
//     uint8_t magic[52] = {0xb5, 0x62, 0x06, 0x3b, 0x2c, 0x00, 0x01, 0x06,
//                         0x0a, 0x00, 0x0e, 0x91, 0x42, 0x01, 0x10, 0x27,
//                         0x00, 0x00, 0x20, 0x4e, 0x00, 0x00, 0x00, 0x00,
//                         0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x2c, 0x01,
//                         0x00, 0x00, 0x4f, 0xc1, 0x03, 0x00, 0x87, 0x02,
//                         0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x64, 0x40,
//                         0x01, 0x00, 0x74, 0xbe};
                
  

    do {
        /* build the UBX message  */
 //       ubx_buf = getCFG_PM2(ubx_msg.payload.CFG_PM2);

        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)magic, 52)) {
            if (GPS_SUCCESS == gps_ack()) {
               // result = gps_savecfg(0xFFFE);
               return GPS_SUCCESS;
            } else {
               return GPS_FAILURE;
            }
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT*2);

    return result;
}

GPS_ERROR gps_cfgpsmoo(uint32_t period)
{
    GPS_ERROR       result  = GPS_FAILURE;
    uint16_t        timeout = 0;

    memset(ubx_buf.data, 0x00, 210);
    /* set the power mode variables for UBX protocol v18 */
    ubx_msg.payload.CFG_PM2.version = 0x02; 
    ubx_msg.payload.CFG_PM2.maxStartupStateDur = 0;
    ubx_msg.payload.CFG_PM2.reserved1 = 0x06;
    ubx_msg.payload.CFG_PM2.flags.blank2 = 0x04;
    ubx_msg.payload.CFG_PM2.flags.blank3 = 0x28;

    /* set the power mode flags for minimal ON/OFF operation */
    ubx_msg.payload.CFG_PM2.flags.mode = UBXPM2OnOffOperation;
    ubx_msg.payload.CFG_PM2.flags.extIntSelect  = 0;
    ubx_msg.payload.CFG_PM2.flags.extIntWake    = 0;
    ubx_msg.payload.CFG_PM2.flags.extIntBackup  = 0;
    ubx_msg.payload.CFG_PM2.flags.extIntInactive= 0;
    ubx_msg.payload.CFG_PM2.flags.limitPeakCurr = UBXPM2LimitCurrentEnabled;
    ubx_msg.payload.CFG_PM2.flags.waitTimeFix   = 1; /* TODO need time fix before starting */
    ubx_msg.payload.CFG_PM2.flags.updateRTC     = 1;
    ubx_msg.payload.CFG_PM2.flags.updateEPH     = 1;
    ubx_msg.payload.CFG_PM2.flags.doNotEnterOff = 0;

    /* set the power mode timing variables */
    ubx_msg.payload.CFG_PM2.updatePeriod    = (UBXU4_t)(period);
    ubx_msg.payload.CFG_PM2.searchPeriod    = (UBXU4_t)(period*GPS_SEARCH_MUL);
    ubx_msg.payload.CFG_PM2.gridOffset      = 0;
    ubx_msg.payload.CFG_PM2.onTime          = 10;
    ubx_msg.payload.CFG_PM2.minAcqTime      = 10;
    ubx_msg.payload.CFG_PM2.extintInactivityMs = period;
  

    do {
        /* build the UBX message  */
        ubx_buf = getCFG_PM2(ubx_msg.payload.CFG_PM2);

        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
               // result = gps_savecfg(0xFFFE);
               result = GPS_SUCCESS;
            } else {
               result = GPS_FAILURE;
            }
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);

    return result;
}

GPS_ERROR gps_cfgpsmoo_18(uint32_t period)
{
    GPS_ERROR       result  = GPS_FAILURE;
    uint16_t        timeout = 0;

    /* set the power mode variables for UBX protocol v18 */
    ubx_msg.payload.CFG_PMS.version         = 0x00;
    ubx_msg.payload.CFG_PMS.powerSetupValue = UBXPMSInterval;
    ubx_msg.payload.CFG_PMS.period          = period;
    ubx_msg.payload.CFG_PMS.onTime          = 0x00;
    ubx_msg.payload.CFG_PMS.reserved[0]     = 0x00;
    ubx_msg.payload.CFG_PMS.reserved[1]     = 0x00;
    
    do {
        /* build the UBX message  */
        ubx_buf = setCFG_PMS(ubx_msg.payload.CFG_PMS);

        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
                result = gps_savecfg(0xFFFE);
            }
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
        
    if (GPS_SUCCESS != result) {
        return GPS_FAILURE;
    }

    return result;
}

GPS_ERROR gps_savecfg(const uint16_t MASK)
{
    GPS_ERROR       result  = GPS_FAILURE;
    uint16_t        timeout = 0;
   
    do {
        /* build the UBX message  */
        ubx_buf = getCFG_CFG_OPT(0, (UBXX4_t)((uint32_t)MASK & 0x0000FFFF), 0,7);
//        ubx_buf = getCFG_CFG_OPT(0, (UBXX4_t)(0xFFFFFFFF), 0,7);

        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            result = gps_ack();
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result&& timeout++ < GPS_CFG_TIMEOUT);
    
    return result;
}

GPS_ERROR gps_loadcfg(const uint16_t MASK)
{
    GPS_ERROR       result  = GPS_FAILURE;
    uint16_t        timeout = 0;
    
    do {
        /* build the UBX message  */
        ubx_buf = getCFG_CFG_OPT(0, 0, (UBXX4_t)((uint32_t)MASK & 0x0000FFFF), 7);

        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            result = gps_ack();
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
    
    return result;
}

GPS_ERROR gps_clearcfg(const uint16_t MASK)
{
    GPS_ERROR       result  = GPS_FAILURE;
    uint16_t        timeout = 0;
    
    do {
        /* build the UBX message  */
        ubx_buf = getCFG_CFG_OPT((UBXX4_t)((uint32_t)MASK & 0x0000FFFF), 0, 0, 7);

        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            result = gps_ack();
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);
    
    return result;
}

GPS_ERROR gps_ack()
{
    UBXMsg*     ack_msg;
    GPS_ERROR   result = GPS_FAILURE;

//     ack_buf = getACK_ACK();
//     if (GPS_SUCCESS == gps_read_i2c_poll((uint8_t*)ack_buf.data, ack_buf.size)) {
//         ack_msg = (UBXMsg*)ack_buf.data;
//         if (ack_msg != NULL && ack_msg->hdr.msgClass == UBXMsgClassACK) {
//             result = ack_msg->hdr.msgId == UBXMsgIdACK_ACK ? GPS_SUCCESS : GPS_INVALID;
//         }
//     }
    result = GPS_SUCCESS;
    return result;
}

GPS_ERROR gps_setrate(const uint32_t period)
{
    uint8_t     result  = GPS_FAILURE;
    uint16_t    timeout = 0;
        
    /* set default 1Hz rate */
    do {
        /* send message disable particular message on all IO ports */
        ubx_buf = getCFG_RATE(1000,1,0);
        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
                result = gps_ack();
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT);

    return result;
}

GPS_ERROR gps_enablepsm() 
{
    GPS_ERROR       result  = GPS_FAILURE;
    uint16_t        timeout = 0;

    do {
        /* build the UBX message  */
        ubx_buf = getCFG_RXM(UBXRXMPowerSaveMode);

        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            if (GPS_SUCCESS == gps_ack()) {
                result = gps_savecfg(0xFFFF);
            } else {
                result = GPS_FAILURE;
            }
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT*2);

    return result;
}


GPS_ERROR gps_sleep()
{
    /* controlled GNSS stop */
    ubx_buf = getCFG_RST(0x08, 0);
    return gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size);
}

GPS_ERROR gps_wake()
{
    /* controlled GNSS stop */
    ubx_buf = getCFG_RST(0x09, 0);
    return gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size);
}

GPS_ERROR gps_nap(uint32_t ms)
{
    GPS_ERROR       result  = GPS_FAILURE;
    uint16_t        timeout = 0;

    do {
        /* build the UBX message  */
        
        ubx_buf = getRXM_PMREQ((UBXU4_t)ms, UBXPMREQBackup|UBXPMREQForce, UBXPMREQExtint0);
        if (GPS_SUCCESS == gps_write_i2c((const uint8_t*)ubx_buf.data, ubx_buf.size)) {
            result = GPS_SUCCESS;
        }
        /* repeat until timeout or successful acknowledge from device */
    } while (GPS_SUCCESS != result && timeout++ < GPS_CFG_TIMEOUT*2);

    return result;

}