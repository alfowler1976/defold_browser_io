#include <dmsdk/sdk.h>
#include <assert.h>
#include <stdint.h>
#include <string>

#include <browser_io_private.h>
#include <browser_io_web.h>

#define LIB_NAME "BrowserIO"
#define MODULE_NAME "browser_io"

using namespace dmBrowserIO;

namespace
{
    constexpr uint32_t DEFAULT_MAX_FILE_SIZE = 16 * 1024 * 1024; // 16 mb default limit

    // -------------------------------------------------------------------------
    // lua option parsing helpers
    // -------------------------------------------------------------------------

    // extract string option from an optional table argument.
    std::string GetStringOption(lua_State *L, int table_index, const char *key, const char *default_value)
    {
        if (table_index == 0)
            return default_value ? default_value : "";

        lua_getfield(L, table_index, key);
        std::string value = default_value ? default_value : "";
        if (lua_isstring(L, -1))
        {
            size_t length = 0;
            const char *text = lua_tolstring(L, -1, &length);
            value.assign(text, length);
        }
        lua_pop(L, 1);
        return value;
    }

    // extract numeric size option from an optional table argument.
    uint32_t GetSizeOption(lua_State *L, int table_index, const char *key, uint32_t default_value)
    {
        if (table_index == 0)
            return default_value;

        lua_getfield(L, table_index, key);
        uint32_t value = default_value;
        if (lua_isnumber(L, -1))
        {
            const lua_Number number = lua_tonumber(L, -1);
            if (number > 0)
                value = static_cast<uint32_t>(number);
        }
        lua_pop(L, 1);
        return value;
    }

    // parse dynamic api signature: function(callback), function(nil, callback),
    // or function(options, callback). first_argument is the options/callback slot.
    void ReadOptionsAndCallback(lua_State *L, int first_argument, int *options_index, int *callback_index)
    {
        if (lua_isfunction(L, first_argument))
        {
            *options_index = 0;
            *callback_index = first_argument;
            return;
        }

        if (lua_isnoneornil(L, first_argument))
        {
            luaL_checktype(L, first_argument + 1, LUA_TFUNCTION);
            *options_index = 0;
            *callback_index = first_argument + 1;
            return;
        }

        luaL_checktype(L, first_argument, LUA_TTABLE);
        luaL_checktype(L, first_argument + 1, LUA_TFUNCTION);
        *options_index = first_argument;
        *callback_index = first_argument + 1;
    }

    // -------------------------------------------------------------------------
    // lua api binding implementations
    // -------------------------------------------------------------------------

    // browser_io.is_supported() -> returns true on html5 platform
    int LuaIsSupported(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);
#if defined(DM_PLATFORM_HTML5)
        lua_pushboolean(L, 1);
#else
        lua_pushboolean(L, 0);
#endif
        return 1;
    }

    // browser_io.copy(text, callback)
    int LuaCopy(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        size_t text_length = 0;
        const char *text = luaL_checklstring(L, 1, &text_length);
        const uint32_t request_id = AddRequest(L, 2);

#if defined(DM_PLATFORM_HTML5)
        BrowserIOJS_Copy(request_id, text, static_cast<uint32_t>(text_length), BrowserIO_OnResult);
#else
        (void)text;
        QueueUnsupported(request_id, OP_COPY);
#endif

        lua_pushinteger(L, static_cast<lua_Integer>(request_id));
        return 1;
    }

    // browser_io.paste(callback)
    int LuaPaste(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        const uint32_t request_id = AddRequest(L, 1);

#if defined(DM_PLATFORM_HTML5)
        BrowserIOJS_Paste(request_id, BrowserIO_OnResult);
#else
        QueueUnsupported(request_id, OP_PASTE);
#endif

        lua_pushinteger(L, static_cast<lua_Integer>(request_id));
        return 1;
    }

    // browser_io.open_file([options], callback)
    int LuaOpenFile(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        int options_index = 0, callback_index = 0;
        ReadOptionsAndCallback(L, 1, &options_index, &callback_index);

        const std::string accept = GetStringOption(L, options_index, "accept", "");
        const uint32_t max_size = GetSizeOption(L, options_index, "max_size", DEFAULT_MAX_FILE_SIZE);

        const uint32_t request_id = AddRequest(L, callback_index);

#if defined(DM_PLATFORM_HTML5)
        BrowserIOJS_OpenFile(request_id, accept.c_str(), static_cast<uint32_t>(accept.size()), max_size, BrowserIO_OnResult);
#else
        (void)max_size;
        QueueUnsupported(request_id, OP_OPEN_FILE);
#endif

        lua_pushinteger(L, static_cast<lua_Integer>(request_id));
        return 1;
    }

    // browser_io.save_file(filename, data, [options], callback)
    int LuaSaveFile(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);

        size_t filename_length = 0, data_length = 0;
        const char *filename = luaL_checklstring(L, 1, &filename_length);
        const char *data = luaL_checklstring(L, 2, &data_length);

        int options_index = 0, callback_index = 0;
        ReadOptionsAndCallback(L, 3, &options_index, &callback_index);

        const std::string mime_type = GetStringOption(L, options_index, "mime_type", "application/octet-stream");
        const uint32_t request_id = AddRequest(L, callback_index);

