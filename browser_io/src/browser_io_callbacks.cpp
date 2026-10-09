#include <browser_io_private.h>
#include <browser_io_web.h>

#include <string>
#include <vector>

using namespace dmBrowserIO;

// private file-scoped data structures and internal state
namespace
{
    // internal structure mapping unique request ids to active lua callback instances.
    struct Request
    {
        uint32_t m_Id;
        dmScript::LuaCallbackInfo *m_Callback;
    };

    // queued asynchronous payload populated by the javascript bridge.
    struct Event
    {
        uint32_t m_RequestId;
        Operation m_Operation;
        Status m_Status;
        std::string m_Data;
        std::string m_Name;
        std::string m_Error;
    };

    // tracks state and defers modifications for persistent drag-and-drop listener.
    struct DropState
    {
        dmScript::LuaCallbackInfo *m_Callback = nullptr;
        dmScript::LuaCallbackInfo *m_PendingCallback = nullptr;
        bool m_InsideCallback = false;
        bool m_ChangePending = false;
    };

    uint32_t g_NextRequestId = 1;
    std::vector<Request> g_Requests;
    std::vector<Event> g_Events;
    DropState g_Drop;

    // maps operation enum value to string literal for lua result payload table.
    const char *OperationName(Operation operation)
    {
        switch (operation)
        {
        case OP_COPY:
            return "copy";
        case OP_PASTE:
            return "paste";
        case OP_OPEN_FILE:
            return "open_file";
        case OP_SAVE_FILE:
            return "save_file";
        case OP_DROP_FILE:
            return "drop_file";
        default:
            return "unknown";
        }
    }

    // safely destroys and nullifies a defold lua callback object.
    void ReleaseCallback(dmScript::LuaCallbackInfo *&callback)
    {
        if (callback)
        {
            dmScript::DestroyCallback(callback);
            callback = nullptr;
        }
    }

    // generates a sequential unique request id, wrapping around 0 (reserved for drag/drop).
    uint32_t NextRequestId()
    {
        const uint32_t id = g_NextRequestId++;
        if (g_NextRequestId == 0)
            g_NextRequestId = 1;
        return id;
    }

    // locates, removes, and takes ownership of a callback matching the request id.
    dmScript::LuaCallbackInfo *TakeRequest(uint32_t request_id)
    {
        for (auto it = g_Requests.begin(); it != g_Requests.end(); ++it)
        {
            if (it->m_Id == request_id)
            {
                dmScript::LuaCallbackInfo *callback = it->m_Callback;
                g_Requests.erase(it);
                return callback;
            }
        }
        return nullptr;
    }

    // enqueues an asynchronous javascript result onto the main-thread event vector.
    void QueueEvent(uint32_t request_id, Operation operation, Status status,
                    const char *data, uint32_t data_len,
                    const char *name, uint32_t name_len,
                    const char *error, uint32_t error_len)
    {
        Event event;

        event.m_RequestId = request_id;
        event.m_Operation = operation;
        event.m_Status = status;

        if (data && data_len)
            event.m_Data.assign(data, data_len);
        if (name && name_len)
            event.m_Name.assign(name, name_len);
        if (error && error_len)
            event.m_Error.assign(error, error_len);

        g_Events.push_back(event);
    }

    // returns true for successful operations that carry a data payload,
    // including valid empty strings and zero-byte files.
    bool OperationReturnsData(Operation operation)
    {
        return operation == OP_PASTE ||
               operation == OP_OPEN_FILE ||
               operation == OP_DROP_FILE;
    }

    // constructs and pushes a standardized response lua table onto the stack.
    void PushResult(lua_State *L, const Event &event)
    {
        lua_newtable(L);

        lua_pushboolean(L, event.m_Status == STATUS_OK);
        lua_setfield(L, -2, "success");

        lua_pushboolean(L, event.m_Status == STATUS_CANCELLED);
        lua_setfield(L, -2, "cancelled");

        lua_pushstring(L, OperationName(event.m_Operation));
        lua_setfield(L, -2, "operation");

        if (!event.m_Error.empty())
        {
            lua_pushlstring(L, event.m_Error.c_str(), event.m_Error.size());
            lua_setfield(L, -2, "error");
        }

        // Successful paste/open/drop operations always expose data and size,
        // even when the clipboard text or selected file is empty.
        if (event.m_Status == STATUS_OK && OperationReturnsData(event.m_Operation))
        {
            lua_pushlstring(L, event.m_Data.c_str(), event.m_Data.size());
            lua_setfield(L, -2, "data");

            lua_pushinteger(L, static_cast<lua_Integer>(event.m_Data.size()));
            lua_setfield(L, -2, "size");
        }

        if (event.m_Operation == OP_OPEN_FILE || event.m_Operation == OP_DROP_FILE)
        {
            lua_pushlstring(L, event.m_Name.c_str(), event.m_Name.size());
            lua_setfield(L, -2, "name");
        }
    }

    // invokes the target lua callback using defold's script context setup and teardown.
    void CallLua(dmScript::LuaCallbackInfo *callback, const Event &event, bool destroy_after_call)
    {
        if (!callback)
            return;

        if (!dmScript::IsCallbackValid(callback))
        {
            if (destroy_after_call)
                dmScript::DestroyCallback(callback);
            return;
        }

        lua_State *L = dmScript::GetCallbackLuaContext(callback);
        DM_LUA_STACK_CHECK(L, 0);

        if (!dmScript::SetupCallback(callback))
        {
            dmLogError("browser_io: unable to set up lua callback");
            if (destroy_after_call)
                dmScript::DestroyCallback(callback);
            return;
        }

        PushResult(L, event);
        dmScript::PCall(L, 2, 0); // passes (self, result)
        dmScript::TeardownCallback(callback);

        if (destroy_after_call)
            dmScript::DestroyCallback(callback);
    }

