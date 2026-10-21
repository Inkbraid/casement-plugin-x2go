// JPEG and PNG decoding with the Windows Imaging Component, into BGRX.

#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>

#include "x2go/frames.h"

namespace x2go {

/// A decoder for one thread: create it on the thread that uses it.
class WicDecoder {
public:
    /// Initialises COM for this thread and creates the WIC factory. On
    /// failure `error` says why and the decoder decodes nothing.
    explicit WicDecoder(std::string& error);
    ~WicDecoder();
    WicDecoder(const WicDecoder&) = delete;
    WicDecoder& operator=(const WicDecoder&) = delete;

    bool decode(std::span<const std::uint8_t> data, Image& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace x2go
