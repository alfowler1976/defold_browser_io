mergeInto(LibraryManager.library, {
    $BrowserIOBridge__deps: ['malloc', 'free'],
    $BrowserIOBridge: {
        // operation codes matching c++ enum values
        OP_COPY: 1,
        OP_PASTE: 2,
        OP_OPEN_FILE: 3,
        OP_SAVE_FILE: 4,
        OP_DROP_FILE: 5,

        // status flags matching c++ response codes
        STATUS_ERROR: 0,
        STATUS_OK: 1,
        STATUS_CANCELLED: 2,

        // persistent drop handler references for lifecycle management
        dropTarget: null,
        dropEnterHandler: null,
        dropOverHandler: null,
        dropHandler: null,

        // converts input values to a uint8array byte buffer
        toBytes: function(value) {
            if (!value) {
                return new Uint8Array(0);
            }
            if (value instanceof Uint8Array) {
                return value;
            }
            if (value instanceof ArrayBuffer) {
                return new Uint8Array(value);
            }
            return new TextEncoder().encode(String(value));
        },

        // allocates heap memory in emscripten runtime for passing byte arrays to c++
        allocBytes: function(value) {
            var bytes = BrowserIOBridge.toBytes(value);
            if (bytes.length === 0) {
                return { ptr: 0, len: 0 };
            }

            var ptr = _malloc(bytes.length);
            HEAPU8.set(bytes, ptr);
            return { ptr: ptr, len: bytes.length };
        },

        // marshals results and dispatches callback to c++ native extension
        complete: function(callback, requestId, operation, status, data, name, error) {
            var dataMem = BrowserIOBridge.allocBytes(data);
            var nameMem = BrowserIOBridge.allocBytes(name);
            var errorMem = BrowserIOBridge.allocBytes(error);

            try {
                {{{ makeDynCall('viiiiiiiii', 'callback') }}}(
                    requestId,
                    operation,
                    status,
                    dataMem.ptr,
                    dataMem.len,
                    nameMem.ptr,
                    nameMem.len,
                    errorMem.ptr,
                    errorMem.len
                );
            } finally {
                if (dataMem.ptr) _free(dataMem.ptr);
                if (nameMem.ptr) _free(nameMem.ptr);
                if (errorMem.ptr) _free(errorMem.ptr);
            }
        },

        // extracts standard error message string or provides fallback text
        errorText: function(error, fallback) {
            if (error && error.message) {
                return String(error.message);
            }
            if (error) {
                return String(error);
            }
            return fallback || 'browser operation failed';
        },

        // returns canvas DOM element from emscripten module or document query
        getCanvas: function() {
            if (typeof Module !== 'undefined' && Module['canvas']) {
                return Module['canvas'];
            }
            return document.getElementById('canvas') || document.querySelector('canvas');
        },

        // validates file against accept filter rules (e.g. extension or mime type)
        matchesAccept: function(file, accept) {
            if (!accept) {
                return true;
            }

            var rules = accept.split(',');
            var name = (file.name || '').toLowerCase();
            var type = (file.type || '').toLowerCase();

            for (var i = 0; i < rules.length; ++i) {
                var rule = rules[i].trim().toLowerCase();
                if (!rule) continue;

                if (rule.charAt(0) === '.') {
                    if (name.endsWith(rule)) return true;
                } else if (rule.endsWith('/*')) {
                    var prefix = rule.substring(0, rule.length - 1);
                    if (type.indexOf(prefix) === 0) return true;
                } else if (type === rule) {
                    return true;
                }
            }

            return false;
        },

        // reads file content via filereader and dispatches buffer to c++
        readFile: function(file, maxSize, callback, requestId, operation) {
            if (!file) {
                BrowserIOBridge.complete(callback, requestId, operation,
                    BrowserIOBridge.STATUS_ERROR, null, null, 'no file selected');
                return;
            }

            if (maxSize > 0 && file.size > maxSize) {
                BrowserIOBridge.complete(callback, requestId, operation,
                    BrowserIOBridge.STATUS_ERROR, null, file.name,
                    'file is too large (' + file.size + ' bytes; maximum is ' + maxSize + ')');
                return;
            }

            var reader = new FileReader();

            reader.onload = function(event) {
                BrowserIOBridge.complete(callback, requestId, operation,
                    BrowserIOBridge.STATUS_OK,
                    new Uint8Array(event.target.result),
                    file.name,
                    null);
            };

            reader.onerror = function() {
                BrowserIOBridge.complete(callback, requestId, operation,
                    BrowserIOBridge.STATUS_ERROR,
                    null,
                    file.name,
                    BrowserIOBridge.errorText(reader.error, 'unable to read file'));
            };

            reader.onabort = function() {
                BrowserIOBridge.complete(callback, requestId, operation,
                    BrowserIOBridge.STATUS_CANCELLED, null, file.name,
                    'file reading was aborted');
            };

            reader.readAsArrayBuffer(file);
        },

        // detaches global drag and drop event listeners from window target
        removeDropHandlers: function() {
            var target = BrowserIOBridge.dropTarget;

            if (target) {
                if (BrowserIOBridge.dropEnterHandler) {
                    target.removeEventListener('dragenter', BrowserIOBridge.dropEnterHandler, true);
                }
                if (BrowserIOBridge.dropOverHandler) {
                    target.removeEventListener('dragover', BrowserIOBridge.dropOverHandler, true);
                }
                if (BrowserIOBridge.dropHandler) {
                    target.removeEventListener('drop', BrowserIOBridge.dropHandler, true);
                }
            }

            BrowserIOBridge.dropTarget = null;
            BrowserIOBridge.dropEnterHandler = null;
            BrowserIOBridge.dropOverHandler = null;
            BrowserIOBridge.dropHandler = null;
        }
    },

    // checks clipboard write availability in secure context
    BrowserIOJS_CanCopy: function() {
        return (typeof window !== 'undefined' &&
                window.isSecureContext === true &&
                typeof navigator !== 'undefined' &&
                navigator.clipboard &&
                typeof navigator.clipboard.writeText === 'function') ? 1 : 0;
    },

    // checks clipboard read availability in secure context
    BrowserIOJS_CanPaste: function() {
        return (typeof window !== 'undefined' &&
                window.isSecureContext === true &&
                typeof navigator !== 'undefined' &&
                navigator.clipboard &&
                typeof navigator.clipboard.readText === 'function') ? 1 : 0;
    },

    // checks input element and filereader DOM support
    BrowserIOJS_CanOpenFile: function() {
        return (typeof document !== 'undefined' &&
                typeof FileReader !== 'undefined' &&
                typeof document.createElement === 'function') ? 1 : 0;
    },

    // checks blob, url, and download anchor element support
    BrowserIOJS_CanSaveFile: function() {
        if (typeof document === 'undefined' ||
            typeof Blob === 'undefined' ||
            typeof URL === 'undefined' ||
            typeof URL.createObjectURL !== 'function') {
            return 0;
        }

        var anchor = document.createElement('a');
        return ('download' in anchor) ? 1 : 0;
    },

    // checks window drag and drop event support
    BrowserIOJS_CanDropFiles: function() {
        if (typeof window === 'undefined' ||
            typeof document === 'undefined' ||
            typeof FileReader === 'undefined') {
            return 0;
        }

        var element = document.createElement('div');
        return ('ondrop' in element && 'ondragover' in element) ? 1 : 0;
    },

    // writes string payload to system clipboard
    BrowserIOJS_Copy__deps: ['$BrowserIOBridge'],
    BrowserIOJS_Copy: function(requestId, textPtr, textLen, callback) {
        var text = UTF8ToString(textPtr, textLen);

        if (!navigator.clipboard || !navigator.clipboard.writeText) {
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_COPY,
                BrowserIOBridge.STATUS_ERROR, null, null, 'clipboard write is not supported by this browser');
            return;
        }

        navigator.clipboard.writeText(text).then(function() {
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_COPY,
                BrowserIOBridge.STATUS_OK, null, null, null);
        }).catch(function(error) {
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_COPY,
                BrowserIOBridge.STATUS_ERROR, null, null,
                BrowserIOBridge.errorText(error, 'clipboard write failed'));
        });
    },

    // reads string payload from system clipboard
    BrowserIOJS_Paste__deps: ['$BrowserIOBridge'],
    BrowserIOJS_Paste: function(requestId, callback) {
        if (!navigator.clipboard || !navigator.clipboard.readText) {
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_PASTE,
                BrowserIOBridge.STATUS_ERROR, null, null, 'clipboard read is not supported by this browser');
            return;
        }

        navigator.clipboard.readText().then(function(text) {
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_PASTE,
                BrowserIOBridge.STATUS_OK, text, null, null);
        }).catch(function(error) {
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_PASTE,
                BrowserIOBridge.STATUS_ERROR, null, null,
                BrowserIOBridge.errorText(error, 'clipboard read failed'));
        });
    },

    // opens system file selection dialog via hidden input element
    BrowserIOJS_OpenFile__deps: ['$BrowserIOBridge'],
    BrowserIOJS_OpenFile: function(requestId, acceptPtr, acceptLen, maxSize, callback) {
        var accept = acceptLen > 0 ? UTF8ToString(acceptPtr, acceptLen) : '';
        var input = document.createElement('input');
        var finished = false;

        input.type = 'file';
        input.style.display = 'none';
        if (accept) input.accept = accept;

        function cleanup() {
            if (input.parentNode) input.parentNode.removeChild(input);
        }

        function cancel() {
            if (finished) return;
            finished = true;
            cleanup();
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_OPEN_FILE,
                BrowserIOBridge.STATUS_CANCELLED, null, null, null);
        }

        input.addEventListener('change', function() {
            if (finished) return;
            if (!input.files || input.files.length === 0) {
                cancel();
                return;
            }

            finished = true;
            var file = input.files[0];
            cleanup();

            if (!BrowserIOBridge.matchesAccept(file, accept)) {
                BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_OPEN_FILE,
                    BrowserIOBridge.STATUS_ERROR, null, file.name,
                    'selected file does not match the accepted file type');
                return;
            }

            BrowserIOBridge.readFile(file, maxSize, callback, requestId, BrowserIOBridge.OP_OPEN_FILE);
        });

        input.addEventListener('cancel', cancel);
        document.body.appendChild(input);

        try {
            input.click();
        } catch (error) {
            if (!finished) {
                finished = true;
                cleanup();
                BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_OPEN_FILE,
                    BrowserIOBridge.STATUS_ERROR, null, null,
                    BrowserIOBridge.errorText(error, 'unable to open file picker'));
            }
        }
    },

    // triggers browser file download using blob object url
    BrowserIOJS_SaveFile__deps: ['$BrowserIOBridge'],
    BrowserIOJS_SaveFile: function(requestId, filenamePtr, filenameLen, dataPtr, dataLen, mimePtr, mimeLen, callback) {
        try {
            var filename = UTF8ToString(filenamePtr, filenameLen);
            var mimeType = mimeLen > 0 ? UTF8ToString(mimePtr, mimeLen) : 'application/octet-stream';
            
            // Use subarray for zero-copy memory mapping to avoid WebAssembly OOM on mobile
            var bytes = HEAPU8.subarray(dataPtr, dataPtr + dataLen);
            var blob = new Blob([bytes], { type: mimeType });
            var url = URL.createObjectURL(blob);
            var anchor = document.createElement('a');

            anchor.href = url;
            anchor.download = filename;
            anchor.style.display = 'none';
            document.body.appendChild(anchor);
            anchor.click();
            document.body.removeChild(anchor);

            // Extended timeout for iOS Safari prompts
            setTimeout(function() {
                URL.revokeObjectURL(url);
            }, 60000);

            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_SAVE_FILE,
                BrowserIOBridge.STATUS_OK, null, null, null);
        } catch (error) {
            BrowserIOBridge.complete(callback, requestId, BrowserIOBridge.OP_SAVE_FILE,
                BrowserIOBridge.STATUS_ERROR, null, null,
                BrowserIOBridge.errorText(error, 'unable to create download'));
        }
    },

    // configures global window drag and drop event capture listeners
    BrowserIOJS_SetDropEnabled__deps: ['$BrowserIOBridge'],
    BrowserIOJS_SetDropEnabled: function(enabled, acceptPtr, acceptLen, maxSize, callback) {
        BrowserIOBridge.removeDropHandlers();

        if (!enabled) {
            return;
        }

        // window capture target prevents browser navigation when dropping files
        var target = window;
        var accept = acceptLen > 0 ? UTF8ToString(acceptPtr, acceptLen) : '';

        BrowserIOBridge.dropTarget = target;

        // dragenter event handler
        BrowserIOBridge.dropEnterHandler = function(event) {
            if (event.dataTransfer) {
                event.dataTransfer.dropEffect = 'copy';
            }
            event.preventDefault();
            event.stopPropagation();
        };

        // dragover event handler
        BrowserIOBridge.dropOverHandler = function(event) {
            if (event.dataTransfer) {
                event.dataTransfer.dropEffect = 'copy';
            }
            event.preventDefault();
            event.stopPropagation();
        };

        // drop event handler
        BrowserIOBridge.dropHandler = function(event) {
            event.preventDefault();
            event.stopPropagation();

            var files = event.dataTransfer && event.dataTransfer.files;
            if (!files || files.length === 0) {
                BrowserIOBridge.complete(callback, 0, BrowserIOBridge.OP_DROP_FILE,
                    BrowserIOBridge.STATUS_ERROR, null, null, 'no file was dropped');
                return;
            }

            var file = files[0];

            if (!BrowserIOBridge.matchesAccept(file, accept)) {
                BrowserIOBridge.complete(callback, 0, BrowserIOBridge.OP_DROP_FILE,
                    BrowserIOBridge.STATUS_ERROR, null, file.name,
                    'dropped file does not match the accepted file type');
                return;
            }

            BrowserIOBridge.readFile(file, maxSize, callback, 0, BrowserIOBridge.OP_DROP_FILE);
        };

        // attach capture listeners on window to override browser defaults
        target.addEventListener('dragenter', BrowserIOBridge.dropEnterHandler, true);
        target.addEventListener('dragover', BrowserIOBridge.dropOverHandler, true);
        target.addEventListener('drop', BrowserIOBridge.dropHandler, true);
    }
});