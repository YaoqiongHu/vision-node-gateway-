/**
 * @file    startup_stm32f407xx.s
 * @brief   STM32F407 启动文件：向量表 + 复位处理
 *
 * 向量表顺序必须与 RM0090 表 62（"STM32F40x 向量表"）逐条对齐——
 * 少写一条、写反一条，中断就会跳进隔壁的 handler，现场极难查。
 * 这里每个 .word 后面都标了偏移和 IRQ 号，方便逐条核对。
 *
 * 复位流程（顺序有讲究）：
 *   1. 拷 .data    FLASH → SRAM（_sidata → _sdata.._edata）
 *   2. 拷 .ccmram  FLASH → CCM （_siccmram → _sccmram.._eccmram，通常为空）
 *   3. 清 .bss     （_sbss.._ebss）
 *   4. SystemInit  开 FPU + 配 PLL 到 168 MHz
 *   5. __libc_init_array  C 库的构造段（纯 C 时是空跑，留着不花钱）
 *   6. main
 *
 * .data/.bss 在 SystemInit 之前就绪，所以 SystemInit 里可以放心用带初值的全局变量。
 */

    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

/* ========================================================================== */
/*                     向量表（放到 FLASH 最开头）                              */
/* ========================================================================== */
    .section .isr_vector, "a", %progbits
    .type  g_pfnVectors, %object
g_pfnVectors:
    .word  _estack                     /* 0x000 初始栈顶 = RAM 末尾            */
    .word  Reset_Handler               /* 0x004 复位                            */
    .word  NMI_Handler                 /* 0x008 NMI                             */
    .word  HardFault_Handler           /* 0x00C HardFault                       */
    .word  MemManage_Handler           /* 0x010 MemManage                       */
    .word  BusFault_Handler            /* 0x014 BusFault                        */
    .word  UsageFault_Handler          /* 0x018 UsageFault                      */
    .word  0                           /* 0x01C 保留                            */
    .word  0                           /* 0x020 保留                            */
    .word  0                           /* 0x024 保留                            */
    .word  0                           /* 0x028 保留                            */
    .word  SVC_Handler                 /* 0x02C SVCall                          */
    .word  DebugMon_Handler            /* 0x030 Debug Monitor                   */
    .word  0                           /* 0x034 保留                            */
    .word  PendSV_Handler              /* 0x038 PendSV                          */
    .word  SysTick_Handler             /* 0x03C SysTick                         */
