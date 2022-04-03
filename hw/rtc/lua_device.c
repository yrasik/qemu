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

#include "qemu/osdep.h"
#include "qemu-common.h"
#include "hw/rtc/lua_device.h"
#include "migration/vmstate.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "trace.h"
#include "qapi/qapi-events-misc-target.h"
#include <lua5.3/lua.h>
#include <lua5.3/lauxlib.h>
#include <lua5.3/lualib.h>


#define DEBUG
#define PFX  __FILE__
#define _FD_  s->log_file
#include "debug.h"


#define RTC_DR      0x00    /* Data read register */
#define RTC_MR      0x04    /* Match register */
#define RTC_LR      0x08    /* Data load register */
#define RTC_CR      0x0c    /* Control register */
#define RTC_IMSC    0x10    /* Interrupt mask and set register */
#define RTC_RIS     0x14    /* Raw interrupt status register */
#define RTC_MIS     0x18    /* Masked interrupt status register */
#define RTC_ICR     0x1c    /* Interrupt clear register */

#define LUA_REG     0x20    /* Lua register */



static const unsigned char pl031_id[] = {
    0xFF,/*0x31,*/ 0x10, 0x14, 0x00,         /* Device ID        */
    0x0d, 0xf0, 0x05, 0xb1          /* Cell ID      */
};







/**
  * @brief Инициализация Lua - машины.
  * @param  fname: Ссылка на строку с именем файла Lua - программы.
  * @retval int В случае неудачи возвращает отрицательные значения.
  */
static int init_lua(LUA_DEVICEState *s, const char *fname, FILE *log_file)
{
  int       err;
  lua_State *L;
  L = luaL_newstate();
  if( L == NULL )
  {
    REPORT(MSG_ERROR, "if( L == NULL )" );
    return -1;
  }

  luaL_openlibs( L );

  err = luaL_loadfile( L, fname );
  if ( err != LUA_OK )
  {
    lua_close( L );
    REPORT(MSG_ERROR, "if ( err != LUA_OK )" );
    return -2;
  }

  lua_pcall(L, 0, 0, 0);
  lua_getglobal(L, "init");

  if( lua_pcall(L, 0, 1, 0) != LUA_OK )
  {
    lua_close( L );
    REPORT(MSG_ERROR, "if( lua_pcall(L, 0, 1, 0) != LUA_OK )" );
    return -3;
  }

  if( ! lua_isinteger(L, -1))
  {
    lua_close( L );
    REPORT(MSG_ERROR, "if( ! lua_isinteger(L, -1))" );
    return -4;
  }

  int ret = lua_tointeger(L, -1);
  lua_pop(L, 1);


  lua_getglobal(L, "nanoseconds_per_step");
  if( lua_type(L, -1) != LUA_TNUMBER )
  {
    lua_close( L );
    REPORT(MSG_ERROR, "if( lua_type(L, -1) != LUA_TNUMBER )" );
    return -5;
  }
  s->nanoseconds_per_step = (int64_t)(lua_tointegerx(L, -1, NULL));

  s->L = L;
  return ret;
}


/**
  * @brief
  * @param  L: Указатель на Lua - машину.
  * @param
  * @param
  * @retval int32_t Возвращает 0 в случае успеха, отрицательные величины в случае неудачи.
  */
static int32_t lua_device_coroutine_yield(LUA_DEVICEState *s, int64_t time)
{
  lua_State *L = s->L;

  lua_getglobal(L, "coroutine_yield");
  lua_pushinteger(L, time);
  if( lua_pcall(L, 1, 0/*1*/, 0) != LUA_OK )
  {
    REPORT(MSG_ERROR, "if( lua_pcall(L, 1, 0, 0) != LUA_OK )" );
    return -1;
  }
/*
  if(! lua_isinteger(L, 1))
  {
    REPORT(MSG_ERROR, "if(! lua_isinteger(L, 1))" );
    return -2;
  }

  *DAT_O = (uint32_t)lua_tointeger(L, 1);
  lua_pop(L, 1);//FIXME ?
*/
  return 0;
}



/**
  * @brief Чтение данных из Lua.
  * @param  L: Указатель на Lua - машину.
  * @param  CMD_I: Команда: 0 - сброс источника, 1 - работа.
  * @param  DAT_O: Возвращает данные (32 бита) для записи в поток данных Verilog.
  * @retval int32_t Возвращает 0 в случае успеха, отрицательные величины в случае неудачи.
  */
