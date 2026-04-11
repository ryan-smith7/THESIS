.mbedtls_heap (NOLOAD) :
{
    KEEP(*(.mbedtls_heap))
    KEEP(*(.mbedtls_heap*))
} > dram1_0_seg