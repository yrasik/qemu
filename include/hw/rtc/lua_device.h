/*
 * lua_device for QEMU
 *
 * Yuri Stepanenko stepanenkoyra@gmail.com   2022
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Contributions after 2012-01-13 are licensed under the terms of the
 * GNU GPL, version 2 or (at your option) any later version.
 */

#ifndef HW_LUA_DEVICE_H
#define HW_LUA_DEVICE_H

#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include <lua5.3/lua.h>

#define TYPE_LUA_DEVICE "lua_device"
OBJECT_DECLARE_SIMPLE_TYPE(LUA_DEVICEState, LUA_DEVICE)

struct LUA_DEVICEState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QEMUTimer *timer;
    qemu_irq irq;

    /*
     * Needed to preserve the tick_count across migration, even if the
     * absolute value of the rtc_clock is different on the source and
     * destination.
     */
    uint32_t tick_offset_vmstate;
    uint32_t tick_offset;
    bool tick_offset_migrated;
    bool migrate_tick_offset;

    uint32_t mr;
    uint32_t lr;
    uint32_t cr;
    uint32_t im;
    uint32_t is;

    lua_State *L;
    QEMUTimer *timer_exchange;
    int64_t nanoseconds_per_step;
    FILE *log_file;

};

#endif //HW_LUA_DEVICE_H
