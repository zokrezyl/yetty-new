#ifndef YETTY_CORE_TYPES_H
#define YETTY_CORE_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Get pointer to containing struct from member pointer */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

typedef uint64_t yetty_core_object_id;

#define YETTY_CORE_OBJECT_ID_NONE 0


#ifdef __cplusplus
}
#endif

#endif /* YETTY_CORE_TYPES_H */