/* ------------------------------- 外部中断 ------------------------------- */
    .word  WWDG_IRQHandler             /* 0x040 IRQ  0  WWDG                    */
    .word  PVD_IRQHandler              /* 0x044 IRQ  1  PVD                     */
    .word  TAMP_STAMP_IRQHandler       /* 0x048 IRQ  2  TAMP/STAMP              */
    .word  RTC_WKUP_IRQHandler         /* 0x04C IRQ  3  RTC WakeUp              */
    .word  FLASH_IRQHandler            /* 0x050 IRQ  4  FLASH                   */
    .word  RCC_IRQHandler              /* 0x054 IRQ  5  RCC                     */
    .word  EXTI0_IRQHandler            /* 0x058 IRQ  6  EXTI0                   */
    .word  EXTI1_IRQHandler            /* 0x05C IRQ  7  EXTI1                   */
    .word  EXTI2_IRQHandler            /* 0x060 IRQ  8  EXTI2                   */
    .word  EXTI3_IRQHandler            /* 0x064 IRQ  9  EXTI3                   */
    .word  EXTI4_IRQHandler            /* 0x068 IRQ 10  EXTI4                   */
    .word  DMA1_Stream0_IRQHandler     /* 0x06C IRQ 11  DMA1 Stream0            */
    .word  DMA1_Stream1_IRQHandler     /* 0x070 IRQ 12  DMA1 Stream1            */
    .word  DMA1_Stream2_IRQHandler     /* 0x074 IRQ 13  DMA1 Stream2            */
    .word  DMA1_Stream3_IRQHandler     /* 0x078 IRQ 14  DMA1 Stream3            */
    .word  DMA1_Stream4_IRQHandler     /* 0x07C IRQ 15  DMA1 Stream4            */
    .word  DMA1_Stream5_IRQHandler     /* 0x080 IRQ 16  DMA1 Stream5            */
    .word  DMA1_Stream6_IRQHandler     /* 0x084 IRQ 17  DMA1 Stream6            */
    .word  ADC_IRQHandler              /* 0x088 IRQ 18  ADC1/2/3                */
    .word  CAN1_TX_IRQHandler          /* 0x08C IRQ 19  CAN1 TX                 */
    .word  CAN1_RX0_IRQHandler         /* 0x090 IRQ 20  CAN1 RX0                */
    .word  CAN1_RX1_IRQHandler         /* 0x094 IRQ 21  CAN1 RX1                */
    .word  CAN1_SCE_IRQHandler         /* 0x098 IRQ 22  CAN1 SCE                */
    .word  EXTI9_5_IRQHandler          /* 0x09C IRQ 23  EXTI9_5                 */
    .word  TIM1_BRK_TIM9_IRQHandler    /* 0x0A0 IRQ 24  TIM1 BRK / TIM9         */
    .word  TIM1_UP_TIM10_IRQHandler    /* 0x0A4 IRQ 25  TIM1 UP / TIM10         */
    .word  TIM1_TRG_COM_TIM11_IRQHandler /* 0x0A8 IRQ 26 TIM1 TRG-COM / TIM11   */
    .word  TIM1_CC_IRQHandler          /* 0x0AC IRQ 27  TIM1 CC                 */
    .word  TIM2_IRQHandler             /* 0x0B0 IRQ 28  TIM2                    */
    .word  TIM3_IRQHandler             /* 0x0B4 IRQ 29  TIM3                    */
    .word  TIM4_IRQHandler             /* 0x0B8 IRQ 30  TIM4                    */
    .word  I2C1_EV_IRQHandler          /* 0x0BC IRQ 31  I2C1 EV                 */
    .word  I2C1_ER_IRQHandler          /* 0x0C0 IRQ 32  I2C1 ER                 */
    .word  I2C2_EV_IRQHandler          /* 0x0C4 IRQ 33  I2C2 EV                 */
    .word  I2C2_ER_IRQHandler          /* 0x0C8 IRQ 34  I2C2 ER                 */
    .word  SPI1_IRQHandler             /* 0x0CC IRQ 35  SPI1                    */
    .word  SPI2_IRQHandler             /* 0x0D0 IRQ 36  SPI2                    */
    .word  USART1_IRQHandler           /* 0x0D4 IRQ 37  USART1                  */
    .word  USART2_IRQHandler           /* 0x0D8 IRQ 38  USART2                  */
    .word  USART3_IRQHandler           /* 0x0DC IRQ 39  USART3                  */
    .word  EXTI15_10_IRQHandler        /* 0x0E0 IRQ 40  EXTI15_10               */
    .word  RTC_Alarm_IRQHandler        /* 0x0E4 IRQ 41  RTC Alarm               */
    .word  OTG_FS_WKUP_IRQHandler      /* 0x0E8 IRQ 42  OTG_FS WKUP             */
    .word  TIM8_BRK_TIM12_IRQHandler   /* 0x0EC IRQ 43  TIM8 BRK / TIM12        */
    .word  TIM8_UP_TIM13_IRQHandler    /* 0x0F0 IRQ 44  TIM8 UP / TIM13         */
    .word  TIM8_TRG_COM_TIM14_IRQHandler /* 0x0F4 IRQ 45 TIM8 TRG-COM / TIM14   */
    .word  TIM8_CC_IRQHandler          /* 0x0F8 IRQ 46  TIM8 CC                 */
    .word  DMA1_Stream7_IRQHandler     /* 0x0FC IRQ 47  DMA1 Stream7            */
    .word  FSMC_IRQHandler             /* 0x100 IRQ 48  FSMC                    */
    .word  SDIO_IRQHandler             /* 0x104 IRQ 49  SDIO                    */
    .word  TIM5_IRQHandler             /* 0x108 IRQ 50  TIM5                    */
    .word  SPI3_IRQHandler             /* 0x10C IRQ 51  SPI3                    */
    .word  UART4_IRQHandler            /* 0x110 IRQ 52  UART4                   */
    .word  UART5_IRQHandler            /* 0x114 IRQ 53  UART5                   */
    .word  TIM6_DAC_IRQHandler         /* 0x118 IRQ 54  TIM6 / DAC              */
    .word  TIM7_IRQHandler             /* 0x11C IRQ 55  TIM7                    */
    .word  DMA2_Stream0_IRQHandler     /* 0x120 IRQ 56  DMA2 Stream0            */
    .word  DMA2_Stream1_IRQHandler     /* 0x124 IRQ 57  DMA2 Stream1  ← DCMI 用 */
    .word  DMA2_Stream2_IRQHandler     /* 0x128 IRQ 58  DMA2 Stream2            */
    .word  DMA2_Stream3_IRQHandler     /* 0x12C IRQ 59  DMA2 Stream3            */
    .word  DMA2_Stream4_IRQHandler     /* 0x130 IRQ 60  DMA2 Stream4            */
    .word  ETH_IRQHandler              /* 0x134 IRQ 61  ETH                     */
    .word  ETH_WKUP_IRQHandler         /* 0x138 IRQ 62  ETH WKUP                */
    .word  CAN2_TX_IRQHandler          /* 0x13C IRQ 63  CAN2 TX                 */
    .word  CAN2_RX0_IRQHandler         /* 0x140 IRQ 64  CAN2 RX0                */
    .word  CAN2_RX1_IRQHandler         /* 0x144 IRQ 65  CAN2 RX1                */
    .word  CAN2_SCE_IRQHandler         /* 0x148 IRQ 66  CAN2 SCE                */
    .word  OTG_FS_IRQHandler           /* 0x14C IRQ 67  OTG_FS                  */
    .word  DMA2_Stream5_IRQHandler     /* 0x150 IRQ 68  DMA2 Stream5            */
    .word  DMA2_Stream6_IRQHandler     /* 0x154 IRQ 69  DMA2 Stream6            */
    .word  DMA2_Stream7_IRQHandler     /* 0x158 IRQ 70  DMA2 Stream7  ← UART TX */
    .word  USART6_IRQHandler           /* 0x15C IRQ 71  USART6                  */
    .word  I2C3_EV_IRQHandler          /* 0x160 IRQ 72  I2C3 EV                 */
    .word  I2C3_ER_IRQHandler          /* 0x164 IRQ 73  I2C3 ER                 */
    .word  OTG_HS_EP1_OUT_IRQHandler   /* 0x168 IRQ 74  OTG_HS EP1 OUT          */
    .word  OTG_HS_EP1_IN_IRQHandler    /* 0x16C IRQ 75  OTG_HS EP1 IN           */
    .word  OTG_HS_WKUP_IRQHandler      /* 0x170 IRQ 76  OTG_HS WKUP             */
    .word  OTG_HS_IRQHandler           /* 0x174 IRQ 77  OTG_HS                  */
    .word  DCMI_IRQHandler             /* 0x178 IRQ 78  DCMI                    */
    .word  0                           /* 0x17C IRQ 79  保留（F407 无 CRYP）    */
    .word  HASH_RNG_IRQHandler         /* 0x180 IRQ 80  HASH / RNG              */
    .word  FPU_IRQHandler              /* 0x184 IRQ 81  FPU                     */
    .size  g_pfnVectors, . - g_pfnVectors

