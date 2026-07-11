// Used as a test when writing PNGVIEW
#include <stdio.h>
#include <sys.h>

main()
{
    unsigned top;
    void *p;

    top = (unsigned)sbrk(0);
    printf("current heap top: %u\n", top);

    p = sbrk(32768);
    if (p == (void *)(-1)) {
        printf("sbrk(32768) FAILED\n");
    } else {
        printf("sbrk(32768) ok, new top: %u\n", (unsigned)sbrk(0));
    }
    return 0;
}
