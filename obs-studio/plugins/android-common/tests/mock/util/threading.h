#pragma once
/* threading.h:45 的 pthread_mutex_init_value 在 POSIX 上是空操作。 */
#ifndef pthread_mutex_init_value
#define pthread_mutex_init_value(m) ((void)0)
#endif
