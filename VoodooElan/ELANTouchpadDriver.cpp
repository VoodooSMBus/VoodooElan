// SPDX-License-Identifier: GPL-2.0-only
/*
 * ELANTouchpadDriver.cpp
 * Elan I2C/SMBus Touchpad driver port for macOS X
 *
 * Copyright (c) 2019 Leonard Kleinhans <leo-labs>
 *
 * Based on linux driver:
 ********************************************************************************
 *
 * Elan I2C/SMBus Touchpad driver
 *
 * Copyright (c) 2013 ELAN Microelectronics Corp.
 *
 * Author: 林政維 (Duson Lin) <dusonlin@emc.com.tw>
 * Author: KT Liao <kt.liao@emc.com.tw>
 * Version: 1.6.3
 *
 * Based on cyapa driver:
 * copyright (c) 2011-2012 Cypress Semiconductor, Inc.
 * copyright (c) 2011-2012 Google, Inc.
 *
 * Trademarks are the property of their respective owners.
 */
#include "ELANTouchpadDriver.hpp"


#define super IOService

OSDefineMetaClassAndStructors(ELANTouchpadDriver, IOService);

void ELANTouchpadDriver::loadConfiguration() {
    disable_while_typing = Configuration::loadBoolConfiguration(this, CONFIG_DISABLE_WHILE_TYPING, true);
    disable_while_trackpoint = Configuration::loadBoolConfiguration(this, CONFIG_DISABLE_WHILE_TRACKPOINT, true);
    
    ignore_set_touchpad_status = Configuration::loadBoolConfiguration(this, CONFIG_IGNORE_SET_TOUCHPAD_STATUS, false);
    
    disable_while_typing_timeout = Configuration::loadUInt64Configuration(this, CONFIG_DISABLE_WHILE_TYPING_TIMEOUT_MS, 500) * 1000000 ;
    disable_while_trackpoint_timeout = Configuration::loadUInt64Configuration(this, CONFIG_DISABLE_WHILE_TRACKPOINT_TIMEOUT_MS, 500) * 1000000 ;
}

bool ELANTouchpadDriver::init(OSDictionary *dict) {
    bool result = super::init(dict);
    loadConfiguration();
    
    data = reinterpret_cast<elan_tp_data*>(IOMalloc(sizeof(elan_tp_data)));

    awake = true;
    trackpointScrolling = false;
    return result;
}

void ELANTouchpadDriver::free(void) {
    IOFree(data, sizeof(elan_tp_data));
    super::free();
}

void ELANTouchpadDriver::releaseResources() {
    sendSleepCommand();
}

bool ELANTouchpadDriver::start(IOService* provider) {
    if (!super::start(provider)) {
        return false;
    }
    PMinit();
    provider->joinPMtree(this);
    registerPowerDriver(this, VoodooSMBusPowerStates, kVoodooSMBusPowerStates);
    
    device_nub->wakeupController();
    device_nub->setSlaveDeviceFlags(I2C_CLIENT_HOST_NOTIFY);
    setDeviceParameters();
    
    int error = tryInitialize();
    if(error) {
        IOLogError("Could not initialize ELAN device.");
        return false;
    }
    
    registerService();
    return true;
}


void ELANTouchpadDriver::stop(IOService* provider) {
    releaseResources();
    PMstop();
    super::stop(provider);
}

ELANTouchpadDriver* ELANTouchpadDriver::probe(IOService* provider, SInt32* score) {
    IOLog("Touchpad probe\n");
    if (!super::probe(provider, score)) {
        return NULL;
    }
    
    device_nub = OSDynamicCast(VoodooSMBusDeviceNub, provider);
    if (!device_nub) {
        IOLog("%s Could not get VoodooSMBus device nub instance\n", getName());
        return NULL;
    }
    return this;
}

IOReturn ELANTouchpadDriver::setPowerState(unsigned long whichState, IOService* whatDevice) {
    if (whatDevice != this)
        return kIOPMAckImplied;
    
    if (whichState == kIOPMPowerOff) {
        if (awake) {
            awake = false;
            sendSleepCommand();
        }
    } else {
        if (!awake) {
            IOLogDebug("ELANTouchpadDriver waking up");
            int error = tryInitialize();
            if(error) {
                IOLogError("Could not initialize ELAN device.");
            }
            awake = true;
        }
    }
    return kIOPMAckImplied;
}

