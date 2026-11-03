#include <windows.h>

#include "app.h"

int WINAPI wWinMain(_In_ HINSTANCE instance,
                    _In_opt_ HINSTANCE,
                    _In_ LPWSTR,
                    _In_ int showCommand)
{
    simul::App app;
    return app.run(instance, showCommand);
}
