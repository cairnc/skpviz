// Android system properties shim — all properties return defaults.
#pragma once
#include <stdbool.h>
#include <string.h>

#define PROPERTY_KEY_MAX 32
#define PROPERTY_VALUE_MAX 92

#ifdef __cplusplus
extern "C"
{
#endif

    static inline int property_get(const char * /*key*/, char *value,
                                   const char *default_value)
    {
        if (default_value && value)
        {
            size_t n = strlen(default_value);
            if (n >= PROPERTY_VALUE_MAX)
                n = PROPERTY_VALUE_MAX - 1;
            memcpy(value, default_value, n);
            value[n] = '\0';
            return (int)n;
        }
        if (value)
            value[0] = '\0';
        return 0;
    }

    static inline int property_set(const char * /*key*/, const char * /*value*/)
    {
        return 0;
    }

    static inline bool property_get_bool(const char * /*key*/,
                                         bool default_value)
    {
        return default_value;
    }
    static inline int32_t property_get_int32(const char * /*key*/,
                                             int32_t default_value)
    {
        return default_value;
    }
    static inline int64_t property_get_int64(const char * /*key*/,
                                             int64_t default_value)
    {
        return default_value;
    }

#ifdef __cplusplus
}
#endif