bool ELANTouchpadDriver::handleOpen(IOService *forClient, IOOptionBits options, void *arg) {
    if (forClient && forClient->getProperty(VOODOO_INPUT_IDENTIFIER)) {
        voodooInputInstance = forClient;
        voodooInputInstance->retain();
        return true;
    }
    
    if (forClient && forClient->getProperty(VOODOO_TRACKPOINT_IDENTIFIER)) {
        voodooTrackpointInstance = forClient;
        voodooTrackpointInstance->retain();
        return true;
    }
    
    return super::handleOpen(forClient, options, arg);
}

bool ELANTouchpadDriver::handleIsOpen(const IOService *forClient) const {
    return (forClient == voodooInputInstance) || (forClient == voodooTrackpointInstance);
}

void ELANTouchpadDriver::handleClose(IOService *forClient, IOOptionBits options) {
    if (forClient == voodooInputInstance) {
        OSSafeReleaseNULL(voodooInputInstance);
    }
    if (forClient == voodooTrackpointInstance) {
        OSSafeReleaseNULL(voodooTrackpointInstance);
    }
    super::handleClose(forClient, options);
}


int ELANTouchpadDriver::tryInitialize() {
    IOSleep(3000);
    int repeat = ETP_RETRY_COUNT;
    int error;
    do {
        error = initialize();
        if (!error)
            return 0;
        
        IOSleep(100);
    } while (--repeat > 0);
    return error;
}

void ELANTouchpadDriver::handleHostNotify() {
    int error;
    u8 report[ETP_MAX_REPORT_LEN];
    error = getReport(report);
    if (error) {
        return;
    }
    
    // Check if input is disabled via ApplePS2Keyboard request
    if (ignoreall && !ignore_set_touchpad_status) {
        return;
    }
    
    // Ignore input for specified time after keyboard usage
    uint64_t timestamp_ns = clock_get_uptime_nanoseconds();
    if (disable_while_typing) {
        if (timestamp_ns - ts_last_keyboard < disable_while_typing_timeout) {
            return;
        }
    }
    
    switch (report[ETP_REPORT_ID_OFFSET]) {
        case ETP_REPORT_ID:
            // ignore touchpad for specified time after trackpoint usage
            if(disable_while_trackpoint) {
                if(timestamp_ns - ts_last_trackpoint < disable_while_trackpoint_timeout) {
                    break;
                }
            }
            reportAbsolute(report);
            break;
        case ETP_TP_REPORT_ID:
            reportTrackpoint(report);
            break;
        default:
            IOLogError("invalid report id data (%x)\n",
                    report[ETP_REPORT_ID_OFFSET]);
    }
}



// elan_smbus_initialize
int ELANTouchpadDriver::initialize() {
    UInt8 check[ETP_SMBUS_HELLOPACKET_LEN] = { 0x55, 0x55, 0x55, 0x55, 0x55 };
    UInt8 values[I2C_SMBUS_BLOCK_MAX] = {0};
    int len, error;
    
    /* Get hello packet */
    len = device_nub->readBlockData(ETP_SMBUS_HELLOPACKET_CMD, values);
    
    if (len != ETP_SMBUS_HELLOPACKET_LEN) {
        IOLog("hello packet length fail: %d\n", len);
        error = len < 0 ? len : -EIO;
        return error;
    }
    
    /* compare hello packet */
    if (memcmp(values, check, ETP_SMBUS_HELLOPACKET_LEN)) {
        IOLog("hello packet fail [%*ph]\n",
                ETP_SMBUS_HELLOPACKET_LEN, values);
        return -ENXIO;
    }
    
    /* enable tp */
    error = device_nub->writeByte(ETP_SMBUS_ENABLE_TP);
    if (error) {
        IOLog("failed to enable touchpad: %d\n", error);
        return error;
    }
    
    u8 mode = ETP_ENABLE_ABS;
    error = setMode(mode);
    if (error) {
        IOLogDebug("failed to switch to absolute mode: %d\n", error);
        return error;
    }
    
    return 0;
}

int ELANTouchpadDriver::setMode(u8 mode) {
    u8 cmd[4] = { 0x00, 0x07, 0x00, mode };
    
    return device_nub->writeBlockData(ETP_SMBUS_IAP_CMD, sizeof(cmd), cmd);
}

