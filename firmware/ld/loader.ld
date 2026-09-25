/* Linker script to configure memory regions for nRF52832 (Bare Metal) */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  /* nRF52832: 512KB Flash, 64KB RAM */
  /*FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 0x80000*/ /* Used without Bootloader */
  FLASH (rx) : ORIGIN = 0x19000, LENGTH = 0x5B000  /* For OTA */
  RAM (rwx)  : ORIGIN = 0x20000000, LENGTH = 0x10000
}

SECTIONS
{
    /* Placing the stack at the top of RAM */
    __stack_top__ = ORIGIN(RAM) + LENGTH(RAM);
    __stack_limit__ = __stack_top__ - 2048; /* 2KB Stack */

    /* RAM that survives a reset: neither copied nor zeroed by the startup
     * code (the crash record, see power.c). It sits at the bottom of RAM,
     * which after a reset only the MBR (its first bytes, hence the offset)
     * and a SoftDevice would use; the bootloader keeps its variables above
     * the SoftDevice RAM and its stack at the top, and the SoftDevice only
     * runs in DFU mode. Placed first, so .data follows it. */
    .noinit ORIGIN(RAM) + 0x40 (NOLOAD) :
    {
        __noinit_start__ = .;
        KEEP(*(.noinit*))
        . = ALIGN(4);
        __noinit_end__ = .;
    } > RAM
}

INCLUDE "nrf_common.ld"
