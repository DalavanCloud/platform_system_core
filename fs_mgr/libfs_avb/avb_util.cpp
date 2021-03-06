/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "avb_util.h"

#include <unistd.h>

#include <array>
#include <sstream>

#include <android-base/file.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include "util.h"

using android::base::StartsWith;
using android::base::unique_fd;

namespace android {
namespace fs_mgr {

// Helper functions to print enum class VBMetaVerifyResult.
const char* VBMetaVerifyResultToString(VBMetaVerifyResult result) {
    // clang-format off
    static const char* const name[] = {
        "ResultSuccess",
        "ResultError",
        "ResultErrorVerification",
        "ResultUnknown",
    };
    // clang-format on

    uint32_t index = static_cast<uint32_t>(result);
    uint32_t unknown_index = sizeof(name) / sizeof(char*) - 1;
    if (index >= unknown_index) {
        index = unknown_index;
    }

    return name[index];
}

std::ostream& operator<<(std::ostream& os, VBMetaVerifyResult result) {
    os << VBMetaVerifyResultToString(result);
    return os;
}

// class VBMetaData
// ----------------
std::unique_ptr<AvbVBMetaImageHeader> VBMetaData::GetVBMetaHeader(bool update_vbmeta_size) {
    auto vbmeta_header(std::make_unique<AvbVBMetaImageHeader>());

    if (!vbmeta_header) return nullptr;

    /* Byteswap the header. */
    avb_vbmeta_image_header_to_host_byte_order((AvbVBMetaImageHeader*)vbmeta_ptr_.get(),
                                               vbmeta_header.get());
    if (update_vbmeta_size) {
        vbmeta_size_ = sizeof(AvbVBMetaImageHeader) +
                       vbmeta_header->authentication_data_block_size +
                       vbmeta_header->auxiliary_data_block_size;
    }

    return vbmeta_header;
}

// Constructs dm-verity arguments for sending DM_TABLE_LOAD ioctl to kernel.
// See the following link for more details:
// https://gitlab.com/cryptsetup/cryptsetup/wikis/DMVerity
bool ConstructVerityTable(const AvbHashtreeDescriptor& hashtree_desc, const std::string& salt,
                          const std::string& root_digest, const std::string& blk_device,
                          android::dm::DmTable* table) {
    // Loads androidboot.veritymode from kernel cmdline.
    std::string verity_mode;
    if (!fs_mgr_get_boot_config("veritymode", &verity_mode)) {
        verity_mode = "enforcing";  // Defaults to enforcing when it's absent.
    }

    // Converts veritymode to the format used in kernel.
    std::string dm_verity_mode;
    if (verity_mode == "enforcing") {
        dm_verity_mode = "restart_on_corruption";
    } else if (verity_mode == "logging") {
        dm_verity_mode = "ignore_corruption";
    } else if (verity_mode != "eio") {  // Default dm_verity_mode is eio.
        LERROR << "Unknown androidboot.veritymode: " << verity_mode;
        return false;
    }

    std::ostringstream hash_algorithm;
    hash_algorithm << hashtree_desc.hash_algorithm;

    android::dm::DmTargetVerity target(0, hashtree_desc.image_size / 512,
                                       hashtree_desc.dm_verity_version, blk_device, blk_device,
                                       hashtree_desc.data_block_size, hashtree_desc.hash_block_size,
                                       hashtree_desc.image_size / hashtree_desc.data_block_size,
                                       hashtree_desc.tree_offset / hashtree_desc.hash_block_size,
                                       hash_algorithm.str(), root_digest, salt);
    if (hashtree_desc.fec_size > 0) {
        target.UseFec(blk_device, hashtree_desc.fec_num_roots,
                      hashtree_desc.fec_offset / hashtree_desc.data_block_size,
                      hashtree_desc.fec_offset / hashtree_desc.data_block_size);
    }
    if (!dm_verity_mode.empty()) {
        target.SetVerityMode(dm_verity_mode);
    }
    // Always use ignore_zero_blocks.
    target.IgnoreZeroBlocks();

    LINFO << "Built verity table: '" << target.GetParameterString() << "'";

    return table->AddTarget(std::make_unique<android::dm::DmTargetVerity>(target));
}

bool HashtreeDmVeritySetup(FstabEntry* fstab_entry, const AvbHashtreeDescriptor& hashtree_desc,
                           const std::string& salt, const std::string& root_digest,
                           bool wait_for_verity_dev) {
    android::dm::DmTable table;
    if (!ConstructVerityTable(hashtree_desc, salt, root_digest, fstab_entry->blk_device, &table) ||
        !table.valid()) {
        LERROR << "Failed to construct verity table.";
        return false;
    }
    table.set_readonly(true);

    const std::string mount_point(basename(fstab_entry->mount_point.c_str()));
    android::dm::DeviceMapper& dm = android::dm::DeviceMapper::Instance();
    if (!dm.CreateDevice(mount_point, table)) {
        LERROR << "Couldn't create verity device!";
        return false;
    }

    std::string dev_path;
    if (!dm.GetDmDevicePathByName(mount_point, &dev_path)) {
        LERROR << "Couldn't get verity device path!";
        return false;
    }

    // Marks the underlying block device as read-only.
    SetBlockDeviceReadOnly(fstab_entry->blk_device);

    // Updates fstab_rec->blk_device to verity device name.
    fstab_entry->blk_device = dev_path;

    // Makes sure we've set everything up properly.
    if (wait_for_verity_dev && !WaitForFile(dev_path, 1s)) {
        return false;
    }

    return true;
}

bool GetHashtreeDescriptor(const std::string& partition_name,
                           const std::vector<VBMetaData>& vbmeta_images,
                           AvbHashtreeDescriptor* out_hashtree_desc, std::string* out_salt,
                           std::string* out_digest) {
    bool found = false;
    const uint8_t* desc_partition_name;

    for (const auto& vbmeta : vbmeta_images) {
        size_t num_descriptors;
        std::unique_ptr<const AvbDescriptor* [], decltype(&avb_free)> descriptors(
                avb_descriptor_get_all(vbmeta.data(), vbmeta.size(), &num_descriptors), avb_free);

        if (!descriptors || num_descriptors < 1) {
            continue;
        }

        for (size_t n = 0; n < num_descriptors && !found; n++) {
            AvbDescriptor desc;
            if (!avb_descriptor_validate_and_byteswap(descriptors[n], &desc)) {
                LWARNING << "Descriptor[" << n << "] is invalid";
                continue;
            }
            if (desc.tag == AVB_DESCRIPTOR_TAG_HASHTREE) {
                desc_partition_name =
                        (const uint8_t*)descriptors[n] + sizeof(AvbHashtreeDescriptor);
                if (!avb_hashtree_descriptor_validate_and_byteswap(
                            (AvbHashtreeDescriptor*)descriptors[n], out_hashtree_desc)) {
                    continue;
                }
                if (out_hashtree_desc->partition_name_len != partition_name.length()) {
                    continue;
                }
                // Notes that desc_partition_name is not NUL-terminated.
                std::string hashtree_partition_name((const char*)desc_partition_name,
                                                    out_hashtree_desc->partition_name_len);
                if (hashtree_partition_name == partition_name) {
                    found = true;
                }
            }
        }

        if (found) break;
    }

    if (!found) {
        LERROR << "Partition descriptor not found: " << partition_name.c_str();
        return false;
    }

    const uint8_t* desc_salt = desc_partition_name + out_hashtree_desc->partition_name_len;
    *out_salt = BytesToHex(desc_salt, out_hashtree_desc->salt_len);

    const uint8_t* desc_digest = desc_salt + out_hashtree_desc->salt_len;
    *out_digest = BytesToHex(desc_digest, out_hashtree_desc->root_digest_len);

    return true;
}

// Converts a AVB partition_name (without A/B suffix) to a device partition name.
// e.g.,       "system" => "system_a",
//       "system_other" => "system_b".
//
// If the device is non-A/B, converts it to a partition name without suffix.
// e.g.,       "system" => "system",
//       "system_other" => "system".
std::string AvbPartitionToDevicePatition(const std::string& avb_partition_name,
                                         const std::string& ab_suffix,
                                         const std::string& ab_other_suffix) {
    bool is_other_slot = false;
    std::string sanitized_partition_name(avb_partition_name);

    auto other_suffix = sanitized_partition_name.rfind("_other");
    if (other_suffix != std::string::npos) {
        sanitized_partition_name.erase(other_suffix);  // converts system_other => system
        is_other_slot = true;
    }

    auto append_suffix = is_other_slot ? ab_other_suffix : ab_suffix;
    return sanitized_partition_name + append_suffix;
}

off64_t GetTotalSize(int fd) {
    off64_t saved_current = lseek64(fd, 0, SEEK_CUR);
    if (saved_current == -1) {
        PERROR << "Failed to get current position";
        return -1;
    }

    // lseek64() returns the resulting offset location from the beginning of the file.
    off64_t total_size = lseek64(fd, 0, SEEK_END);
    if (total_size == -1) {
        PERROR << "Failed to lseek64 to end of the partition";
        return -1;
    }

    // Restores the original offset.
    if (lseek64(fd, saved_current, SEEK_SET) == -1) {
        PERROR << "Failed to lseek64 to the original offset: " << saved_current;
    }

    return total_size;
}

std::unique_ptr<AvbFooter> GetAvbFooter(int fd) {
    std::array<uint8_t, AVB_FOOTER_SIZE> footer_buf;
    auto footer(std::make_unique<AvbFooter>());

    off64_t footer_offset = GetTotalSize(fd) - AVB_FOOTER_SIZE;

    ssize_t num_read =
            TEMP_FAILURE_RETRY(pread64(fd, footer_buf.data(), AVB_FOOTER_SIZE, footer_offset));
    if (num_read < 0 || num_read != AVB_FOOTER_SIZE) {
        PERROR << "Failed to read AVB footer";
        return nullptr;
    }

    if (!avb_footer_validate_and_byteswap((const AvbFooter*)footer_buf.data(), footer.get())) {
        PERROR << "AVB footer verification failed.";
        return nullptr;
    }

    return footer;
}

bool VerifyPublicKeyBlob(const uint8_t* key, size_t length, const std::string& expected_key_blob) {
    if (expected_key_blob.empty()) {  // no expectation of the key, return true.
        return true;
    }
    if (expected_key_blob.size() != length) {
        return false;
    }
    if (0 == memcmp(key, expected_key_blob.data(), length)) {
        return true;
    }
    return false;
}

VBMetaVerifyResult VerifyVBMetaSignature(const VBMetaData& vbmeta,
                                         const std::string& expected_public_key_blob) {
    const uint8_t* pk_data;
    size_t pk_len;
    ::AvbVBMetaVerifyResult vbmeta_ret;

    vbmeta_ret = avb_vbmeta_image_verify(vbmeta.data(), vbmeta.size(), &pk_data, &pk_len);

    switch (vbmeta_ret) {
        case AVB_VBMETA_VERIFY_RESULT_OK:
            if (pk_data == nullptr || pk_len <= 0) {
                LERROR << vbmeta.partition()
                       << ": Error verifying vbmeta image: failed to get public key";
                return VBMetaVerifyResult::kError;
            }
            if (!VerifyPublicKeyBlob(pk_data, pk_len, expected_public_key_blob)) {
                LERROR << vbmeta.partition() << ": Error verifying vbmeta image: public key used to"
                       << " sign data does not match key in chain descriptor";
                return VBMetaVerifyResult::kErrorVerification;
            }
            return VBMetaVerifyResult::kSuccess;
        case AVB_VBMETA_VERIFY_RESULT_OK_NOT_SIGNED:
        case AVB_VBMETA_VERIFY_RESULT_HASH_MISMATCH:
        case AVB_VBMETA_VERIFY_RESULT_SIGNATURE_MISMATCH:
            LERROR << vbmeta.partition() << ": Error verifying vbmeta image: "
                   << avb_vbmeta_verify_result_to_string(vbmeta_ret);
            return VBMetaVerifyResult::kErrorVerification;
        case AVB_VBMETA_VERIFY_RESULT_INVALID_VBMETA_HEADER:
            // No way to continue this case.
            LERROR << vbmeta.partition() << ": Error verifying vbmeta image: invalid vbmeta header";
            break;
        case AVB_VBMETA_VERIFY_RESULT_UNSUPPORTED_VERSION:
            // No way to continue this case.
            LERROR << vbmeta.partition()
                   << ": Error verifying vbmeta image: unsupported AVB version";
            break;
        default:
            LERROR << "Unknown vbmeta image verify return value: " << vbmeta_ret;
            break;
    }

    return VBMetaVerifyResult::kError;
}

std::unique_ptr<VBMetaData> VerifyVBMetaData(int fd, const std::string& partition_name,
                                             const std::string& expected_public_key_blob,
                                             VBMetaVerifyResult* out_verify_result) {
    uint64_t vbmeta_offset = 0;
    uint64_t vbmeta_size = VBMetaData::kMaxVBMetaSize;
    bool is_vbmeta_partition = StartsWith(partition_name, "vbmeta");

    if (!is_vbmeta_partition) {
        std::unique_ptr<AvbFooter> footer = GetAvbFooter(fd);
        if (!footer) {
            return nullptr;
        }
        vbmeta_offset = footer->vbmeta_offset;
        vbmeta_size = footer->vbmeta_size;
    }

    if (vbmeta_size > VBMetaData::kMaxVBMetaSize) {
        LERROR << "VbMeta size in footer exceeds kMaxVBMetaSize";
        return nullptr;
    }

    auto vbmeta = std::make_unique<VBMetaData>(vbmeta_size, partition_name);
    ssize_t num_read = TEMP_FAILURE_RETRY(pread64(fd, vbmeta->data(), vbmeta_size, vbmeta_offset));
    // Allows partial read for vbmeta partition, because its vbmeta_size is kMaxVBMetaSize.
    if (num_read < 0 || (!is_vbmeta_partition && static_cast<uint64_t>(num_read) != vbmeta_size)) {
        PERROR << partition_name << ": Failed to read vbmeta at offset " << vbmeta_offset
               << " with size " << vbmeta_size;
        return nullptr;
    }

    auto verify_result = VerifyVBMetaSignature(*vbmeta, expected_public_key_blob);
    if (out_verify_result != nullptr) *out_verify_result = verify_result;

    if (verify_result == VBMetaVerifyResult::kSuccess ||
        verify_result == VBMetaVerifyResult::kErrorVerification) {
        return vbmeta;
    }

    return nullptr;
}

bool RollbackDetected(const std::string& partition_name ATTRIBUTE_UNUSED,
                      uint64_t rollback_index ATTRIBUTE_UNUSED) {
    // TODO(bowgotsai): Support rollback protection.
    return false;
}

std::vector<ChainInfo> GetChainPartitionInfo(const VBMetaData& vbmeta, bool* fatal_error) {
    CHECK(fatal_error != nullptr);
    std::vector<ChainInfo> chain_partitions;

    size_t num_descriptors;
    std::unique_ptr<const AvbDescriptor* [], decltype(&avb_free)> descriptors(
            avb_descriptor_get_all(vbmeta.data(), vbmeta.size(), &num_descriptors), avb_free);

    if (!descriptors || num_descriptors < 1) {
        return {};
    }

    for (size_t i = 0; i < num_descriptors; i++) {
        AvbDescriptor desc;
        if (!avb_descriptor_validate_and_byteswap(descriptors[i], &desc)) {
            LERROR << "Descriptor[" << i << "] is invalid in vbmeta: " << vbmeta.partition();
            *fatal_error = true;
            return {};
        }
        if (desc.tag == AVB_DESCRIPTOR_TAG_CHAIN_PARTITION) {
            AvbChainPartitionDescriptor chain_desc;
            if (!avb_chain_partition_descriptor_validate_and_byteswap(
                        (AvbChainPartitionDescriptor*)descriptors[i], &chain_desc)) {
                LERROR << "Chain descriptor[" << i
                       << "] is invalid in vbmeta: " << vbmeta.partition();
                *fatal_error = true;
                return {};
            }
            const char* chain_partition_name =
                    ((const char*)descriptors[i]) + sizeof(AvbChainPartitionDescriptor);
            const char* chain_public_key_blob =
                    chain_partition_name + chain_desc.partition_name_len;
            chain_partitions.emplace_back(
                    std::string(chain_partition_name, chain_desc.partition_name_len),
                    std::string(chain_public_key_blob, chain_desc.public_key_len));
        }
    }

    return chain_partitions;
}

VBMetaVerifyResult LoadAndVerifyVbmetaImpl(
        const std::string& partition_name, const std::string& ab_suffix,
        const std::string& ab_other_suffix, const std::string& expected_public_key_blob,
        bool allow_verification_error, bool load_chained_vbmeta, bool rollback_protection,
        std::function<std::string(const std::string&)> device_path_constructor,
        bool is_chained_vbmeta, std::vector<VBMetaData>* out_vbmeta_images) {
    // Ensures the device path (might be a symlink created by init) is ready to access.
    auto device_path = device_path_constructor(
            AvbPartitionToDevicePatition(partition_name, ab_suffix, ab_other_suffix));
    if (!WaitForFile(device_path, 1s)) {
        PERROR << "No such partition: " << device_path;
        return VBMetaVerifyResult::kError;
    }

    unique_fd fd(TEMP_FAILURE_RETRY(open(device_path.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd < 0) {
        PERROR << "Failed to open: " << device_path;
        return VBMetaVerifyResult::kError;
    }

    VBMetaVerifyResult verify_result;
    std::unique_ptr<VBMetaData> vbmeta =
            VerifyVBMetaData(fd, partition_name, expected_public_key_blob, &verify_result);
    if (!vbmeta) {
        LERROR << partition_name << ": Failed to load vbmeta, result: " << verify_result;
        return VBMetaVerifyResult::kError;
    }

    if (!allow_verification_error && verify_result == VBMetaVerifyResult::kErrorVerification) {
        LERROR << partition_name << ": allow verification error is not allowed";
        return VBMetaVerifyResult::kError;
    }

    std::unique_ptr<AvbVBMetaImageHeader> vbmeta_header =
            vbmeta->GetVBMetaHeader(true /* update_vbmeta_size */);
    if (!vbmeta_header) {
        LERROR << partition_name << ": Failed to get vbmeta header";
        return VBMetaVerifyResult::kError;
    }

    if (rollback_protection && RollbackDetected(partition_name, vbmeta_header->rollback_index)) {
        return VBMetaVerifyResult::kError;
    }

    // vbmeta flags can only be set by the top-level vbmeta image.
    if (is_chained_vbmeta && vbmeta_header->flags != 0) {
        LERROR << partition_name << ": chained vbmeta image has non-zero flags";
        return VBMetaVerifyResult::kError;
    }

    if (out_vbmeta_images) {
        out_vbmeta_images->emplace_back(std::move(*vbmeta));
    }

    // If verification has been disabled by setting a bit in the image, we're done.
    if (vbmeta_header->flags & AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED) {
        LWARNING << "VERIFICATION_DISABLED bit is set for partition: " << partition_name;
        return verify_result;
    }

    if (load_chained_vbmeta) {
        bool fatal_error = false;
        auto chain_partitions = GetChainPartitionInfo(*out_vbmeta_images->rbegin(), &fatal_error);
        if (fatal_error) {
            return VBMetaVerifyResult::kError;
        }
        for (auto& chain : chain_partitions) {
            auto sub_ret = LoadAndVerifyVbmetaImpl(
                    chain.partition_name, ab_suffix, ab_other_suffix, chain.public_key_blob,
                    allow_verification_error, load_chained_vbmeta, rollback_protection,
                    device_path_constructor, true, /* is_chained_vbmeta */
                    out_vbmeta_images);
            if (sub_ret != VBMetaVerifyResult::kSuccess) {
                verify_result = sub_ret;  // might be 'ERROR' or 'ERROR VERIFICATION'.
                if (verify_result == VBMetaVerifyResult::kError) {
                    return verify_result;  // stop here if we got an 'ERROR'.
                }
            }
        }
    }

    return verify_result;
}

}  // namespace fs_mgr
}  // namespace android
