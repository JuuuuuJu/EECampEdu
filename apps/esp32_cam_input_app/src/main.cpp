#include "App/App.hpp"

int main(int, char**) {
    App app;
    if (!app.Init()) {
        return 1;
    }

    while (!app.isDone()) {
        app.Update();
        app.Render();
    }

    app.Shutdown();
    return 0;
}
