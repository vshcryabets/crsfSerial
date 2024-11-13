#include "CrsfSerial.h"
#include <cstring>

#ifdef USE_PICO_SDK
    #include "pico.h"
    #include "pico/time.h"
#endif

#ifdef USE_ARDUINO
CrsfSerial::CrsfSerial(HardwareSerial &port, uint32_t baud) :
    _port(port), _crc(0xd5), _baud(baud),
    _lastReceive(0), _lastChannelsPacket(0), _linkIsUp(false),
    _passthroughMode(false)
{
    // Crsf serial is 420000 baud for V2
    setBaudrate(baud);
}
#endif

#ifdef USE_PICO_SDK
CrsfSerial::CrsfSerial(uart_inst_t *port, uint32_t baud) :
    _port(port), _crc(0xd5), _baud(baud),
    _lastReceive(0), _lastChannelsPacket(0), _linkIsUp(false),
    _passthroughMode(false)
{
    // Crsf serial is 420000 baud for V2
    setBaudrate(baud);
}
#endif

// Call from main loop to update
void CrsfSerial::loop()
{
    handleSerialIn();
}

void CrsfSerial::handleSerialIn()
{
    while (available())
    {
        uint8_t b = read();
        _lastReceive = millis();

        if (_passthroughMode)
        {
            if (onShiftyByte)
                onShiftyByte(b);
            continue;
        }

        _rxBuf[_rxBufPos++] = b;
        handleByteReceived();

        if (_rxBufPos == (sizeof(_rxBuf)/sizeof(_rxBuf[0])))
        {
            // Packet buffer filled and no valid packet found, dump the whole thing
            _rxBufPos = 0;
        }
    }

    checkPacketTimeout();
    checkLinkDown();
}

void CrsfSerial::handleByteReceived()
{
    bool reprocess;
    do
    {
        reprocess = false;
        if (_rxBufPos > 1)
        {
            uint8_t len = _rxBuf[1];
            // Sanity check the declared length, can't be shorter than Type, X, CRC
            if (len < 3 || len > CRSF_MAX_PACKET_LEN)
            {
                shiftRxBuffer(1);
                reprocess = true;
            }

            else if (_rxBufPos >= (len + 2))
            {
                uint8_t inCrc = _rxBuf[2 + len - 1];
                uint8_t crc = _crc.calc(&_rxBuf[2], len - 1);
                if (crc == inCrc)
                {
                    processPacketIn(len);
                    shiftRxBuffer(len + 2);
                    reprocess = true;
                }
                else
                {
                    shiftRxBuffer(1);
                    reprocess = true;
                }
            }  // if complete packet
        } // if pos > 1
    } while (reprocess);
}

void CrsfSerial::checkPacketTimeout()
{
    // If we haven't received data in a long time, flush the buffer a byte at a time (to trigger shiftyByte)
    if (_rxBufPos > 0 && millis() - _lastReceive > CRSF_PACKET_TIMEOUT_MS)
        while (_rxBufPos)
            shiftRxBuffer(1);
}

void CrsfSerial::checkLinkDown()
{
    if (_linkIsUp && millis() - _lastChannelsPacket > CRSF_FAILSAFE_STAGE1_MS)
    {
        if (onLinkDown)
            onLinkDown();
        _linkIsUp = false;
    }
}

void CrsfSerial::processPacketIn(uint8_t len)
{
    const crsf_header_t *hdr = (crsf_header_t *)_rxBuf;
    if (hdr->device_addr == CRSF_ADDRESS_FLIGHT_CONTROLLER)
    {
        switch (hdr->type)
        {
        case CRSF_FRAMETYPE_GPS:
            packetGps(hdr);
            break;
        case CRSF_FRAMETYPE_RC_CHANNELS_PACKED:
            packetChannelsPacked(hdr);
            break;
        case CRSF_FRAMETYPE_LINK_STATISTICS:
            packetLinkStatistics(hdr);
            break;
        }
    } // CRSF_ADDRESS_FLIGHT_CONTROLLER
}

// Shift the bytes in the RxBuf down by cnt bytes
void CrsfSerial::shiftRxBuffer(uint8_t cnt)
{
    // If removing the whole thing, just set pos to 0
    if (cnt >= _rxBufPos)
    {
        _rxBufPos = 0;
        return;
    }

    if (cnt == 1 && onShiftyByte)
        onShiftyByte(_rxBuf[0]);

    // Otherwise do the slow shift down
    uint8_t *src = &_rxBuf[cnt];
    uint8_t *dst = &_rxBuf[0];
    _rxBufPos -= cnt;
    uint8_t left = _rxBufPos;
    while (left--)
        *dst++ = *src++;
}

