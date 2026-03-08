#include <zephyr/kernel.h>

K_MUTEX_DEFINE(i2c_mux);

void i2c_gate_acquire(void) { k_mutex_lock(&i2c_mux, K_FOREVER); }
void i2c_gate_release(void) { k_mutex_unlock(&i2c_mux); }
