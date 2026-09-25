#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace VRLoadingScreens::DDSOverlayCodec
{
    // SteamVR's DirectX overlay handoff accepts the conservative texture shape
    // used by its own samples: one uncompressed RGBA8 mip.  Fallout loading art
    // is commonly BGRA8 or BC-compressed, so VR backgrounds are normalized on
    // the existing low-priority prewarm worker before they reach IVROverlay.
    enum class SourceFormat
    {
        kRGBA8,
        kBGRA8,
        kBC1,
        kBC3
    };

    using CancellationCheck = bool(*)(void*);

    inline constexpr std::uint32_t kMaxDecodedDimension = 8192;
    inline constexpr std::size_t kMaxDecodedBytes =
        64ull * 1024ull * 1024ull;

    namespace Detail
    {
        struct Color
        {
            std::uint8_t r;
            std::uint8_t g;
            std::uint8_t b;
            std::uint8_t a;
        };

        constexpr std::uint16_t ReadU16(const std::uint8_t* data)
        {
            return static_cast<std::uint16_t>(data[0]) |
                static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(data[1]) << 8);
        }

        constexpr std::uint32_t ReadU32(const std::uint8_t* data)
        {
            return static_cast<std::uint32_t>(data[0]) |
                (static_cast<std::uint32_t>(data[1]) << 8) |
                (static_cast<std::uint32_t>(data[2]) << 16) |
                (static_cast<std::uint32_t>(data[3]) << 24);
        }

        constexpr Color Decode565(std::uint16_t value)
        {
            const auto r5 = static_cast<std::uint8_t>((value >> 11) & 0x1f);
            const auto g6 = static_cast<std::uint8_t>((value >> 5) & 0x3f);
            const auto b5 = static_cast<std::uint8_t>(value & 0x1f);
            return {
                static_cast<std::uint8_t>((r5 << 3) | (r5 >> 2)),
                static_cast<std::uint8_t>((g6 << 2) | (g6 >> 4)),
                static_cast<std::uint8_t>((b5 << 3) | (b5 >> 2)),
                0xff
            };
        }

        constexpr Color Mix(
            const Color& left, unsigned leftWeight,
            const Color& right, unsigned rightWeight,
            unsigned divisor)
        {
            return {
                static_cast<std::uint8_t>(
                    (leftWeight * left.r + rightWeight * right.r) / divisor),
                static_cast<std::uint8_t>(
                    (leftWeight * left.g + rightWeight * right.g) / divisor),
                static_cast<std::uint8_t>(
                    (leftWeight * left.b + rightWeight * right.b) / divisor),
                0xff
            };
        }

        inline void WritePixel(
            std::vector<std::uint8_t>& output,
            std::uint32_t width,
            std::uint32_t x,
            std::uint32_t y,
            const Color& color)
        {
            const auto offset =
                (static_cast<std::size_t>(y) * width + x) * 4;
            output[offset + 0] = color.r;
            output[offset + 1] = color.g;
            output[offset + 2] = color.b;
            output[offset + 3] = color.a;
        }

        inline bool CheckedImageBytes(
            std::uint32_t width,
            std::uint32_t height,
            std::size_t& outputBytes)
        {
            if (width == 0 || height == 0) return false;
            const auto pixelCount =
                static_cast<std::size_t>(width) * height;
            if (pixelCount >
                std::numeric_limits<std::size_t>::max() / 4) {
                return false;
            }
            outputBytes = pixelCount * 4;
            return true;
        }
    }

    inline bool DecodeTopMipToRGBA8(
        std::span<const std::uint8_t> source,
        std::uint32_t width,
        std::uint32_t height,
        SourceFormat format,
        bool forceOpaqueAlpha,
        std::vector<std::uint8_t>& output,
        CancellationCheck cancellationCheck = nullptr,
        void* cancellationContext = nullptr)
    {
        // Failure never leaves a stale prior decode looking usable to callers.
        output.clear();
        const auto cancelled = [cancellationCheck, cancellationContext]() {
            return cancellationCheck &&
                cancellationCheck(cancellationContext);
        };

        std::size_t outputBytes = 0;
        if (width > kMaxDecodedDimension ||
            height > kMaxDecodedDimension ||
            !Detail::CheckedImageBytes(width, height, outputBytes) ||
            outputBytes > kMaxDecodedBytes) {
            return false;
        }

        std::size_t blockBytes = 0;
        std::size_t blocksWide = 0;
        std::size_t blocksHigh = 0;
        std::size_t requiredSourceBytes = 0;
        switch (format) {
        case SourceFormat::kRGBA8:
        case SourceFormat::kBGRA8:
            requiredSourceBytes = outputBytes;
            break;
        case SourceFormat::kBC1:
        case SourceFormat::kBC3:
            blockBytes = format == SourceFormat::kBC1 ? 8 : 16;
            blocksWide = (static_cast<std::size_t>(width) + 3) / 4;
            blocksHigh = (static_cast<std::size_t>(height) + 3) / 4;
            if (blocksWide != 0 &&
                blocksHigh >
                    std::numeric_limits<std::size_t>::max() / blocksWide) {
                return false;
            }
            requiredSourceBytes = blocksWide * blocksHigh;
            if (requiredSourceBytes >
                std::numeric_limits<std::size_t>::max() / blockBytes) {
                return false;
            }
            requiredSourceBytes *= blockBytes;
            break;
        default:
            return false;
        }
        if (source.size() < requiredSourceBytes ||
            outputBytes > output.max_size() || cancelled()) {
            return false;
        }

        try {
            output.assign(outputBytes, 0);
        } catch (const std::bad_alloc&) {
            return false;
        }
        if (cancelled()) {
            output.clear();
            return false;
        }

        if (format == SourceFormat::kRGBA8 ||
            format == SourceFormat::kBGRA8) {
            const auto rowBytes = static_cast<std::size_t>(width) * 4;
            for (std::uint32_t y = 0; y < height; ++y) {
                if (cancelled()) {
                    output.clear();
                    return false;
                }
                const auto rowStart = static_cast<std::size_t>(y) * rowBytes;
                for (std::size_t x = 0; x < rowBytes; x += 4) {
                    const auto offset = rowStart + x;
                    if (format == SourceFormat::kRGBA8) {
                        output[offset + 0] = source[offset + 0];
                        output[offset + 1] = source[offset + 1];
                        output[offset + 2] = source[offset + 2];
                    } else {
                        output[offset + 0] = source[offset + 2];
                        output[offset + 1] = source[offset + 1];
                        output[offset + 2] = source[offset + 0];
                    }
                    output[offset + 3] =
                        forceOpaqueAlpha ? 0xff : source[offset + 3];
                }
            }
            return true;
        }

        for (std::size_t blockY = 0; blockY < blocksHigh; ++blockY) {
            if (cancelled()) {
                output.clear();
                return false;
            }
            for (std::size_t blockX = 0; blockX < blocksWide; ++blockX) {
                const auto* block =
                    source.data() +
                    (blockY * blocksWide + blockX) * blockBytes;
                const auto* colorBlock =
                    format == SourceFormat::kBC3 ? block + 8 : block;

                const std::uint16_t color0 =
                    Detail::ReadU16(colorBlock + 0);
                const std::uint16_t color1 =
                    Detail::ReadU16(colorBlock + 2);
                Detail::Color colors[4] = {
                    Detail::Decode565(color0),
                    Detail::Decode565(color1),
                    {},
                    {}
                };
                if (format == SourceFormat::kBC3 || color0 > color1) {
                    colors[2] = Detail::Mix(
                        colors[0], 2, colors[1], 1, 3);
                    colors[3] = Detail::Mix(
                        colors[0], 1, colors[1], 2, 3);
                } else {
                    colors[2] = Detail::Mix(
                        colors[0], 1, colors[1], 1, 2);
                    colors[3] = { 0, 0, 0, 0 };
                }

                std::uint8_t alphas[8] = {
                    0xff, 0xff, 0xff, 0xff,
                    0xff, 0xff, 0xff, 0xff
                };
                std::uint64_t alphaIndices = 0;
                if (format == SourceFormat::kBC3) {
                    alphas[0] = block[0];
                    alphas[1] = block[1];
                    if (alphas[0] > alphas[1]) {
                        for (unsigned index = 2; index < 8; ++index) {
                            alphas[index] = static_cast<std::uint8_t>(
                                ((8 - index) * alphas[0] +
                                 (index - 1) * alphas[1]) / 7);
                        }
                    } else {
                        for (unsigned index = 2; index < 6; ++index) {
                            alphas[index] = static_cast<std::uint8_t>(
                                ((6 - index) * alphas[0] +
                                 (index - 1) * alphas[1]) / 5);
                        }
                        alphas[6] = 0;
                        alphas[7] = 0xff;
                    }
                    for (unsigned byteIndex = 0; byteIndex < 6; ++byteIndex) {
                        alphaIndices |=
                            static_cast<std::uint64_t>(block[2 + byteIndex])
                            << (8 * byteIndex);
                    }
                }

                std::uint32_t colorIndices =
                    Detail::ReadU32(colorBlock + 4);
                for (std::uint32_t y = 0; y < 4; ++y) {
                    for (std::uint32_t x = 0; x < 4; ++x) {
                        const auto outputX =
                            static_cast<std::uint32_t>(blockX * 4 + x);
                        const auto outputY =
                            static_cast<std::uint32_t>(blockY * 4 + y);
                        const auto pixelIndex = y * 4 + x;
                        const auto colorIndex =
                            static_cast<unsigned>(
                                (colorIndices >> (2 * pixelIndex)) & 0x3);
                        auto color = colors[colorIndex];
                        if (format == SourceFormat::kBC3) {
                            const auto alphaIndex =
                                static_cast<unsigned>(
                                    (alphaIndices >> (3 * pixelIndex)) & 0x7);
                            color.a = alphas[alphaIndex];
                        }
                        if (forceOpaqueAlpha) {
                            color.a = 0xff;
                        }
                        if (outputX < width && outputY < height) {
                            Detail::WritePixel(
                                output, width, outputX, outputY, color);
                        }
                    }
                }
            }
        }
        return true;
    }
}