/* ========================================================================== */
/*                              复位处理                                       */
/* ========================================================================== */
    .section .text.Reset_Handler, "ax", %progbits
    /* 必须是全局强符号：链接脚本的 ENTRY(Reset_Handler) 靠它找入口。
     * 只写 .type 不写 .global，符号会变成局部符号，ld 只能警告然后自己猜一个地址。 */
    .global Reset_Handler
    .type   Reset_Handler, %function
Reset_Handler:
    /* ---- 1. .data: FLASH(_sidata) → SRAM(_sdata.._edata) ---- */
    ldr    r0, =_sdata
    ldr    r1, =_edata
    ldr    r2, =_sidata
    b      .Lcopy_test
.Lcopy_loop:
    ldr    r3, [r2], #4
    str    r3, [r0], #4
.Lcopy_test:
    cmp    r0, r1
    bcc    .Lcopy_loop

    /* ---- 2. .ccmram: FLASH(_siccmram) → CCM(_sccmram.._eccmram) ----
     * 只有真在 CCM 里放了带初值的变量才需要干活，否则首尾相等、循环空转。 */
    ldr    r0, =_sccmram
    ldr    r1, =_eccmram
    ldr    r2, =_siccmram
    b      .Lccm_test
.Lccm_loop:
    ldr    r3, [r2], #4
    str    r3, [r0], #4
.Lccm_test:
    cmp    r0, r1
    bcc    .Lccm_loop

    /* ---- 3. .bss: 清零 ---- */
    ldr    r0, =_sbss
    ldr    r1, =_ebss
    movs   r3, #0
    b      .Lzero_test
.Lzero_loop:
    str    r3, [r0], #4
.Lzero_test:
    cmp    r0, r1
    bcc    .Lzero_loop

    /* ---- 4. 时钟与 FPU ---- */
    bl     SystemInit

    /* ---- 5. C 库构造段（.init_array / .fini_array） ---- */
    bl     __libc_init_array

    /* ---- 6. 进 main（正常不返回） ---- */
    bl     main
    b      .Lhang

.Lhang:
    b      .Lhang
    .size  Reset_Handler, . - Reset_Handler
    .ltorg

/* ========================================================================== */
/*                        默认 handler + 弱符号                                 */
/* ========================================================================== */
    .section .text.Default_Handler, "ax", %progbits
    .type   Default_Handler, %function
Default_Handler:
    b      Default_Handler
    .size  Default_Handler, . - Default_Handler

/**
 * 每个 handler 都先声明成弱符号并指向 Default_Handler：
 * 谁要用谁在自己的 .c 里定义同名函数即可覆盖，不用改这个文件。
 * 将来 FreeRTOS 接管 SVC/PendSV/SysTick 也是同一机制。
 */
