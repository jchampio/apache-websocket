#if !defined(_MOD_WEBSOCKET_HOOK_EXPORT_H_)
#define _MOD_WEBSOCKET_HOOK_EXPORT_H_

#include "websocket_plugin.h"
#include "ap_config.h"

#if defined(__cplusplus)
extern "C"
{
#endif

AP_DECLARE_HOOK(int,websocket_plugin_init,(const char*, struct _WebSocketPlugin**))

#if defined(__cplusplus)
}
#endif

#endif