#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

/* =============================================================================
 *  newlib 系统调用桩（只用 `--specs=nosys.specs` 时，链接器会抱怨
 *  _close/_fstat/_isatty/_lseek/_read 未实现 —— 那些是 libnosys 的空壳，
 *  这里给正式实现，把警告消干净，行为也明确）。
 *
 *  _write 在 bsp_uart.c 里（串口输出），不在这里重复定义。
 * ========================================================================== */

/* ---- 堆：故意放在 CCM，防止将来 malloc 出来的缓冲区被当成 DMA 目标 ---- */
#define SYS_HEAP_SIZE 4096U

static uint8_t s_heap[SYS_HEAP_SIZE] __attribute__((section(".ccmbss"), used));
static uint8_t *s_heap_end;

void *_sbrk(ptrdiff_t incr)
{
    uint8_t *prev;

    if (s_heap_end == NULL) {
        s_heap_end = s_heap;
    }

    if ((incr < 0) || ((size_t)incr > (SYS_HEAP_SIZE - (size_t)(s_heap_end - s_heap)))) {
        errno = ENOMEM;
        return (void *)-1;
    }

    prev = s_heap_end;
    s_heap_end += incr;
    return prev;
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    errno = ENOSYS;
    return -1;
}

int _close(int file)
{
    (void)file;
    return -1;
}

int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;   /* 当字符设备，printf 才不会去算偏移 */
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

off_t _lseek(int file, off_t offset, int whence)
{
    (void)file;
    (void)offset;
    (void)whence;
    return 0;
}

int _getpid(void)
{
    return 1;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

void _exit(int status)
{
    (void)status;
    for (;;) {
        /* 真停下来，方便调试器看到卡在这里 */
    }
}
