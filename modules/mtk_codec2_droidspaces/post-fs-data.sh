#!/system/bin/sh
# SPDX-License-Identifier: Apache-2.0
# ReSukiSU loads all sepolicy.rule files before this stage, including
# the DroidSpaces rule file that declares droidspacesd.
exec /data/adb/ksud sepolicy patch \
    'allow mtk_hal_c2 droidspacesd binder { call transfer }
allow mtk_hal_c2 droidspacesd fd use'