    // applies pending drop listener replacement if modified re-entrantly inside a callback.
    void ApplyPendingDropChange()
    {
        if (!g_Drop.m_ChangePending)
            return;

        ReleaseCallback(g_Drop.m_Callback);
        g_Drop.m_Callback = g_Drop.m_PendingCallback;
        g_Drop.m_PendingCallback = nullptr;
        g_Drop.m_ChangePending = false;
    }

    // dispatches file drop event to the persistent drop listener.
    void DispatchDrop(const Event &event)
    {
        if (!g_Drop.m_Callback)
            return;

        if (!dmScript::IsCallbackValid(g_Drop.m_Callback))
        {
            ReleaseCallback(g_Drop.m_Callback);
#if defined(DM_PLATFORM_HTML5)
            BrowserIOJS_SetDropEnabled(0, "", 0, 0, BrowserIO_OnResult);
#endif
            return;
        }

        g_Drop.m_InsideCallback = true;
        CallLua(g_Drop.m_Callback, event, false);
        g_Drop.m_InsideCallback = false;

        ApplyPendingDropChange();
    }

    // routes event execution based on operation type.
    void Dispatch(const Event &event)
    {
        if (event.m_Operation == OP_DROP_FILE)
        {
            DispatchDrop(event);
            return;
        }

        dmScript::LuaCallbackInfo *callback = TakeRequest(event.m_RequestId);
        CallLua(callback, event, true);
    }

    // removes callbacks whose owning script instance has been unloaded.
    // This runs every update so cleanup does not depend on another browser event arriving.
    void SweepInvalidCallbacks()
    {
        for (auto it = g_Requests.begin(); it != g_Requests.end();)
        {
            if (!it->m_Callback || !dmScript::IsCallbackValid(it->m_Callback))
            {
                ReleaseCallback(it->m_Callback);
                it = g_Requests.erase(it);
            }
            else
            {
                ++it;
            }
        }

        if (!g_Drop.m_InsideCallback &&
            g_Drop.m_Callback &&
            !dmScript::IsCallbackValid(g_Drop.m_Callback))
        {
            ReleaseCallback(g_Drop.m_Callback);
#if defined(DM_PLATFORM_HTML5)
            BrowserIOJS_SetDropEnabled(0, "", 0, 0, BrowserIO_OnResult);
#endif
        }
    }
}

// inter-file public definitions exposed via dmBrowserIO namespace
namespace dmBrowserIO
{
    // creates and registers a one-shot lua callback for an asynchronous request.
    uint32_t AddRequest(lua_State *L, int callback_index)
    {
        luaL_checktype(L, callback_index, LUA_TFUNCTION);

        dmScript::LuaCallbackInfo *callback = dmScript::CreateCallback(L, callback_index);
        if (!callback)
        {
            luaL_error(L, "Unable to create browser_io callback");
            return 0;
        }

        Request request;
        request.m_Id = NextRequestId();
        request.m_Callback = callback;
        g_Requests.push_back(request);

        return request.m_Id;
    }

    // global c-abi callback function called directly from emscripten js bridge.
    extern "C" void BrowserIO_OnResult(uint32_t request_id, int operation, int status,
                                       const char *data, uint32_t data_len,
                                       const char *name, uint32_t name_len,
                                       const char *error, uint32_t error_len)
    {
        QueueEvent(request_id,
                   static_cast<Operation>(operation),
                   static_cast<Status>(status),
                   data, data_len,
                   name, name_len,
                   error, error_len);
    }

    // enqueues an error event on non-html5 build targets.
    void QueueUnsupported(uint32_t request_id, Operation operation)
    {
        static const char kMessage[] = "browser_io is only supported on HTML5";
        QueueEvent(request_id, operation, STATUS_ERROR,
                   nullptr, 0, nullptr, 0, kMessage, static_cast<uint32_t>(sizeof(kMessage) - 1));
    }

    // registers or defers updating the persistent drag-and-drop listener.
    void SetDropCallback(dmScript::LuaCallbackInfo *callback)
    {
        if (g_Drop.m_InsideCallback)
        {
            ReleaseCallback(g_Drop.m_PendingCallback);
            g_Drop.m_PendingCallback = callback;
            g_Drop.m_ChangePending = true;
            return;
        }

        ReleaseCallback(g_Drop.m_Callback);
        g_Drop.m_Callback = callback;
    }

    // dispatches all queued asynchronous browser events on engine frame update.
    void DispatchQueuedEvents()
    {
        SweepInvalidCallbacks();

        if (g_Events.empty())
            return;

        std::vector<Event> events;
        events.swap(g_Events);

        for (size_t i = 0; i < events.size(); ++i)
        {
            Dispatch(events[i]);
        }
    }

    // releases all active and pending callback pointers upon extension finalization.
    void DestroyAllCallbacks()
    {
        ReleaseCallback(g_Drop.m_Callback);
        ReleaseCallback(g_Drop.m_PendingCallback);
        g_Drop.m_InsideCallback = false;
        g_Drop.m_ChangePending = false;

        for (size_t i = 0; i < g_Requests.size(); ++i)
        {
            ReleaseCallback(g_Requests[i].m_Callback);
        }

        g_Requests.clear();
        g_Events.clear();
    }
}