void CrsfSerial::packetChannelsPacked(const crsf_header_t *p)
{
    crsf_channels_t *ch = (crsf_channels_t *)&p->data;
    _channels[0] = ch->ch0;
    _channels[1] = ch->ch1;
    _channels[2] = ch->ch2;
    _channels[3] = ch->ch3;
    _channels[4] = ch->ch4;
    _channels[5] = ch->ch5;
    _channels[6] = ch->ch6;
    _channels[7] = ch->ch7;
    _channels[8] = ch->ch8;
    _channels[9] = ch->ch9;
    _channels[10] = ch->ch10;
    _channels[11] = ch->ch11;
    _channels[12] = ch->ch12;
    _channels[13] = ch->ch13;
    _channels[14] = ch->ch14;
    _channels[15] = ch->ch15;

    for (uint8_t i=0; i<CRSF_NUM_CHANNELS; ++i) {
        //map(_channels[i], CRSF_CHANNEL_VALUE_1000, CRSF_CHANNEL_VALUE_2000, 1000, 2000);
        int32_t newValue = ((int32_t)_channels[i] - CRSF_CHANNEL_VALUE_1000) * 1000 / (CRSF_CHANNEL_VALUE_2000 - CRSF_CHANNEL_VALUE_1000) + 1000;
        _channels[i] = newValue;
    }

    if (!_linkIsUp && onLinkUp)
        onLinkUp();
    _linkIsUp = true;
    _lastChannelsPacket = millis();

    if (onPacketChannels)
        onPacketChannels();
}

void CrsfSerial::packetLinkStatistics(const crsf_header_t *p)
{
    const crsfLinkStatistics_t *link = (crsfLinkStatistics_t *)p->data;
    memcpy(&_linkStatistics, link, sizeof(_linkStatistics));

    if (onPacketLinkStatistics)
        onPacketLinkStatistics(&_linkStatistics);
}

void CrsfSerial::packetGps(const crsf_header_t *p)
{
    const crsf_sensor_gps_t *gps = (crsf_sensor_gps_t *)p->data;
    _gpsSensor.latitude = be32toh(gps->latitude);
    _gpsSensor.longitude = be32toh(gps->longitude);
    _gpsSensor.groundspeed = be16toh(gps->groundspeed);
    _gpsSensor.heading = be16toh(gps->heading);
    _gpsSensor.altitude = be16toh(gps->altitude);
    _gpsSensor.satellites = gps->satellites;

    if (onPacketGps)
        onPacketGps(&_gpsSensor);
}

void CrsfSerial::write(uint8_t b)
{    
#ifdef USE_ARDUINO
    _port.write(b);
#elif defined(USE_PICO_SDK)
    uart_putc_raw(_port, b);
#endif

}

void CrsfSerial::write(const uint8_t *buf, size_t len)
{
#ifdef USE_ARDUINO
    _port.write(buf, len);
#elif defined(USE_PICO_SDK)
    uart_write_blocking(_port, buf, len);
#endif
}

void CrsfSerial::queuePacket(uint8_t addr, uint8_t type, const void *payload, uint8_t len)
{
    if (!_linkIsUp)
        return;
    if (_passthroughMode)
        return;
    if (len > CRSF_MAX_PACKET_LEN)
        return;

    uint8_t buf[CRSF_MAX_PACKET_LEN+4];
    buf[0] = addr;
    buf[1] = len + 2; // type + payload + crc
    buf[2] = type;
    memcpy(&buf[3], payload, len);
    buf[len+3] = _crc.calc(&buf[2], len + 1);

    // Busywait until the serial port seems free
    //while (millis() - _lastReceive < 2)
    //    loop();
    write(buf, len + 4);
}

void CrsfSerial::setPassthroughMode(bool val, unsigned int baud)
{
    _passthroughMode = val;
#ifdef USE_ARDUINO
    _port.flush();
#endif    
    if (baud != 0)
        setBaudrate(baud);
    else
        setBaudrate(_baud);
}

inline void CrsfSerial::setBaudrate(uint32_t baud) const {
#ifdef USE_ARDUINO
    _port.begin(_baud);
#elif defined(USE_PICO_SDK)
    uart_init(_port, baud);
#endif
}

inline uint32_t CrsfSerial::millis() const
 {
#ifdef USE_ANDROID
    return millis();
#elif defined(USE_PICO_SDK)    
    return to_ms_since_boot(get_absolute_time());
#endif    
 }

 inline bool CrsfSerial::available() const {
#ifdef USE_ANDROID
    return _port.available();
#elif defined(USE_PICO_SDK)    
    return uart_is_readable(_port);
#endif 
 }

 inline char CrsfSerial::read() const {
#ifdef USE_ANDROID
    return _port.read();
#elif defined(USE_PICO_SDK)    
    return uart_getc(_port);
#endif 
 }