static int32_t read_data(lua_State* L, const int32_t *CMD_I, int32_t *DAT_O)  // CMD_I -> lua -> DAT_O
{
  lua_getglobal(L, "read_data");

  lua_pushinteger(L, *CMD_I);

  if( lua_pcall(L, 1, 1, 0) != LUA_OK )
  {
    printf("ERROR : in 'read_data()'  '%s'\n", lua_tostring(L, -1));
    return -1;
  }

  if(! lua_isinteger(L, 1))
  {
    printf("ERROR : in return type from 'read_data()'  '%s'\n", lua_tostring(L, -1));
    return -4;
  }

  *DAT_O = (uint32_t)lua_tointeger(L, 1);

  lua_pop(L, 1);//FIXME ?

  return 0;
}


#if 0
/**
  * @brief Запись данных в Lua.
  * @param  L: Указатель на Lua - машину.
  * @param  TIME_I: временная метка для передачи в Lua.
  * @param  DAT_I: Считывает данные (32 бита) для передачи в Lua.
  * @retval int32_t Возвращает 0 в случае успеха, отрицательные величины в случае неудачи.
  */
static int32_t write_data(lua_State* L, const int32_t *TIME_I, const int32_t *DAT_I) // TIME_I, DAT_I -> lua
{
  lua_getglobal(L, "write_data");

  lua_pushinteger(L, *TIME_I);
  lua_pushinteger(L, *DAT_I);

  if( lua_pcall(L, 2, 0, 0) != LUA_OK )
  {
    printf("ERROR : in 'write_data()'  '%s'\n", lua_tostring(L, -1));
    return -1;
  }

  return 0;
}
#endif


#if 0
/**
  * @brief Обмен данными, приспособленный под интерфейс системной шины процессора.
  * @param  L: Указатель на Lua - машину.
  * @param  CMD_O: Возвращает код команды (ожидание, запись, чтение). Эта команда используется автоматом состояний, написанном на Verilog для отработки соответствующей временной диаграмы на шине данных.
  * @param  ADR_O: Возвращает адрес (32 бита) для формирования на шине адреса.
  * @param  DAT_O: Возвращает данные (32 бита) для формирования на шине данных.
  * @param  DAT_I: Считывает данные (32 бита), принимаемые по шине данных.
  * @param  STATUS_I: Считывает состояния сигналов сброса и прерываний (32 бита, рекомендуется в 31 бит помещать состояние сигнала сброса.)
  * @retval int32_t Возвращает 0 в случае успеха, отрицательные величины в случае неудачи.
  */
static int32_t exchange_CAD(lua_State* L, int32_t *CMD_O, int32_t *ADR_O, int32_t *DAT_O, const int32_t *DAT_I, const int32_t *STATUS_I)
{
  lua_getglobal(L, "exchange_CAD");

  lua_pushinteger(L, *DAT_I);
  lua_pushinteger(L, *STATUS_I);

  if( lua_pcall(L, 2, 3, 0) != LUA_OK )
  {
    //printf("ERROR : in 'exchange_CAD()'  '%s'\n", lua_tostring(L, -1));
    return -1;
  }


  if(! lua_isinteger(L, 1))
  {
     //printf("ERROR : in return type from 'exchange_CAD()'  '%s'\n", lua_tostring(L, -1));
     return -2;
  }
  *CMD_O = (uint32_t)lua_tointeger(L, 1);


  if(! lua_isinteger(L, 2))
  {
     //printf("ERROR : in return type from 'exchange_CAD()'  '%s'\n", lua_tostring(L, -1));
     return -3;
  }
  *ADR_O = (uint32_t)lua_tointeger(L, 2);


  if(! lua_isinteger(L, 3))
  {
     //printf("ERROR : in return type from 'exchange_CAD()'  '%s'\n", lua_tostring(L, -1));
     return -4;
  }

  *DAT_O = (uint32_t)lua_tointeger(L, 3);

  lua_pop(L, 3);//FIXME ?

  return 0;
}

#endif


static void lua_device_timer_exchanger(void * opaque)
{
  LUA_DEVICEState *s = (LUA_DEVICEState *)opaque;

  int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
  REPORT(MSG_INFO, "time = %llu", now);
  lua_device_coroutine_yield(s, now); //FIXME ret value
  timer_mod(s->timer_exchange, now + s->nanoseconds_per_step);
}


static void pl031_update(LUA_DEVICEState *s)
{
    uint32_t flags = s->is & s->im;

    trace_pl031_irq_state(flags);
    qemu_set_irq(s->irq, flags);
}

static void pl031_interrupt(void * opaque)
{
	LUA_DEVICEState *s = (LUA_DEVICEState *)opaque;

    s->is = 1;
    trace_pl031_alarm_raised();
    pl031_update(s);
}

static uint32_t pl031_get_count(LUA_DEVICEState *s)
{
    int64_t now = qemu_clock_get_ns(rtc_clock);
    return s->tick_offset + now / NANOSECONDS_PER_SECOND;
}

