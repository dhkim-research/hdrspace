#include <csignal>
#include "native/NativeApp.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
#if !defined(_WIN32)
    // A write to a helper's closed pipe returns EPIPE (handled where it happens) instead of ending
    // the app with SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);
#endif
    try {
        return runHdrspaceNativeApp(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