.macro weak_handler name
    .weak   \name
    .thumb_set \name, Default_Handler
.endm

/* ---- 内核异常 ---- */
weak_handler NMI_Handler
weak_handler HardFault_Handler
weak_handler MemManage_Handler
weak_handler BusFault_Handler
weak_handler UsageFault_Handler
weak_handler SVC_Handler
weak_handler DebugMon_Handler
weak_handler PendSV_Handler
weak_handler SysTick_Handler

/* ---- 外部中断（顺序与向量表一致） ---- */
weak_handler WWDG_IRQHandler
weak_handler PVD_IRQHandler
weak_handler TAMP_STAMP_IRQHandler
weak_handler RTC_WKUP_IRQHandler
weak_handler FLASH_IRQHandler
weak_handler RCC_IRQHandler
weak_handler EXTI0_IRQHandler
weak_handler EXTI1_IRQHandler
weak_handler EXTI2_IRQHandler
weak_handler EXTI3_IRQHandler
weak_handler EXTI4_IRQHandler
weak_handler DMA1_Stream0_IRQHandler
weak_handler DMA1_Stream1_IRQHandler
weak_handler DMA1_Stream2_IRQHandler
weak_handler DMA1_Stream3_IRQHandler
weak_handler DMA1_Stream4_IRQHandler
weak_handler DMA1_Stream5_IRQHandler
weak_handler DMA1_Stream6_IRQHandler
weak_handler ADC_IRQHandler
weak_handler CAN1_TX_IRQHandler
weak_handler CAN1_RX0_IRQHandler
weak_handler CAN1_RX1_IRQHandler
weak_handler CAN1_SCE_IRQHandler
weak_handler EXTI9_5_IRQHandler
weak_handler TIM1_BRK_TIM9_IRQHandler
weak_handler TIM1_UP_TIM10_IRQHandler
weak_handler TIM1_TRG_COM_TIM11_IRQHandler
weak_handler TIM1_CC_IRQHandler
weak_handler TIM2_IRQHandler
weak_handler TIM3_IRQHandler
weak_handler TIM4_IRQHandler
weak_handler I2C1_EV_IRQHandler
weak_handler I2C1_ER_IRQHandler
weak_handler I2C2_EV_IRQHandler
weak_handler I2C2_ER_IRQHandler
weak_handler SPI1_IRQHandler
weak_handler SPI2_IRQHandler
weak_handler USART1_IRQHandler
weak_handler USART2_IRQHandler
weak_handler USART3_IRQHandler
weak_handler EXTI15_10_IRQHandler
weak_handler RTC_Alarm_IRQHandler
weak_handler OTG_FS_WKUP_IRQHandler
weak_handler TIM8_BRK_TIM12_IRQHandler
weak_handler TIM8_UP_TIM13_IRQHandler
weak_handler TIM8_TRG_COM_TIM14_IRQHandler
weak_handler TIM8_CC_IRQHandler
weak_handler DMA1_Stream7_IRQHandler
weak_handler FSMC_IRQHandler
weak_handler SDIO_IRQHandler
weak_handler TIM5_IRQHandler
weak_handler SPI3_IRQHandler
weak_handler UART4_IRQHandler
weak_handler UART5_IRQHandler
weak_handler TIM6_DAC_IRQHandler
weak_handler TIM7_IRQHandler
weak_handler DMA2_Stream0_IRQHandler
weak_handler DMA2_Stream1_IRQHandler
weak_handler DMA2_Stream2_IRQHandler
weak_handler DMA2_Stream3_IRQHandler
weak_handler DMA2_Stream4_IRQHandler
weak_handler ETH_IRQHandler
weak_handler ETH_WKUP_IRQHandler
weak_handler CAN2_TX_IRQHandler
weak_handler CAN2_RX0_IRQHandler
weak_handler CAN2_RX1_IRQHandler
weak_handler CAN2_SCE_IRQHandler
weak_handler OTG_FS_IRQHandler
weak_handler DMA2_Stream5_IRQHandler
weak_handler DMA2_Stream6_IRQHandler
weak_handler DMA2_Stream7_IRQHandler
weak_handler USART6_IRQHandler
weak_handler I2C3_EV_IRQHandler
weak_handler I2C3_ER_IRQHandler
weak_handler OTG_HS_EP1_OUT_IRQHandler
weak_handler OTG_HS_EP1_IN_IRQHandler
weak_handler OTG_HS_WKUP_IRQHandler
weak_handler OTG_HS_IRQHandler
weak_handler DCMI_IRQHandler
weak_handler HASH_RNG_IRQHandler
weak_handler FPU_IRQHandler