static void pl031_set_alarm(LUA_DEVICEState *s)
{
    uint32_t ticks;

    /* The timer wraps around.  This subtraction also wraps in the same way,
       and gives correct results when alarm < now_ticks.  */
    ticks = s->mr - pl031_get_count(s);
    trace_pl031_set_alarm(ticks);
    if (ticks == 0) {
        timer_del(s->timer);
        pl031_interrupt(s);
    } else {
        int64_t now = qemu_clock_get_ns(rtc_clock);
        timer_mod(s->timer, now + (int64_t)ticks * NANOSECONDS_PER_SECOND);
    }
}

static uint64_t pl031_read(void *opaque, hwaddr offset,
                           unsigned size)
{
	LUA_DEVICEState *s = (LUA_DEVICEState *)opaque;
    uint64_t r;

    switch (offset) {
    case RTC_DR:
        r = pl031_get_count(s);
        break;
    case RTC_MR:
        r = s->mr;
        break;
    case RTC_IMSC:
        r = s->im;
        break;
    case RTC_RIS:
        r = s->is;
        break;
    case RTC_LR:
        r = s->lr;
        break;
    case RTC_CR:
        /* RTC is permanently enabled.  */
        r = 1;
        break;
    case RTC_MIS:
        r = s->is & s->im;
        break;
    case 0xfe0 ... 0xfff:
        r = pl031_id[(offset - 0xfe0) >> 2];
        break;
    case RTC_ICR:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "lua_device: read of write-only register at offset 0x%x\n",
                      (int)offset);
        r = 0;
        break;
    case LUA_REG:
        { 
          int ret;
          int32_t CMD = 0;
          int32_t DAT = 0;
          if ( (ret = read_data(s->L, &CMD, &DAT)) < 0)
          {
            qemu_log_mask(LOG_GUEST_ERROR, "lua_device_read: LUA_REG: %d\n", ret);
          }
          r = (uint64_t)DAT;
        }
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "lua_device_read: Bad offset 0x%x\n", (int)offset);
        r = 0;
        break;
    }

    trace_pl031_read(offset, r);
    return r;
}

static void pl031_write(void * opaque, hwaddr offset,
                        uint64_t value, unsigned size)
{
	LUA_DEVICEState *s = (LUA_DEVICEState *)opaque;

    trace_pl031_write(offset, value);

    switch (offset) {
    case RTC_LR: {
        struct tm tm;

        s->tick_offset += value - pl031_get_count(s);

        qemu_get_timedate(&tm, s->tick_offset);
        qapi_event_send_rtc_change(qemu_timedate_diff(&tm));

        pl031_set_alarm(s);
        break;
    }
    case RTC_MR:
        s->mr = value;
        pl031_set_alarm(s);
        break;
    case RTC_IMSC:
        s->im = value & 1;
        pl031_update(s);
        break;
    case RTC_ICR:
        s->is &= ~value;
        pl031_update(s);
        break;
    case RTC_CR:
        /* Written value is ignored.  */
        break;

    case RTC_DR:
    case RTC_MIS:
    case RTC_RIS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "lua_device: write to read-only register at offset 0x%x\n",
                      (int)offset);
        break;

    case LUA_REG:



        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "lua_device_write: Bad offset 0x%x\n", (int)offset);
        break;
    }
}

