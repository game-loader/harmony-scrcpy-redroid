#include <string>

#include <ace/xcomponent/native_interface_xcomponent.h>
#include <napi/native_api.h>

#include "scrcpy_client.h"

namespace {
napi_env g_env = nullptr;
napi_threadsafe_function g_statusFn = nullptr;
float g_surfaceW = 0;
float g_surfaceH = 0;

struct StatusEvent {
    std::string status;
    int width;
    int height;
};

void UpdateSurfaceSize(OH_NativeXComponent *component, void *window) {
    uint64_t w = 0;
    uint64_t h = 0;
    if (OH_NativeXComponent_GetXComponentSize(component, window, &w, &h) == OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        g_surfaceW = static_cast<float>(w);
        g_surfaceH = static_cast<float>(h);
    }
}

void OnSurfaceCreated(OH_NativeXComponent *component, void *window) {
    UpdateSurfaceSize(component, window);
    ScrcpyClient::Instance().SetWindow(static_cast<NativeWindow *>(window));
}

void OnSurfaceChanged(OH_NativeXComponent *component, void *window) { UpdateSurfaceSize(component, window); }

void OnSurfaceDestroyed(OH_NativeXComponent *, void *) {
    ScrcpyClient::Instance().Disconnect();
    ScrcpyClient::Instance().SetWindow(nullptr);
}

void DispatchTouchEvent(OH_NativeXComponent *component, void *window) {
    OH_NativeXComponent_TouchEvent ev;
    if (OH_NativeXComponent_GetTouchEvent(component, window, &ev) != OH_NATIVEXCOMPONENT_RESULT_SUCCESS) {
        return;
    }
    int action;
    switch (ev.type) {
        case OH_NATIVEXCOMPONENT_DOWN: action = 0; break;
        case OH_NATIVEXCOMPONENT_UP: action = 1; break;
        case OH_NATIVEXCOMPONENT_MOVE: action = 2; break;
        case OH_NATIVEXCOMPONENT_CANCEL: action = 3; break;
        default: return;
    }
    if (action == 2) {
        // A move event carries every pointer; forward each so multi-touch gestures work.
        for (uint32_t i = 0; i < ev.numPoints; ++i) {
            const auto &p = ev.touchPoints[i];
            ScrcpyClient::Instance().SendTouch(2, p.id, p.x, p.y, g_surfaceW, g_surfaceH);
        }
        return;
    }
    ScrcpyClient::Instance().SendTouch(action, ev.id, ev.x, ev.y, g_surfaceW, g_surfaceH);
}

OH_NativeXComponent_Callback g_callback = {OnSurfaceCreated, OnSurfaceChanged, OnSurfaceDestroyed,
                                           DispatchTouchEvent};

void CallStatusJs(napi_env env, napi_value jsCb, void *, void *data) {
    auto *ev = static_cast<StatusEvent *>(data);
    if (env && jsCb) {
        napi_value args[3];
        napi_create_string_utf8(env, ev->status.c_str(), ev->status.size(), &args[0]);
        napi_create_int32(env, ev->width, &args[1]);
        napi_create_int32(env, ev->height, &args[2]);
        napi_value undefined;
        napi_get_undefined(env, &undefined);
        napi_call_function(env, undefined, jsCb, 3, args, nullptr);
    }
    delete ev;
}

std::string GetString(napi_env env, napi_value v) {
    size_t len = 0;
    napi_get_value_string_utf8(env, v, nullptr, 0, &len);
    std::string s(len, '\0');
    napi_get_value_string_utf8(env, v, s.data(), len + 1, &len);
    return s;
}

// connect(host: string, port: number, onStatus: (status, width, height) => void)
napi_value Connect(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    std::string host = GetString(env, args[0]);
    int32_t port = 0;
    napi_get_value_int32(env, args[1], &port);
    if (g_statusFn) {
        napi_release_threadsafe_function(g_statusFn, napi_tsfn_abort);
        g_statusFn = nullptr;
    }
    napi_value name;
    napi_create_string_utf8(env, "scrcpyStatus", NAPI_AUTO_LENGTH, &name);
    napi_create_threadsafe_function(env, args[2], nullptr, name, 0, 1, nullptr, nullptr, nullptr, CallStatusJs,
                                    &g_statusFn);
    napi_threadsafe_function fn = g_statusFn;
    ScrcpyClient::Instance().Connect(host, port, [fn](const std::string &status, int w, int h) {
        napi_call_threadsafe_function(fn, new StatusEvent{status, w, h}, napi_tsfn_nonblocking);
    });
    return nullptr;
}

napi_value Disconnect(napi_env, napi_callback_info) {
    ScrcpyClient::Instance().Disconnect();
    return nullptr;
}

napi_value SendKey(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value arg;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
    int32_t keycode = 0;
    napi_get_value_int32(env, arg, &keycode);
    ScrcpyClient::Instance().SendKey(keycode);
    return nullptr;
}

napi_value SendText(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value arg;
    napi_get_cb_info(env, info, &argc, &arg, nullptr, nullptr);
    ScrcpyClient::Instance().SendText(GetString(env, arg));
    return nullptr;
}

napi_value RequestResize(napi_env env, napi_callback_info info) {
    size_t argc = 3;
    napi_value args[3];
    napi_get_cb_info(env, info, &argc, args, nullptr, nullptr);
    int32_t v[3] = {0, 0, 0};
    for (size_t i = 0; i < 3 && i < argc; ++i) napi_get_value_int32(env, args[i], &v[i]);
    ScrcpyClient::Instance().RequestResize(v[0], v[1], v[2]);
    return nullptr;
}

void RegisterXComponent(napi_env env, napi_value exports) {
    napi_value obj = nullptr;
    if (napi_get_named_property(env, exports, OH_NATIVE_XCOMPONENT_OBJ, &obj) != napi_ok || !obj) return;
    OH_NativeXComponent *component = nullptr;
    if (napi_unwrap(env, obj, reinterpret_cast<void **>(&component)) != napi_ok || !component) return;
    OH_NativeXComponent_RegisterCallback(component, &g_callback);
}
} // namespace

EXTERN_C_START
static napi_value Init(napi_env env, napi_value exports) {
    g_env = env;
    napi_property_descriptor desc[] = {
        {"connect", nullptr, Connect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"disconnect", nullptr, Disconnect, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendKey", nullptr, SendKey, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"sendText", nullptr, SendText, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"requestResize", nullptr, RequestResize, nullptr, nullptr, nullptr, napi_default, nullptr},
    };
    napi_define_properties(env, exports, sizeof(desc) / sizeof(desc[0]), desc);
    RegisterXComponent(env, exports);
    return exports;
}
EXTERN_C_END

static napi_module scrcpyModule = {
    .nm_version = 1,
    .nm_flags = 0,
    .nm_filename = nullptr,
    .nm_register_func = Init,
    .nm_modname = "scrcpy",
    .nm_priv = nullptr,
    .reserved = {0},
};

extern "C" __attribute__((constructor)) void RegisterScrcpyModule(void) { napi_module_register(&scrcpyModule); }
