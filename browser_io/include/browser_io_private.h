#pragma once

#include <dmsdk/sdk.h>
#include <stdint.h>

namespace dmBrowserIO
{
    // supported browser file and clipboard operations.
    enum Operation
    {
        OP_COPY      = 1,
        OP_PASTE     = 2,
        OP_OPEN_FILE = 3,
        OP_SAVE_FILE = 4,
        OP_DROP_FILE = 5,
    };

    // status response flags from js bridge operations.
    enum Status
    {
        STATUS_ERROR     = 0,
        STATUS_OK        = 1,
        STATUS_CANCELLED = 2,
    };

    // callback signature used by javascript bridge to pass results back to c++.
    typedef void (*BrowserResultCallback)(uint32_t request_id, int operation, int status, const char* data, uint32_t data_len, const char* name, uint32_t name_len, const char* error, uint32_t error_len);

    // registers a lua callback for async requests (copy, paste, open, save).
    uint32_t AddRequest(lua_State* L, int callback_index);

    // sets or replaces the persistent lua listener for drag-and-drop events.
    void SetDropCallback(dmScript::LuaCallbackInfo* callback);

    // flushes and dispatches queued asynchronous browser events to lua callbacks.
    void DispatchQueuedEvents();

    // cleans up and frees all registered lua callbacks upon extension termination.
    void DestroyAllCallbacks();

    // queues an error response for calls made on non-html5 platforms.
    void QueueUnsupported(uint32_t request_id, Operation operation);

    // c-abi bridge function called directly from javascript upon operation completion.
    extern "C" void BrowserIO_OnResult(uint32_t request_id, int operation, int status, const char* data, uint32_t data_len, const char* name, uint32_t name_len, const char* error, uint32_t error_len);
}