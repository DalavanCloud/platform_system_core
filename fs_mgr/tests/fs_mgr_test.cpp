/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include <linux/fs.h>
#include <mntent.h>

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <android-base/file.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <fstab/fstab.h>
#include <gtest/gtest.h>

#include "../fs_mgr_priv_boot_config.h"

namespace {

const std::string cmdline =
        "rcupdate.rcu_expedited=1 rootwait ro "
        "init=/init androidboot.bootdevice=1d84000.ufshc "
        "androidboot.baseband=sdy androidboot.keymaster=1  skip_initramfs "
        "androidboot.serialno=BLAHBLAHBLAH androidboot.slot_suffix=_a "
        "androidboot.hardware.platform=sdw813 androidboot.hardware=foo "
        "androidboot.revision=EVT1.0 androidboot.bootloader=burp-0.1-7521 "
        "androidboot.hardware.sku=mary androidboot.hardware.radio.subtype=0 "
        "androidboot.dtbo_idx=2 androidboot.mode=normal "
        "androidboot.hardware.ddr=1GB,combuchi,LPDDR4X "
        "androidboot.ddr_info=combuchiandroidboot.ddr_size=2GB "
        "androidboot.hardware.ufs=2GB,combushi "
        "androidboot.boottime=0BLE:58,1BLL:22,1BLE:571,2BLL:105,ODT:0,AVB:123 "
        "androidboot.ramdump=disabled "
        "dm=\"1 vroot none ro 1,0 10416 verity 1 624684 fec_start 624684\" "
        "root=/dev/dm-0 "
        "androidboot.vbmeta.device=PARTUUID=aa08f1a4-c7c9-402e-9a66-9707cafa9ceb "
        "androidboot.vbmeta.avb_version=\"1.1\" "
        "androidboot.vbmeta.device_state=unlocked "
        "androidboot.vbmeta.hash_alg=sha256 androidboot.vbmeta.size=5248 "
        "androidboot.vbmeta.digest="
        "ac13147e959861c20f2a6da97d25fe79e60e902c022a371c5c039d31e7c68860 "
        "androidboot.vbmeta.invalidate_on_error=yes "
        "androidboot.veritymode=enforcing androidboot.verifiedbootstate=orange "
        "androidboot.space=\"sha256 5248 androidboot.nospace=nope\" "
        "printk.devkmsg=on msm_rtb.filter=0x237 ehci-hcd.park=3 "
        "\"string =\"\"string '\" "
        "service_locator.enable=1 firmware_class.path=/vendor/firmware "
        "cgroup.memory=nokmem lpm_levels.sleep_disabled=1 "
        "buildvariant=userdebug  console=null "
        "terminator=\"truncated";

const std::vector<std::pair<std::string, std::string>> result_space = {
        {"rcupdate.rcu_expedited", "1"},
        {"rootwait", ""},
        {"ro", ""},
        {"init", "/init"},
        {"androidboot.bootdevice", "1d84000.ufshc"},
        {"androidboot.baseband", "sdy"},
        {"androidboot.keymaster", "1"},
        {"skip_initramfs", ""},
        {"androidboot.serialno", "BLAHBLAHBLAH"},
        {"androidboot.slot_suffix", "_a"},
        {"androidboot.hardware.platform", "sdw813"},
        {"androidboot.hardware", "foo"},
        {"androidboot.revision", "EVT1.0"},
        {"androidboot.bootloader", "burp-0.1-7521"},
        {"androidboot.hardware.sku", "mary"},
        {"androidboot.hardware.radio.subtype", "0"},
        {"androidboot.dtbo_idx", "2"},
        {"androidboot.mode", "normal"},
        {"androidboot.hardware.ddr", "1GB,combuchi,LPDDR4X"},
        {"androidboot.ddr_info", "combuchiandroidboot.ddr_size=2GB"},
        {"androidboot.hardware.ufs", "2GB,combushi"},
        {"androidboot.boottime", "0BLE:58,1BLL:22,1BLE:571,2BLL:105,ODT:0,AVB:123"},
        {"androidboot.ramdump", "disabled"},
        {"dm", "1 vroot none ro 1,0 10416 verity 1 624684 fec_start 624684"},
        {"root", "/dev/dm-0"},
        {"androidboot.vbmeta.device", "PARTUUID=aa08f1a4-c7c9-402e-9a66-9707cafa9ceb"},
        {"androidboot.vbmeta.avb_version", "1.1"},
        {"androidboot.vbmeta.device_state", "unlocked"},
        {"androidboot.vbmeta.hash_alg", "sha256"},
        {"androidboot.vbmeta.size", "5248"},
        {"androidboot.vbmeta.digest",
         "ac13147e959861c20f2a6da97d25fe79e60e902c022a371c5c039d31e7c68860"},
        {"androidboot.vbmeta.invalidate_on_error", "yes"},
        {"androidboot.veritymode", "enforcing"},
        {"androidboot.verifiedbootstate", "orange"},
        {"androidboot.space", "sha256 5248 androidboot.nospace=nope"},
        {"printk.devkmsg", "on"},
        {"msm_rtb.filter", "0x237"},
        {"ehci-hcd.park", "3"},
        {"string ", "string '"},
        {"service_locator.enable", "1"},
        {"firmware_class.path", "/vendor/firmware"},
        {"cgroup.memory", "nokmem"},
        {"lpm_levels.sleep_disabled", "1"},
        {"buildvariant", "userdebug"},
        {"console", "null"},
        {"terminator", "truncated"},
};

}  // namespace

