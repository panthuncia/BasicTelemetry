#pragma once

#include <BasicTelemetry/Telemetry.h>
#include <tracy/Tracy.hpp>

#define BT_ZONE_SCOPE(name) \
    BASIC_TELEMETRY_SCOPE(name); \
    ZoneScopedN(name)

#define BT_ZONE_SCOPE_CATEGORY(name, category) \
    BASIC_TELEMETRY_SCOPE_CATEGORY(name, category); \
    ZoneScopedN(name)

#define BT_ZONE_NAMED(variable, name) \
    BASIC_TELEMETRY_SCOPE(name); \
    ZoneNamedN(variable, name, true)

#define BT_ZONE_TEXT(text, size) \
    do { \
        ::basic_telemetry::AnnotateCurrentScope(std::string_view{ (text), (size) }); \
        ZoneText((text), (size)); \
    } while (false)

#define BT_ZONE_VALUE(value) \
    do { \
        ::basic_telemetry::SetCurrentScopeValue(static_cast<std::uint64_t>(value)); \
        ZoneValue(value); \
    } while (false)

#define BT_ZONE_NAME(text, size) \
    do { \
        ::basic_telemetry::AnnotateCurrentScope(std::string_view{ (text), (size) }); \
        ZoneName((text), (size)); \
    } while (false)

#define BT_ZONE_NAME_NAMED(variable, text, size) \
    do { \
        ::basic_telemetry::AnnotateCurrentScope(std::string_view{ (text), (size) }); \
        ZoneNameV((variable), (text), (size)); \
    } while (false)

#define BT_PLOT(name, value) \
    do { \
        static const ::basic_telemetry::Gauge BASIC_TELEMETRY_JOIN(_btPlot, __LINE__){ name }; \
        BASIC_TELEMETRY_JOIN(_btPlot, __LINE__).Set(static_cast<std::int64_t>(value)); \
        TracyPlot((name), (value)); \
    } while (false)

#define BT_FRAME_MARK() FrameMark
#define BT_FRAME_MARK_NAMED(name) FrameMarkNamed(name)

#define BT_ALLOC_N(pointer, size, name) TracyAllocN(pointer, size, name)
#define BT_FREE_N(pointer, name) TracyFreeN(pointer, name)
#define BT_TRACY_CONNECTED() TracyIsConnected
