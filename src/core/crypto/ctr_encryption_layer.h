// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>

#include "core/crypto/aes_util.h"
#include "core/crypto/encryption_layer.h"
#include "core/crypto/key_manager.h"

namespace Core::Crypto {

// Sits on top of a VirtualFile and provides CTR-mode AES decription.
class CTREncryptionLayer : public EncryptionLayer {
public:
    using IVData = std::array<u8, 16>;

    CTREncryptionLayer(FileSys::VirtualFile base, Key128 key, std::size_t base_offset);

    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override;

    void SetIV(const IVData& iv);

private:
    std::size_t base_offset;

    // Must be mutable as operations modify cipher contexts.
    mutable AESCipher<Key128> cipher;
    mutable IVData iv{};

    void UpdateIV(std::size_t offset) const;
};

} // namespace Core::Crypto