// TODO lets query stuff
bool ELANTouchpadDriver::setDeviceParameters() {
    
    u8 hw_x_res = 1, hw_y_res = 1;
    unsigned int x_traces = 1, y_traces = 1;
    
    data->max_x = 3052;
    data->max_y = 1888;
    data->width_x = data->max_x / x_traces;
    data->width_y = data->max_y / y_traces;
    
    data->pressure_adjustment = 25;
    
    data->x_res = convertResolution(hw_x_res);
    data->y_res = convertResolution(hw_y_res);

    setProperty(VOODOO_INPUT_LOGICAL_MAX_X_KEY, data->max_x, 16);
    setProperty(VOODOO_INPUT_LOGICAL_MAX_Y_KEY, data->max_y, 16);
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_X_KEY, data->max_x * 10 / data->x_res, 16);
    setProperty(VOODOO_INPUT_PHYSICAL_MAX_Y_KEY, data->max_y * 10 / data->y_res, 16);
    
    setProperty(VOODOO_INPUT_TRANSFORM_KEY, 0ull, 8);
    
    return true;
}

// elan_convert_resolution
unsigned int ELANTouchpadDriver::convertResolution(u8 val) {
    /*
     * (value from firmware) * 10 + 790 = dpi
     *
     * We also have to convert dpi to dots/mm (*10/254 to avoid floating
     * point).
     */
    
    return ((int)(char)val * 10 + 790) * 10 / 254;
}


// elan_smbus_get_report
int ELANTouchpadDriver::getReport(u8 *report)
{
    int len;
    
    len = device_nub->readBlockData(ETP_SMBUS_PACKET_QUERY,
                                    &report[ETP_SMBUS_REPORT_OFFSET]);
    if (len < 0) {
        IOLogError("failed to read report data: %d\n", len);
        return len;
    }
    
    if (len != ETP_SMBUS_REPORT_LEN) {
        IOLogError("wrong report length (%d vs %d expected)\n", len,
                   ETP_SMBUS_REPORT_LEN);
        return -EIO;
    }
    
    return 0;
}

void ELANTouchpadDriver::reportTrackpoint(u8 *report) {
    u8 *packet = &report[ETP_REPORT_ID_OFFSET + 1];
    int x = 0, y = 0;
    
    int btn_left = packet[0] & 0x01;
    int btn_right = packet[0] & 0x02;
    int btn_middle = packet[0] & 0x04;

    int button = btn_left | btn_right | btn_middle;
    if ((packet[3] & 0x0F) == 0x06) {
        x = packet[4] - (int)((packet[1] ^ 0x80) << 1);
        y = (int)((packet[2] ^ 0x80) << 1) - packet[5];
    }
    
    // trackpoint was used
    if(x != 0 || y != 0) {
        ts_last_trackpoint = clock_get_uptime_nanoseconds();
    }
    
    // enable trackpoint scroll mode when middle button was pressed and the trackpoint moved
    if (btn_middle == 4 && x != 0 && y!=0) {
        trackpointScrolling = true;
    }
    
    // disable trackpoint scrolling mode always when middle button is released
    if (trackpointScrolling && btn_middle == 0) {
        trackpointScrolling = false;
    }
    
    AbsoluteTime timestamp;
    clock_get_uptime(&timestamp);
    
    if(trackpointScrolling) {
        scrollEvent.deltaAxis1 = -y;
        scrollEvent.deltaAxis2 = -x;
        scrollEvent.deltaAxis3 = 0;
        scrollEvent.timestamp = timestamp;
        messageClient(kIOMessageVoodooTrackpointScrollWheel, voodooTrackpointInstance, &scrollEvent, sizeof(ScrollWheelEvent));
    } else {
        relativeEvent.buttons = button;
        relativeEvent.timestamp = timestamp;
        relativeEvent.dx = x;
        relativeEvent.dy = y;
        messageClient(kIOMessageVoodooTrackpointRelativePointer, voodooTrackpointInstance, &relativeEvent, sizeof(RelativePointerEvent));
    }
}

