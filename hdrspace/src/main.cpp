#include "native/NativeApp.h"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    try {
        return runHdrspaceNativeApp(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
