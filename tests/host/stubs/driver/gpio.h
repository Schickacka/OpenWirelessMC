#pragma once
#include <stdint.h>
#define G(n) GPIO_NUM_##n = n
typedef enum { G(0),G(1),G(2),G(3),G(4),G(5),G(12),G(13),G(14),G(15),G(16),G(17),G(18),G(19),G(21),G(22),G(23),G(25),G(26),G(27),G(32),G(33),G(34),G(35),G(36),G(39) } gpio_num_t;
static inline int gpio_set_level(gpio_num_t, uint32_t){ return 0; }
