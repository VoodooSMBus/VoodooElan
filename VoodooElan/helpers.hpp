/*
 * helper.hpp
 *
 * Copyright (c) 2020 VoodooSMBus, Leonard Kleinhans <leo-labs>
 *
 */

#ifndef helpers_hpp
#define helpers_hpp

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

uint64_t clock_get_uptime_nanoseconds();

#define IOLogInfo(format, ...) do { IOLog("VELN - Info: " format "\n", ## __VA_ARGS__); } while(0)
#define IOLogError(format, ...) do { IOLog("VELN - Error: " format "\n", ## __VA_ARGS__); } while(0)

#ifdef DEBUG
#define IOLogDebug(format, ...) do { IOLog("VELN - Debug: " format "\n", ## __VA_ARGS__); } while(0)
#else
#define IOLogDebug(arg...)
#endif // DEBUG

static const int kIOPMPowerOff = 0;

// power management
static IOPMPowerState ELANPowerStates[] = {
    {1, 0                , 0, 0           , 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};


#endif /* helpers_hpp */
