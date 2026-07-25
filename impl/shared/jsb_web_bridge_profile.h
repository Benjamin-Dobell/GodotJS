#ifndef GODOTJS_SHARED_WEB_BRIDGE_PROFILE_H
#define GODOTJS_SHARED_WEB_BRIDGE_PROFILE_H

#include <cstdint>

namespace v8
{
    class Isolate;
}

namespace jsb::impl
{
    // Stable IDs shared with impl/web/bridge/src/monolith.ts.
    enum class WebBridgeOperation : uint8_t
    {
        None = 0,
        FunctionCache,
        StringNameCache,
        JavaScriptCall,
        GodotMethodCall,
        GodotPropertyGet,
        GodotPropertySet,
        GodotSignalGet,
        GodotUtilityCall,
        JSToGodotVariant,
        GodotToJSVariant,
        JSToGodotObject,
        GodotToJSObject,
        ValueWrapperConstruction,
        ClassConstruction,
        Overflow,
        Num,
    };

    constexpr uint32_t WEB_BRIDGE_PROFILE_MAX_OPERATION_DEPTH = 16;

    class WebBridgeProfileScope
    {
    public:
        WebBridgeProfileScope(v8::Isolate* p_isolate, WebBridgeOperation p_operation)
#if JSB_WITH_WEB
            : isolate_(p_isolate), operation_(p_operation)
        {
            if (isolate_ && isolate_->is_web_bridge_profile_enabled())
            {
                generation_ = isolate_->enter_web_bridge_profile_operation(static_cast<uint32_t>(operation_));
            }
        }
#else
        {
            (void) p_isolate;
            (void) p_operation;
        }
#endif

        ~WebBridgeProfileScope()
        {
#if JSB_WITH_WEB
            if (generation_ != 0)
            {
                isolate_->exit_web_bridge_profile_operation(static_cast<uint32_t>(operation_), generation_);
            }
#endif
        }

        WebBridgeProfileScope(const WebBridgeProfileScope&) = delete;
        WebBridgeProfileScope& operator=(const WebBridgeProfileScope&) = delete;
        WebBridgeProfileScope(WebBridgeProfileScope&&) = delete;
        WebBridgeProfileScope& operator=(WebBridgeProfileScope&&) = delete;

    private:
#if JSB_WITH_WEB
        v8::Isolate* isolate_ = nullptr;
        WebBridgeOperation operation_ = WebBridgeOperation::None;
        uint32_t generation_ = 0;
#endif
    };
}

#endif
