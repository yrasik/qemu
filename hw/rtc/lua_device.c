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


#define LUA_REG     0x20    /* Lua register */



static const unsigned char lua_device_id[] = {
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


static void lua_device_irq(LUA_DEVICEState *s)
{
  uint32_t flags = 1; //s->is & s->im;

  trace_lua_device_irq_state(flags);
  qemu_set_irq(s->irq, flags);
}


/**
  * @brief
  * @param  L: Указатель на Lua - машину.
  * @param
  * @param
  * @retval int32_t Возвращает 0 в случае успеха, отрицательные величины в случае неудачи.
  */
static int32_t lua_device_coroutine_yield(LUA_DEVICEState *s, uint64_t time_ns)
{
  int32_t status;
  lua_State *L = s->L;

  lua_getglobal(L, "coroutine_yield");
  lua_pushinteger(L, (lua_Integer)time_ns);
  if( lua_pcall(L, 1, 1, 0) != LUA_OK )
  {
    REPORT(MSG_ERROR, "if( lua_pcall(L, 1, 1, 0) != LUA_OK )" );
    return -1;
  }
#ifdef DEBUG
  if( ! lua_isinteger(L, -1))
  {
    REPORT(MSG_ERROR, "if(! lua_isinteger(L, -1))" );
    return -2;
  }
#endif

  status = (int32_t)lua_tointeger(L, -1);

#ifdef DEBUG
  if( status < 0 )
  {
    REPORT(MSG_ERROR, "if(status < 0)" );
    return -3;
  }
#endif

  if( status == 1 ) //INFO: Генерация прерывания
  {
    lua_device_irq(s);
  }

  return 0;
}


/**
  * @brief Чтение данных из Lua.
  * @param  L: Указатель на Lua - машину.
  * @param  TIME_I: временная метка для передачи в Lua.
  * @param  ADR_I: Адрес ячейки.
  * @param  DAT_O: Возвращает данные.
  * @retval int32_t Возвращает 0 в случае успеха, отрицательные величины в случае неудачи.
  */
static int32_t read_data(LUA_DEVICEState *s, uint64_t time_ns, uint64_t ADR_I, uint64_t *DAT_O)
{
  int32_t status;
  lua_State *L = s->L;

  lua_getglobal(L, "read_data");

  lua_pushinteger(L, (lua_Integer)time_ns);
  lua_pushinteger(L, (lua_Integer)ADR_I);

  if( lua_pcall(L, 2, 2, 0) != LUA_OK )
  {
    REPORT(MSG_ERROR, "if( lua_pcall(L, 2, 2, 0) != LUA_OK )" );
    return -1;
  }

#ifdef DEBUG
  if( ! lua_isinteger(L, -2))
  {
    REPORT(MSG_ERROR, "if(! lua_isinteger(L, -2))" );
    return -2;
  }
#endif

  status = (int32_t)lua_tointeger(L, -2);

#ifdef DEBUG
  if( status < 0 )
  {
    REPORT(MSG_ERROR, "if(status < 0)" );
    return -3;
  }
#endif

#ifdef DEBUG
  if( ! lua_isinteger(L, -1)) /* Элемент на вершине стека, т.е. последний помещённый на стек элемент */
  {
    REPORT(MSG_ERROR, "if(! lua_isinteger(L, -1))" );
    return -4;
  }
#endif

  *DAT_O = (uint64_t)lua_tointeger(L, -1);

  lua_pop(L, 2);

  if( status == 1 ) //INFO: Генерация прерывания
  {
    lua_device_irq(s);
  }

  return 0;
}


/**
  * @brief Запись данных в Lua.
  * @param  L: Указатель на Lua - машину.
  * @param  TIME_I: временная метка для передачи в Lua.
  * @param  ADR_I: Адрес ячейки.
  * @param  DAT_I: Считывает данные (32 бита) для передачи в Lua.
  * @retval int32_t Возвращает 0 в случае успеха, отрицательные величины в случае неудачи.
  */
static int32_t write_data(LUA_DEVICEState *s, uint64_t time_ns, uint64_t ADR_I, uint64_t DAT_I)
{
  int32_t status;
  lua_State *L = s->L;

  lua_getglobal(L, "write_data");

  lua_pushinteger(L, (lua_Integer)time_ns);
  lua_pushinteger(L, (lua_Integer)ADR_I);

  if( lua_pcall(L, 2, 1, 0) != LUA_OK )
  {
    REPORT(MSG_ERROR, "if( lua_pcall(L, 2, 1, 0) != LUA_OK )" );
    return -1;
  }

#ifdef DEBUG
  if( ! lua_isinteger(L, -1))
  {
    REPORT(MSG_ERROR, "if(! lua_isinteger(L, -1))" );
    return -2;
  }
#endif

  status = (int32_t)lua_tointeger(L, -1);

#ifdef DEBUG
  if( status < 0 )
  {
    REPORT(MSG_ERROR, "if(status < 0)" );
    return -2;
  }
#endif

  lua_pop(L, 1);

  if( status == 1 ) //INFO: Генерация прерывания
  {
    lua_device_irq(s);
  }

  return 0;
}





static void lua_device_timer_exchanger(void * opaque)
{
  LUA_DEVICEState *s = (LUA_DEVICEState *)opaque;

  int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
  REPORT(MSG_INFO, "time = %llu", now);
  lua_device_coroutine_yield(s, now); //FIXME ret value
  timer_mod(s->timer_exchange, now + s->nanoseconds_per_step);
}


static uint64_t lua_device_read(void *opaque, hwaddr offset, unsigned size)
{
    LUA_DEVICEState *s = (LUA_DEVICEState *)opaque;
    uint64_t r = 0;

    switch (offset)
    {
      case LUA_REG:
        read_data(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), (uint64_t)offset, &r); //FIXME: ret
        break;

      case 0xfe0 ... 0xfff:
        r = lua_device_id[(offset - 0xfe0) >> 2];
        break;
      default:
        qemu_log_mask(LOG_GUEST_ERROR, "lua_device_read: Bad offset 0x%x\n", (int)offset);
        r = 0;
        break;
    }

    trace_lua_device_read(offset, r);
    return r;
}


