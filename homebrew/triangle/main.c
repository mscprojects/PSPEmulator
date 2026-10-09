#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspkernel.h>

PSP_MODULE_INFO("PSPEmulator Triangle", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

static unsigned int list[4096] __attribute__((aligned(16)));
static volatile int done;
static volatile int finishes;

struct Vertex
{
    unsigned int color;
    float x, y, z;
};

static const struct Vertex vertices[3] __attribute__((aligned(16))) = {
    {0xFF0000FF, 240.0f, 40.0f, 0.0f},
    {0xFF00FF00, 80.0f, 232.0f, 0.0f},
    {0xFFFF0000, 400.0f, 232.0f, 0.0f},
};

static int exit_callback(int count, int argument, void *common)
{
    (void)count;
    (void)argument;
    (void)common;
    done = 1;
    return 0;
}

static int callback_thread(SceSize size, void *arguments)
{
    (void)size;
    (void)arguments;
    int callback = sceKernelCreateCallback("Triangle exit", exit_callback, 0);
    sceKernelRegisterExitCallback(callback);
    sceKernelSleepThreadCB();
    return 0;
}

static void finish_callback(int id)
{
    if (id == 7)
        ++finishes;
}

int main(void)
{
    int thread = sceKernelCreateThread("Triangle callbacks", callback_thread, 0x11, 0x1000, 0, 0);
    sceKernelStartThread(thread, 0, 0);
    sceDisplaySetMode(0, 480, 272);
    sceGuInit();
    sceGuSetCallback(GU_CALLBACK_FINISH, finish_callback);
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, 512);
    sceGuDispBuffer(480, 272, (void *)0x88000, 512);
    sceGuScissor(0, 0, 480, 272);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDepthMask(GU_TRUE);
    sceGuClearColor(0);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 3, 0, vertices);
    sceGuFinishId(7);
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    sceGuSwapBuffers();

    // Completion must run the actual guest finish callback, not just satisfy sync.
    while (!finishes)
        sceKernelDelayThread(1000);
    // Keep the image visible and input available until Home invokes exit_callback.
    while (!done)
        sceDisplayWaitVblankStart();

    sceGuTerm();
    sceKernelExitGame();
    return 0;
}
