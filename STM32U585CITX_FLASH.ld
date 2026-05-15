/*
 * Linker script for the wolfDemo Doom port (STM32U585CIT6).
 *
 * Memory layout matches the existing wolfDemo blinky example:
 *   FLASH 2 MB at 0x08000000
 *   RAM   768 KB at 0x20000000  (main SRAM, no privilege/security split)
 *   SRAM4 16 KB at 0x28000000   (separate AHB, useful for hot small data)
 *
 * Adds a dedicated .wad section at the tail of FLASH for the WHD-compressed
 * Doom WAD blob produced by the build pipeline.
 */

ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);

_Min_Heap_Size  = 0x200;
/* 32 KB stack: PendSV runs music synth + SFX mixer on top of whatever
 * main was using, with mix_chunk holding 3 KB of locals (mix_buf +
 * music_buf at HALF_SAMPLES=512) plus the OPL call chain. */
_Min_Stack_Size = 0x8000;
_heap_limit = _estack - _Min_Stack_Size;

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 768K
  SRAM4 (xrw) : ORIGIN = 0x28000000, LENGTH = 16K
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
}

SECTIONS
{
  .isr_vector :
  {
    KEEP(*(.isr_vector))
  } >FLASH

  .text :
  {
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)

    KEEP (*(.init))
    KEEP (*(.fini))

    _etext = .;
  } >FLASH

  .rodata :
  {
    *(.rodata)
    *(.rodata*)
  } >FLASH

  .ARM.extab (READONLY) :
  {
    *(.ARM.extab* .gnu.linkonce.armextab.*)
  } >FLASH

  .ARM (READONLY) :
  {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array (READONLY) :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array (READONLY) :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array (READONLY) :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /*
   * WAD blob lives at the tail of FLASH so the bootloader / programmer can
   * incrementally rewrite it without reflashing the engine. Aligned to 16
   * to give the engine reader room for misaligned-load tricks.
   */
  .wad (READONLY) :
  {
    . = ALIGN(16);
    _wad_start = .;
    KEEP(*(.wad))
    KEEP(*(.wad*))
    _wad_end = .;
    . = ALIGN(16);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    _sdata = .;
    *(.data)
    *(.data*)
    *(.RamFunc)
    *(.RamFunc*)
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /*
   * Optional separate-bank scratch memory: SRAM4 is on its own AHB and is
   * great for small hot structures (e.g. column drawers' state). Anything
   * marked __attribute__((section(".sram4"))) lands here. Treated as BSS:
   * the startup zero-fill loop clears [_ssram4, _esram4) at reset.
   */
  .sram4 (NOLOAD) :
  {
    . = ALIGN(4);
    _sram4_start = .;
    _ssram4 = .;
    *(.sram4)
    *(.sram4*)
    . = ALIGN(4);
    _esram4 = .;
    _sram4_end = .;
  } >SRAM4

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
