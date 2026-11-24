#include "ui/Application.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <string>

int main(int argc, char **argv) {
    SDL_SetAppMetadata("RevDock", "0.1.0", "com.revdock.app");
    revdock::Application app;
    return app.run(argc, argv);
}
