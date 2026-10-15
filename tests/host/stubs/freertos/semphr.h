#pragma once
#include "FreeRTOS.h"
#include <pthread.h>
typedef pthread_mutex_t* SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(){ auto m=new pthread_mutex_t; pthread_mutexattr_t a; pthread_mutexattr_init(&a); pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE); pthread_mutex_init(m,&a); return m;}
static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t m, uint32_t){ return pthread_mutex_lock(m)==0; }
static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t m){ return pthread_mutex_unlock(m)==0; }
