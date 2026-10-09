#pragma once

#include <browser_io_private.h>

#if defined(DM_PLATFORM_HTML5)

extern "C"
{
    // tests if the browser supports copy/paste and file open/save dialogs.
    int BrowserIOJS_CanCopy();
    int BrowserIOJS_CanPaste();
    int BrowserIOJS_CanOpenFile();
    int BrowserIOJS_CanSaveFile();
    int BrowserIOJS_CanDropFiles();
    
    // copies text to system clipboard via Emscripten JS library. 
    void BrowserIOJS_Copy(uint32_t request_id, const char* text, uint32_t text_len, dmBrowserIO::BrowserResultCallback callback);

    // Pastes text from system clipboard via Emscripten JS library.
    void BrowserIOJS_Paste(uint32_t request_id, dmBrowserIO::BrowserResultCallback callback);

    // triggers a browser file open dialog.
    void BrowserIOJS_OpenFile(uint32_t request_id, const char* accept, uint32_t accept_len, uint32_t max_size, dmBrowserIO::BrowserResultCallback callback);

    // triggers a browser file save/download dialog. 
    void BrowserIOJS_SaveFile(uint32_t request_id, const char* filename,  uint32_t filename_len, const uint8_t* data, uint32_t data_len, const char* mime_type, uint32_t mime_type_len, dmBrowserIO::BrowserResultCallback callback);

    // enables or disables browser drag-and-drop file listener.
    void BrowserIOJS_SetDropEnabled(int enabled, const char* accept, uint32_t accept_len, uint32_t max_size, dmBrowserIO::BrowserResultCallback callback);
}

#endif