static const MemoryRegionOps pl031_ops = {
    .read = pl031_read,
    .write = pl031_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void pl031_init(Object *obj)
{
  LUA_DEVICEState *s = LUA_DEVICE(obj);
  SysBusDevice *dev = SYS_BUS_DEVICE(obj);
  struct tm tm;
  s->L = NULL;
  char log_file_name[] = "lua_device.log";

  s->log_file = fopen(log_file_name, "w");

  if (s->log_file == NULL)
  {
    printf("ERROR: %s: %s @%d - if (s->log_file == NULL)\n", PFX, __func__, __LINE__);
    exit(1);
  }

  REPORT(MSG_INFO, "<<<< INIT: lua_device >>>>" );
  printf("<<<< INIT: lua_device >>>>\n");

  int ret = init_lua(s, "lua_device.lua", s->log_file);
  if(ret < 0)
  {
    exit(1);
  }

  memory_region_init_io(&s->iomem, obj, &pl031_ops, s, "lua_device", 0x1000);
  sysbus_init_mmio(dev, &s->iomem);

  sysbus_init_irq(dev, &s->irq);
  qemu_get_timedate(&tm, 0);
  s->tick_offset = mktimegm(&tm) -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;

  s->timer = timer_new_ns(rtc_clock, pl031_interrupt, s);

  /* Таймер для синхронизации с SystemC / iVerilog */
  s->timer_exchange = timer_new_ns(QEMU_CLOCK_VIRTUAL, lua_device_timer_exchanger, (void *)s);
  int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
  timer_mod(s->timer_exchange, now + s->nanoseconds_per_step);
}

static void pl031_finalize(Object *obj)
{
	LUA_DEVICEState *s = LUA_DEVICE(obj);

    timer_free(s->timer_exchange);
    timer_free(s->timer);

    if ( s->L != NULL )
    {
      lua_close( s->L );
    }

    REPORT(MSG_INFO, "<<<< DEINIT: lua_device >>>>" );
    fclose(s->log_file);
}

static int pl031_pre_save(void *opaque)
{
	LUA_DEVICEState *s = opaque;

    /*
     * The PL031 device model code uses the tick_offset field, which is
     * the offset between what the guest RTC should read and what the
     * QEMU rtc_clock reads:
     *  guest_rtc = rtc_clock + tick_offset
     * and so
     *  tick_offset = guest_rtc - rtc_clock
     *
     * We want to migrate this offset, which sounds straightforward.
     * Unfortunately older versions of QEMU migrated a conversion of this
     * offset into an offset from the vm_clock. (This was in turn an
     * attempt to be compatible with even older QEMU versions, but it
     * has incorrect behaviour if the rtc_clock is not the same as the
     * vm_clock.) So we put the actual tick_offset into a migration
     * subsection, and the backwards-compatible time-relative-to-vm_clock
     * in the main migration state.
     *
     * Calculate base time relative to QEMU_CLOCK_VIRTUAL:
     */
    int64_t delta = qemu_clock_get_ns(rtc_clock) - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tick_offset_vmstate = s->tick_offset + delta / NANOSECONDS_PER_SECOND;

    return 0;
}

static int pl031_pre_load(void *opaque)
{
	LUA_DEVICEState *s = opaque;

    s->tick_offset_migrated = false;
    return 0;
}

static int pl031_post_load(void *opaque, int version_id)
{
	LUA_DEVICEState *s = opaque;

    /*
     * If we got the tick_offset subsection, then we can just use
     * the value in that. Otherwise the source is an older QEMU and
     * has given us the offset from the vm_clock; convert it back to
     * an offset from the rtc_clock. This will cause time to incorrectly
     * go backwards compared to the host RTC, but this is unavoidable.
     */

    if (!s->tick_offset_migrated) {
        int64_t delta = qemu_clock_get_ns(rtc_clock) -
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        s->tick_offset = s->tick_offset_vmstate -
            delta / NANOSECONDS_PER_SECOND;
    }
    pl031_set_alarm(s);
    return 0;
}

static int pl031_tick_offset_post_load(void *opaque, int version_id)
{
	LUA_DEVICEState *s = opaque;

    s->tick_offset_migrated = true;
    return 0;
}

static bool pl031_tick_offset_needed(void *opaque)
{
	LUA_DEVICEState *s = opaque;

    return s->migrate_tick_offset;
}

static const VMStateDescription vmstate_pl031_tick_offset = {
    .name = "lua_device/tick-offset",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = pl031_tick_offset_needed,
    .post_load = pl031_tick_offset_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(tick_offset, LUA_DEVICEState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_pl031 = {
    .name = "lua_device",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = pl031_pre_save,
    .pre_load = pl031_pre_load,
    .post_load = pl031_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(tick_offset_vmstate, LUA_DEVICEState),
        VMSTATE_UINT32(mr, LUA_DEVICEState),
        VMSTATE_UINT32(lr, LUA_DEVICEState),
        VMSTATE_UINT32(cr, LUA_DEVICEState),
        VMSTATE_UINT32(im, LUA_DEVICEState),
        VMSTATE_UINT32(is, LUA_DEVICEState),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription*[]) {
        &vmstate_pl031_tick_offset,
        NULL
    }
};

static Property pl031_properties[] = {
    /*
     * True to correctly migrate the tick offset of the RTC. False to
     * obtain backward migration compatibility with older QEMU versions,
     * at the expense of the guest RTC going backwards compared with the
     * host RTC when the VM is saved/restored if using -rtc host.
     * (Even if set to 'true' older QEMU can migrate forward to newer QEMU;
     * 'false' also permits newer QEMU to migrate to older QEMU.)
     */
    DEFINE_PROP_BOOL("migrate-tick-offset",
    		LUA_DEVICEState, migrate_tick_offset, true),
    DEFINE_PROP_END_OF_LIST()
};

static void pl031_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_pl031;
    device_class_set_props(dc, pl031_properties);
}

static const TypeInfo pl031_info = {
    .name          = TYPE_LUA_DEVICE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LUA_DEVICEState),
    .instance_init = pl031_init,
    .instance_finalize = pl031_finalize,
    .class_init    = pl031_class_init,
};

static void lua_device_register_types(void)
{
    type_register_static(&pl031_info);
}

type_init(lua_device_register_types)