#if defined(DM_PLATFORM_HTML5)
        BrowserIOJS_SaveFile(request_id,
                             filename, static_cast<uint32_t>(filename_length),
                             reinterpret_cast<const uint8_t *>(data), static_cast<uint32_t>(data_length),
                             mime_type.c_str(), static_cast<uint32_t>(mime_type.size()),
                             BrowserIO_OnResult);
#else
        (void)filename;
        (void)data;
        QueueUnsupported(request_id, OP_SAVE_FILE);
#endif

        lua_pushinteger(L, static_cast<lua_Integer>(request_id));
        return 1;
    }

    // browser_io.set_drop_listener([options], callback)
    int LuaSetDropListener(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);

#if defined(DM_PLATFORM_HTML5)
        int options_index = 0, callback_index = 0;
        ReadOptionsAndCallback(L, 1, &options_index, &callback_index);

        dmScript::LuaCallbackInfo *callback = dmScript::CreateCallback(L, callback_index);
        if (!callback)
            return luaL_error(L, "Unable to create browser_io drop callback");

        const std::string accept = GetStringOption(L, options_index, "accept", "");
        const uint32_t max_size = GetSizeOption(L, options_index, "max_size", DEFAULT_MAX_FILE_SIZE);

        SetDropCallback(callback);
        BrowserIOJS_SetDropEnabled(1, accept.c_str(), static_cast<uint32_t>(accept.size()), max_size, BrowserIO_OnResult);

        lua_pushboolean(L, 1);
#else
        lua_pushboolean(L, 0);
#endif
        return 1;
    }

    // browser_io.clear_drop_listener()
    int LuaCancelDropListener(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 0);

        SetDropCallback(nullptr);

#if defined(DM_PLATFORM_HTML5)
        BrowserIOJS_SetDropEnabled(0, "", 0, 0, BrowserIO_OnResult);
#endif
        return 0;
    }

    int LuaCanCopy(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);
#if defined(DM_PLATFORM_HTML5)
        lua_pushboolean(L, BrowserIOJS_CanCopy());
#else
        lua_pushboolean(L, 0);
#endif
        return 1;
    }

    int LuaCanPaste(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);
#if defined(DM_PLATFORM_HTML5)
        lua_pushboolean(L, BrowserIOJS_CanPaste());
#else
        lua_pushboolean(L, 0);
#endif
        return 1;
    }

    int LuaCanOpenFile(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);
#if defined(DM_PLATFORM_HTML5)
        lua_pushboolean(L, BrowserIOJS_CanOpenFile());
#else
        lua_pushboolean(L, 0);
#endif
        return 1;
    }

    int LuaCanSaveFile(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);
#if defined(DM_PLATFORM_HTML5)
        lua_pushboolean(L, BrowserIOJS_CanSaveFile());
#else
        lua_pushboolean(L, 0);
#endif
        return 1;
    }

    int LuaCanDropFiles(lua_State *L)
    {
        DM_LUA_STACK_CHECK(L, 1);
#if defined(DM_PLATFORM_HTML5)
        lua_pushboolean(L, BrowserIOJS_CanDropFiles());
#else
        lua_pushboolean(L, 0);
#endif
        return 1;
    }

    // expose functions to lua
    const luaL_reg ModuleMethods[] =
        {
            {"is_supported", LuaIsSupported},
            {"can_copy", LuaCanCopy},
            {"can_paste", LuaCanPaste},
            {"can_open_file", LuaCanOpenFile},
            {"can_save_file", LuaCanSaveFile},
            {"can_drop_files", LuaCanDropFiles},
            {"copy", LuaCopy},
            {"paste", LuaPaste},
            {"open_file", LuaOpenFile},
            {"save_file", LuaSaveFile},
            {"set_drop_listener", LuaSetDropListener},
            {"cancel_drop_listener", LuaCancelDropListener},
            {nullptr, nullptr}};

    void RegisterLuaModule(lua_State *L)
    {
        const int top = lua_gettop(L);
        luaL_register(L, MODULE_NAME, ModuleMethods);
        lua_pop(L, 1);
        assert(top == lua_gettop(L));
    }

    dmExtension::Result ExtensionInitialize(dmExtension::Params *params)
    {
        RegisterLuaModule(params->m_L);
        return dmExtension::RESULT_OK;
    }

    dmExtension::Result ExtensionUpdate(dmExtension::Params *params)
    {
        DispatchQueuedEvents();
        return dmExtension::RESULT_OK;
    }

    dmExtension::Result ExtensionFinalize(dmExtension::Params *params)
    {
#if defined(DM_PLATFORM_HTML5)
        BrowserIOJS_SetDropEnabled(0, "", 0, 0, BrowserIO_OnResult);
#endif
        DestroyAllCallbacks();
        return dmExtension::RESULT_OK;
    }
}

DM_DECLARE_EXTENSION(browser_io, LIB_NAME, 0, 0, ExtensionInitialize, ExtensionUpdate, 0, ExtensionFinalize)