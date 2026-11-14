// bionic on Android API < 23 does not export the stdin/stdout/stderr symbols (only __sF; see <stdio.h>).
// But the NDK's prebuilt libc++ (iostream.o, which implements the std::cin/cout/cerr buffers) references these three symbols directly,
// so including <fstream> pulls it into the link → undefined symbol. Define aliases with the legacy bionic names here, pointing at __sF.
#if defined(__ANDROID__) && __ANDROID_API__ < 23
#include <cstdio>

#undef stdin
#undef stdout
#undef stderr

extern "C" FILE __sF[3];

extern "C" {
FILE* stdin  = &__sF[0];
FILE* stdout = &__sF[1];
FILE* stderr = &__sF[2];
}
#endif
