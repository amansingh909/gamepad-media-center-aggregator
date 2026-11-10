// Virtual gamepad loaded into an emulator with DYLD_INSERT_LIBRARIES (see ctl.py).
// SDL only delivers keyboard input to the key window; gamepad input also reaches a
// background window with SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=1. Commands come from
// the FIFO named by GMCA_VPAD, one per line: "button <SDL_GamepadButton> <0|1>" or
// "axis <SDL_GamepadAxis> <-32768..32767>".

#include <SDL3/SDL.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Emulators link SDL statically with the dynamic API: the public symbol may be a
// stub that is not exported, the implementation is always NAME_REAL.
static void *sym(const char *name) {
    void *p = dlsym(RTLD_DEFAULT, name);
    if (!p) {
        char real[128];
        snprintf(real, sizeof(real), "%s_REAL", name);
        p = dlsym(RTLD_DEFAULT, real);
    }
    if (!p) fprintf(stderr, "vpad: missing %s\n", name);
    return p;
}

static void *run(void *arg) {
    const char *path = arg;
    SDL_InitFlags (*was_init)(SDL_InitFlags) = sym("SDL_WasInit");
    SDL_JoystickID (*attach)(const SDL_VirtualJoystickDesc *) = sym("SDL_AttachVirtualJoystick");
    SDL_Joystick *(*open)(SDL_JoystickID) = sym("SDL_OpenJoystick");
    bool (*set_button)(SDL_Joystick *, int, bool) = sym("SDL_SetJoystickVirtualButton");
    bool (*set_axis)(SDL_Joystick *, int, Sint16) = sym("SDL_SetJoystickVirtualAxis");
    const char *(*get_error)(void) = sym("SDL_GetError");
    if (!was_init || !attach || !open || !set_button || !set_axis || !get_error) return NULL;

    while (!(was_init(SDL_INIT_GAMEPAD) & SDL_INIT_GAMEPAD)) usleep(100000);

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "GMCA virtual gamepad";
    SDL_JoystickID id = attach(&desc);
    SDL_Joystick *joy = id ? open(id) : NULL;
    if (!joy) {
        fprintf(stderr, "vpad: attach failed: %s\n", get_error());
        return NULL;
    }
    fprintf(stderr, "vpad: attached joystick %u, reading %s\n", id, path);

    // Opened read-write so the FIFO never reports EOF between two writers.
    FILE *f = fopen(path, "r+");
    if (!f) {
        perror("vpad: fopen");
        return NULL;
    }
    char line[64], kind[16];
    int index, value;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%15s %d %d", kind, &index, &value) != 3) continue;
        if (!strcmp(kind, "button")) set_button(joy, index, value != 0);
        else if (!strcmp(kind, "axis")) set_axis(joy, index, (Sint16)value);
    }
    return NULL;
}

__attribute__((constructor)) static void vpad_init(void) {
    const char *path = getenv("GMCA_VPAD");
    if (!path) return;
    pthread_t t;
    pthread_create(&t, NULL, run, strdup(path));
    pthread_detach(t);
}