// elan_report_contact
void ELANTouchpadDriver::processContact(int finger_id, bool contact_valid, bool physical_button_down, u8 *finger_data, AbsoluteTime timestamp) {
    unsigned int pos_x, pos_y;
    unsigned int pressure, mk_x, mk_y;
    unsigned int area_x, area_y, major, minor;
    unsigned int scaled_pressure;
    
    auto& transducer = touchInputEvent.transducers[finger_id];
    
    
    transducer.secondaryId = finger_id;
    //transducer->logical_max_x = mt_interface->logical_max_x;
    //transducer->logical_max_y = mt_interface->logical_max_y;
    
    //transducer.isPhysicalButtonDown = tp_info & BIT(0);
    //transducer->physical_button.update(tp_info & BIT(0), timestamp);
    
    
    transducer.type = FINGER;
    //transducer->type = kDigitiserTransducerFinger;
    transducer.isValid = contact_valid;
    transducer.timestamp = timestamp;

    
    if (contact_valid) {
        pos_x = ((finger_data[0] & 0xf0) << 4) |
        finger_data[1];
        pos_y = ((finger_data[0] & 0x0f) << 8) |
        finger_data[2];
        mk_x = (finger_data[3] & 0x0f);
        mk_y = (finger_data[3] >> 4);
        pressure = finger_data[4];
        
        if (pos_x > data->max_x || pos_y > data->max_y) {
            IOLogDebug("[%d] x=%d y=%d over max (%d, %d)",
                    transducer.secondaryId, pos_x, pos_y,
                    data->max_x, data->max_y);
            return;
        }
        
        /*
         * To avoid treating large finger as palm, let's reduce the
         * width x and y per trace.
         */
        area_x = mk_x * (data->width_x - ETP_FWIDTH_REDUCE);
        area_y = mk_y * (data->width_y - ETP_FWIDTH_REDUCE);
        
        major = max(area_x, area_y);
        minor = min(area_x, area_y);
        
        scaled_pressure = pressure + data->pressure_adjustment;
        
        if (scaled_pressure > ETP_MAX_PRESSURE)
            scaled_pressure = ETP_MAX_PRESSURE;
        
        transducer.previousCoordinates = transducer.currentCoordinates;
        
        transducer.currentCoordinates.x = pos_x;
        transducer.currentCoordinates.y = pos_y;
        
       // TODO
       // transducer->tip_switch.update(1, timestamp);

    } else {
        transducer.currentCoordinates.x = transducer.previousCoordinates.x;
        transducer.currentCoordinates.y = transducer.previousCoordinates.y;
        // TODO

        //transducer->tip_switch.update(0, timestamp);
    }
}

// elan_report_absolute
void ELANTouchpadDriver::reportAbsolute(u8 *packet) {
    u8 *finger_data = &packet[ETP_FINGER_DATA_OFFSET];
    int i;
    u8 tp_info = packet[ETP_TOUCH_INFO_OFFSET];
    //u8 hover_info = packet[ETP_HOVER_INFO_OFFSET];
    bool contact_valid, physical_button_down;
    //hover_event;
    
    AbsoluteTime timestamp;
    clock_get_uptime(&timestamp);
    
    touchInputEvent.contact_count = 0;
    touchInputEvent.timestamp = timestamp;
    
    //hover_event = hover_info & 0x40;
    for (i = 0; i < ETP_MAX_FINGERS; i++) {
        contact_valid = tp_info & (1U << (3 + i));
        physical_button_down = tp_info & BIT(0);
        
        processContact(i, contact_valid, physical_button_down, finger_data, timestamp);
        
        if (contact_valid) {
            finger_data += ETP_FINGER_DATA_LEN;
            touchInputEvent.contact_count++;
        }
    }
   
    messageClient(kIOMessageVoodooInputMessage, voodooInputInstance, &touchInputEvent, sizeof(VoodooInputEvent));
}

void ELANTouchpadDriver::sendSleepCommand() {
    device_nub->writeByte(ETP_SMBUS_SLEEP_CMD);
}

IOReturn ELANTouchpadDriver::message(UInt32 type, IOService* provider, void* argument) {
    switch (type) {
        case kKeyboardGetTouchStatus: {
            bool* pResult = (bool*)argument;
            *pResult = !ignoreall;
            break;
        }
        case kKeyboardSetTouchStatus: {
            bool enable = *((bool*)argument);

            // ignoreall is true when trackpad has been disabled
            if (enable == ignoreall) {
                // save state, and update LED
                ignoreall = !enable;
            }
            break;
        }
        case kKeyboardKeyPressTime: {
            //  Remember last time key was pressed
            ts_last_keyboard = *((uint64_t*)argument);
            break;
        }
        case kIOMessageVoodooSMBusHostNotify: {
            handleHostNotify();
            break;
        }
    }
    
    return kIOReturnSuccess;
}