TEST(fs_mgr, fs_mgr_parse_boot_config) {
    EXPECT_EQ(result_space, fs_mgr_parse_boot_config(cmdline));
}

TEST(fs_mgr, fs_mgr_get_boot_config_from_kernel_cmdline) {
    std::string content;
    for (const auto& entry : result_space) {
        static constexpr char androidboot[] = "androidboot.";
        if (!android::base::StartsWith(entry.first, androidboot)) continue;
        auto key = entry.first.substr(strlen(androidboot));
        EXPECT_TRUE(fs_mgr_get_boot_config_from_kernel(cmdline, key, &content)) << " for " << key;
        EXPECT_EQ(entry.second, content);
    }
    EXPECT_FALSE(fs_mgr_get_boot_config_from_kernel(cmdline, "vbmeta.avb_versio", &content));
    EXPECT_TRUE(content.empty()) << content;
    EXPECT_FALSE(fs_mgr_get_boot_config_from_kernel(cmdline, "nospace", &content));
    EXPECT_TRUE(content.empty()) << content;
}

TEST(fs_mgr, fs_mgr_read_fstab_file_proc_mounts) {
    Fstab fstab;
    ASSERT_TRUE(ReadFstabFromFile("/proc/mounts", &fstab));

    std::unique_ptr<std::FILE, int (*)(std::FILE*)> mounts(setmntent("/proc/mounts", "re"),
                                                           endmntent);
    ASSERT_NE(mounts, nullptr);

    mntent* mentry;
    size_t i = 0;
    while ((mentry = getmntent(mounts.get())) != nullptr) {
        ASSERT_LT(i, fstab.size());
        auto& entry = fstab[i];

        EXPECT_EQ(mentry->mnt_fsname, entry.blk_device);
        EXPECT_EQ(mentry->mnt_dir, entry.mount_point);
        EXPECT_EQ(mentry->mnt_type, entry.fs_type);

        std::set<std::string> mnt_opts;
        for (auto& s : android::base::Split(mentry->mnt_opts, ",")) {
            mnt_opts.emplace(s);
        }
        std::set<std::string> fs_options;
        if (!entry.fs_options.empty()) {
            for (auto& s : android::base::Split(entry.fs_options, ",")) {
                fs_options.emplace(s);
            }
        }
        // matches private content in fs_mgr_fstab.c
        static struct flag_list {
            const char* name;
            unsigned int flag;
        } mount_flags[] = {
                {"noatime", MS_NOATIME},
                {"noexec", MS_NOEXEC},
                {"nosuid", MS_NOSUID},
                {"nodev", MS_NODEV},
                {"nodiratime", MS_NODIRATIME},
                {"ro", MS_RDONLY},
                {"rw", 0},
                {"remount", MS_REMOUNT},
                {"bind", MS_BIND},
                {"rec", MS_REC},
                {"unbindable", MS_UNBINDABLE},
                {"private", MS_PRIVATE},
                {"slave", MS_SLAVE},
                {"shared", MS_SHARED},
                {"defaults", 0},
                {0, 0},
        };
        for (auto f = 0; mount_flags[f].name; ++f) {
            if (mount_flags[f].flag & entry.flags) {
                fs_options.emplace(mount_flags[f].name);
            }
        }
        if (!(entry.flags & MS_RDONLY)) {
            fs_options.emplace("rw");
        }
        EXPECT_EQ(mnt_opts, fs_options);
        ++i;
    }
    EXPECT_EQ(i, fstab.size());
}

TEST(fs_mgr, ReadFstabFromFile_FsOptions) {
    Fstab fstab;
    std::string fstab_file = android::base::GetExecutableDirectory() + "/data/fstab.example";
    EXPECT_TRUE(ReadFstabFromFile(fstab_file, &fstab));

    EXPECT_EQ("/", fstab[0].mount_point);
    EXPECT_EQ("barrier=1", fstab[0].fs_options);

    EXPECT_EQ("/metadata", fstab[1].mount_point);
    EXPECT_EQ("discard", fstab[1].fs_options);

    EXPECT_EQ("/data", fstab[2].mount_point);
    EXPECT_EQ("discard,reserve_root=32768,resgid=1065,fsync_mode=nobarrier", fstab[2].fs_options);

    EXPECT_EQ("/misc", fstab[3].mount_point);
    EXPECT_EQ("", fstab[3].fs_options);

    EXPECT_EQ("/vendor/firmware_mnt", fstab[4].mount_point);
    EXPECT_EQ(
            "shortname=lower,uid=1000,gid=1000,dmask=227,fmask=337,"
            "context=u:object_r:firmware_file:s0",
            fstab[4].fs_options);

    EXPECT_EQ("auto", fstab[5].mount_point);
    EXPECT_EQ("", fstab[5].fs_options);

    EXPECT_EQ("none", fstab[6].mount_point);
    EXPECT_EQ("", fstab[6].fs_options);
}
