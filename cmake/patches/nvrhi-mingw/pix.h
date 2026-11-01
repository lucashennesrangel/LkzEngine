#pragma once

// Marcadores PIX mínimos para compilar o NVRHI com MinGW: o pix.h do Windows SDK não vem
// no mingw-w64. Mesmo formato do pix3 legado (texto ANSI), que PIX e RenderDoc mostram.
template <class CommandList>
inline void PIXBeginEvent(CommandList& list, unsigned long long /*color*/, const char* name) {
    unsigned size = 1;
    while (name[size - 1] != '\0') {
        ++size;
    }
    list->BeginEvent(1, name, size);
}

template <class CommandList>
inline void PIXEndEvent(CommandList& list) {
    list->EndEvent();
}
