#if defined(__APPLE__)
#define SYM(x) _##x
#else
#define SYM(x) x
#endif

    .section .rodata
    .global SYM(STARTUP_WAV)
    .global SYM(STARTUP_WAV_LEN)
    .global SYM(STARTUP_WEBP)
    .global SYM(STARTUP_WEBP_LEN)

    .align 4
SYM(STARTUP_WAV):
    .incbin "startup.wav"
SYM(STARTUP_WAV_END):

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
    .align 4
SYM(STARTUP_WAV_LEN):
    .long SYM(STARTUP_WAV_END) - SYM(STARTUP_WAV)
#else
    .align 8
SYM(STARTUP_WAV_LEN):
    .quad SYM(STARTUP_WAV_END) - SYM(STARTUP_WAV)
#endif

    .align 4
SYM(STARTUP_WEBP):
    .incbin "startup.webp"
SYM(STARTUP_WEBP_END):

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4
    .align 4
SYM(STARTUP_WEBP_LEN):
    .long SYM(STARTUP_WEBP_END) - SYM(STARTUP_WEBP)
#else
    .align 8
SYM(STARTUP_WEBP_LEN):
    .quad SYM(STARTUP_WEBP_END) - SYM(STARTUP_WEBP)
#endif