static void lua_device_write(void * opaque, hwaddr offset, uint64_t value, unsigned size)
{
    LUA_DEVICEState *s = (LUA_DEVICEState *)opaque;

    trace_lua_device_write(offset, value);

    switch (offset) 
    {
      case LUA_REG:
        write_data(s, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), (uint64_t)offset, value);
        break;

      default:
        qemu_log_mask(LOG_GUEST_ERROR, "lua_device_write: Bad offset 0x%x\n", (int)offset);
        break;
    }
}


static const MemoryRegionOps lua_device_ops = {
  .read = lua_device_read,
  .write = lua_device_write,
  .endianness = DEVICE_NATIVE_ENDIAN,
};


static void lua_device_init(Object *obj)
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

  if( init_lua(s, "lua_device.lua", s->log_file) < 0 )
  {
    REPORT(MSG_INFO, "if( init_lua(s, \"lua_device.lua\", s->log_file) < 0 )" );
    exit(1);
  }

  memory_region_init_io(&s->iomem, obj, &lua_device_ops, s, "lua_device", 0x1000);
  sysbus_init_mmio(dev, &s->iomem);

  sysbus_init_irq(dev, &s->irq);
  qemu_get_timedate(&tm, 0);
  s->tick_offset = mktimegm(&tm) -
        qemu_clock_get_ns(rtc_clock) / NANOSECONDS_PER_SECOND;

  /* Таймер для синхронизации с SystemC / iVerilog */
  s->timer_exchange = timer_new_ns(QEMU_CLOCK_VIRTUAL, lua_device_timer_exchanger, (void *)s);
  int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
  timer_mod(s->timer_exchange, now + s->nanoseconds_per_step);
}


static void lua_device_finalize(Object *obj)
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


static int lua_device_pre_save(void *opaque)
{
  //LUA_DEVICEState *s = opaque;

  return 0;
}


static int lua_device_pre_load(void *opaque)
{
  //LUA_DEVICEState *s = opaque;

  return 0;
}


static int lua_device_post_load(void *opaque, int version_id)
{
  //LUA_DEVICEState *s = opaque;

  return 0;
}


static int lua_device_tick_offset_post_load(void *opaque, int version_id)
{
  LUA_DEVICEState *s = opaque;

  s->tick_offset_migrated = true;
  return 0;
}

static bool lua_device_tick_offset_needed(void *opaque)
{
  LUA_DEVICEState *s = opaque;

  return s->migrate_tick_offset;
}

static const VMStateDescription vmstate_lua_device_tick_offset = {
    .name = "lua_device/tick-offset",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = lua_device_tick_offset_needed,
    .post_load = lua_device_tick_offset_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(tick_offset, LUA_DEVICEState),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_lua_device = {
    .name = "lua_device",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = lua_device_pre_save,
    .pre_load = lua_device_pre_load,
    .post_load = lua_device_post_load,
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
        &vmstate_lua_device_tick_offset,
        NULL
    }
};


static Property lua_device_properties[] = {
    /*
     * True to correctly migrate the tick offset of the RTC. False to
     * obtain backward migration compatibility with older QEMU versions,
     * at the expense of the guest RTC going backwards compared with the
     * host RTC when the VM is saved/restored if using -rtc host.
     * (Even if set to 'true' older QEMU can migrate forward to newer QEMU;
     * 'false' also permits newer QEMU to migrate to older QEMU.)
     */
    DEFINE_PROP_BOOL("migrate-tick-offset", LUA_DEVICEState, migrate_tick_offset, true),
    DEFINE_PROP_END_OF_LIST()
};


static void lua_device_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_lua_device;
    device_class_set_props(dc, lua_device_properties);
}


static const TypeInfo lua_device_info = {
    .name          = TYPE_LUA_DEVICE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(LUA_DEVICEState),
    .instance_init = lua_device_init,
    .instance_finalize = lua_device_finalize,
    .class_init    = lua_device_class_init,
};


static void lua_device_register_types(void)
{
    type_register_static(&lua_device_info);
}


type_init(lua_device_register_types)
