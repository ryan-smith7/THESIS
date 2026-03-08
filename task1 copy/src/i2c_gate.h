#pragma once
#include <zephyr/kernel.h>

extern struct k_mutex i2c_mux;
void i2c_gate_acquire(void);
void i2c_gate_release(void);