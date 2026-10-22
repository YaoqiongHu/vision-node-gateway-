/**
 * @file    stm32f407.ld
 * @brief   STM32F407VGT6 链接脚本（GCC / ld）
 *
 * 存储器布局（RM0090 第 2 章）：
 *   FLASH   1024 KiB @ 0x0800_0000
 *   SRAM1    112 KiB @ 0x2000_0000  ┐ 物理连续，合起来 128 KiB，
 *   SRAM2     16 KiB @ 0x2001_C000  ┘ DMA 都能访问 → 合成一个 RAM 区
 *   CCMRAM    64 KiB @ 0x1000_0000     只有 CPU 能访问，DMA 不可达
 *
 * 为什么把 SRAM1/SRAM2 合成一个 RAM 区：两者物理连续、属性完全相同，
 * 拆成两个区唯一的实际效果是给"哪个变量落在哪"多造一个出错机会。
 * 真正需要单独对待的只有 CCM——它 DMA 到不了，所以用 .ccmram 显式放
 * FreeRTOS 的堆和任务栈（只给 CPU 用，正好避让 DMA 缓冲区）。
 *
 * 段符号（startup_stm32f407xx.s 依赖，改名会链接失败）：
 *   _estack / _sidata / _sdata / _edata / _sbss / _ebss
 */

ENTRY(Reset_Handler)

/* ---- 最小堆栈预留：只用于链接期做溢出检查，不占实际空间 ---- */
_Min_Stack_Size = 0x1000;   /* 4 KiB：MSP 主栈，中断嵌套都在这上面   */
_Min_Heap_Size  = 0x0200;   /* 512 B：newlib 的 _sbrk 起点，本项目基本不用 malloc */

MEMORY
{
    FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 1024K
    RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
    CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 64K
}

/* 栈顶 = RAM 末尾，MSP 从这里向下生长 */
_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
    /* ---- 向量表必须落在 FLASH 最开头 ---- */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } >FLASH

    /* ---- 代码 ---- */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)          /* ARM/Thumb 互调胶合代码 */
        *(.glue_7t)
        *(.eh_frame)

        KEEP(*(.init))
        KEEP(*(.fini))

        . = ALIGN(4);
        _etext = .;
    } >FLASH

    /* ---- 只读数据（含常量表、字符串） ---- */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } >FLASH

    /* ---- 异常展开表（-fno-exceptions 时为空，留着不花钱） ---- */
    .ARM.extab :
    {
        . = ALIGN(4);
        *(.ARM.extab* .gnu.linkonce.armextab.*)
        . = ALIGN(4);
    } >FLASH
    .ARM :
    {
        . = ALIGN(4);
        __exidx_start = .;
        *(.ARM.exidx*)
        __exidx_end = .;
        . = ALIGN(4);
    } >FLASH

    /* ---- C 库构造/析构段：__libc_init_array 靠它们工作 ---- */
    .preinit_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
        . = ALIGN(4);
    } >FLASH
    .init_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
        . = ALIGN(4);
    } >FLASH
    .fini_array :
    {
        . = ALIGN(4);
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
        . = ALIGN(4);
    } >FLASH

    /* ---- 已初始化变量：VMA 在 RAM，LMA 在 FLASH，启动时拷贝 ---- */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } >RAM AT> FLASH
    _sidata = LOADADDR(.data);

    /* ---- 未初始化变量：启动时清零 ---- */
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } >RAM

    /* ---- CCM：只有 CPU 能访问。放 RTOS 堆/任务栈，DMA 缓冲区绝对不能进这里 ----
     * 带 AT> FLASH：如果谁在 CCM 里放了带初值的变量，初值由启动代码从 Flash 拷过来
     * （_siccmram → _sccmram.._eccmram），不会静默变成垃圾值。
     * KEEP：放进这个段的东西就是特意要留的，别被 --gc-sections 当成死代码丢掉。 */
    .ccmram :
    {
        . = ALIGN(4);
        _sccmram = .;
        KEEP(*(.ccmram))
        KEEP(*(.ccmram*))
        . = ALIGN(4);
        _eccmram = .;
    } >CCMRAM AT> FLASH
    _siccmram = LOADADDR(.ccmram);

    /* ---- CCM 的 .bss：全是零初始化的东西（堆、任务栈…），不需要占 Flash ----
     * 用 NOLOAD 之后，这些字节只在 CCM 里占位，初值由启动代码清零。
     * 段名故意不叫 .ccmram_bss —— 否则会被上面的 KEEP(*(.ccmram*)) 抓走。 */
    .ccmbss (NOLOAD) :
    {
        . = ALIGN(8);
        _sccmbss = .;
        KEEP(*(.ccmbss))
        KEEP(*(.ccmbss*))
        . = ALIGN(8);
        _eccmbss = .;
    } >CCMRAM

    /* ---- 链接期溢出检查：堆 + 栈 必须还能放得下 ---- */
    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(end  = .);
        PROVIDE(_end = .);
        . = . + _Min_Heap_Size;
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } >RAM

    /* ---- 丢掉用不上的库段，省 Flash ---- */
    /DISCARD/ :
    {
        libc.a (*)
        libm.a (*)
        libgcc.a (*)
    }

    .ARM.attributes 0 : { *(.ARM.attributes) }
}

/* ---- 布局自检：写错一个数就让链接失败，而不是等到板子上跑飞 ---- */
ASSERT(_estack == ORIGIN(RAM) + LENGTH(RAM), "栈顶必须等于 RAM 末尾")
ASSERT(SIZEOF(.isr_vector) == 98 * 4,          "向量表必须是 98 项 × 4 字节")
ASSERT(ADDR(.isr_vector) == ORIGIN(FLASH),     "向量表必须在 FLASH 起始地址")
ASSERT(_edata >= _sdata,                       ".data 段符号顺序异常")
ASSERT(_ebss  >= _sbss,                        ".bss 段符号顺序异常")
