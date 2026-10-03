#ifndef MERGEHDR_DESKTOP_PLATFORM_TEVIPCCLIENT_H
#define MERGEHDR_DESKTOP_PLATFORM_TEVIPCCLIENT_H

#include <cstdint>
#include <string>

namespace tevipc {

bool sendOpenImage(
    const std::string &host,
    std::uint16_t port,
    const std::string &imagePath,
    const std::string &channelSelector,
    bool grabFocus,
    std::string *errorMessage
);

}

#endif
