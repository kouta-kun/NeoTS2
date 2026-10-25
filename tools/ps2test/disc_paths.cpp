// The disc file system (cdrom0:) only finds upper-case names, while the engine's paths keep the case of the files they came from
// (Neighborhoods/N001/...). This upper-cases a cdrom0: path where the C library opens it, so nothing else needs to know.
#include <cstddef>
#include <cstring>

extern "C" int __real__open(const char* path, int flags, int mode);

extern "C" int __wrap__open(const char* path, int flags, int mode) {
    if (std::strncmp(path, "cdrom0:", 7) != 0) {
        return __real__open(path, flags, mode);
    }
    char upper[256];
    std::size_t i = 0;
    for (; i < 7; ++i) {
        upper[i] = path[i];  // the device name stays as it is
    }
    for (; path[i] != '\0' && i + 1 < sizeof upper; ++i) {
        const char c = path[i];
        upper[i] = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    }
    upper[i] = '\0';
    return __real__open(upper, flags, mode);
}
