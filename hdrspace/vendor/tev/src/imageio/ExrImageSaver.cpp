/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/imageio/ExrImageSaver.h>

#include <Iex.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfHeader.h>
#include <ImfIO.h>
#include <ImfInputPart.h>
#include <ImfOutputFile.h>
#include <ImfStandardAttributes.h>
#include <ImfStringAttribute.h>

#include <ostream>
#include <span>
#include <string>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

class StdOStream : public Imf::OStream {
public:
    StdOStream(ostream& stream, const char fileName[]) : Imf::OStream{fileName}, mStream{stream} {}

    void write(const char c[/*n*/], int n) {
        clearError();
        mStream.write(c, n);
        checkError(mStream);
    }

    uint64_t tellp() { return streamoff(mStream.tellp()); }

    void seekp(uint64_t pos) {
        mStream.seekp(pos);
        checkError(mStream);
    }

private:
    // The following error-checking functions were copy&pasted from the OpenEXR source code
    static void clearError() { errno = 0; }

    static void checkError(ostream& os) {
        if (!os) {
            if (errno) {
                IEX_NAMESPACE::throwErrnoExc();
            }

            throw IEX_NAMESPACE::ErrnoExc("File output failed.");
        }
    }

    ostream& mStream;
};

namespace {

void writeExr(
    ostream& oStream, const fs::path& path, span<const float> data, Vector2i imageSize, int nChannels, const ExrAbsoluteAttributes* attributes
) {
    const vector<string> channelNames = {"R", "G", "B", "A"};

    if (nChannels <= 0 || nChannels > 4) {
        throw ImageSaveError{format("Invalid number of channels {}.", nChannels)};
    }

    Imf::Header header{imageSize.x(), imageSize.y()};
    if (attributes) {
        const auto& c = attributes->chromaticities;
        Imf::addChromaticities(
            header, Imf::Chromaticities{Imath::V2f{c[0], c[1]}, Imath::V2f{c[2], c[3]}, Imath::V2f{c[4], c[5]}, Imath::V2f{c[6], c[7]}}
        );
        Imf::addWhiteLuminance(header, attributes->whiteLuminance);
        for (const auto& [key, value] : attributes->strings) {
            header.insert(key.c_str(), Imf::StringAttribute{value});
        }
    }
    Imf::FrameBuffer frameBuffer;

    for (int i = 0; i < nChannels; ++i) {
        header.channels().insert(channelNames[i], Imf::Channel(Imf::FLOAT));
        frameBuffer.insert(
            channelNames[i],
            Imf::Slice(
                Imf::FLOAT,                               // Type
                (char*)(data.data() + i),                 // Base pointer
                sizeof(float) * nChannels,                // x-stride in bytes
                sizeof(float) * imageSize.x() * nChannels // y-stride in bytes
            )
        );
    }

    StdOStream imfOStream{oStream, toString(path).c_str()};
    Imf::OutputFile file{imfOStream, header};
    file.setFrameBuffer(frameBuffer);
    file.writePixels(imageSize.y());
}

} // namespace

Task<void> ExrImageSaver::save(ostream& oStream, const fs::path& path, span<const float> data, Vector2i imageSize, int nChannels) const {
    writeExr(oStream, path, data, imageSize, nChannels, nullptr);
    co_return;
}

void saveExrWithAttributes(
    ostream& oStream, const fs::path& path, span<const float> data, Vector2i imageSize, int nChannels, const ExrAbsoluteAttributes& attributes
) {
    writeExr(oStream, path, data, imageSize, nChannels, &attributes);
}

} // namespace tev
