#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Flash 分区 / RAM 预算 / 缓冲尺寸的结构性检查。

这些都是"改一个数字，板子就废"的地方，而且编译器不会替你检查：

  * 应用区压到白名单页上 -> 每次烧固件把卡全擦掉（这个项目真发生过，
    白名单原来在 0x08007000，IROM1 只能给到 28KB）；
  * 白名单页没对齐 1KB -> 擦除按页走，会连带擦掉相邻分区；
  * 接收缓冲小于擦除窗口内的字节数 -> 加卡时丢车位帧；
  * 行缓冲 >= 环形缓冲 -> 环形缓冲再大也没用，行早就截断了。

全部是纯文本/常量比较，不需要编译，也就不会因为工具链版本而失效。
"""

import unittest

import common


def _fw(*parts):
    return common.read_fw(*parts)


class FlashPartitionTest(unittest.TestCase):
    def setUp(self):
        self.storage = _fw("Hardware", "flash_storage.h")

    def slot(self, name):
        return common.define_int(self.storage, name)

    def test_slots_are_page_aligned(self):
        for name in ("FLASH_SLOT_A_ADDR", "FLASH_SLOT_B_ADDR"):
            addr = self.slot(name)
            self.assertEqual(0, addr % 0x400, "%s=0x%08X 没按 1KB 页对齐" % (name, addr))

    def test_slots_are_adjacent_pages(self):
        a = self.slot("FLASH_SLOT_A_ADDR")
        b = self.slot("FLASH_SLOT_B_ADDR")
        self.assertEqual(0x400, b - a, "A/B 两槽必须相邻，否则中间会留下没人管的页")

    def test_slots_inside_flash(self):
        for name in ("FLASH_SLOT_A_ADDR", "FLASH_SLOT_B_ADDR"):
            addr = self.slot(name)
            self.assertGreaterEqual(addr, 0x08000000)
            self.assertLess(addr + 0x400, 0x08010000)

    def test_app_region_stops_before_whitelist(self):
        a = self.slot("FLASH_SLOT_A_ADDR")
        limit = common.define_int(_fw("User", "project_config.h"), "FLASH_APP_LIMIT_ADDR")
        self.assertEqual(a, limit,
                         "FLASH_APP_LIMIT_ADDR 必须等于 A 槽起始地址，否则应用会盖住白名单")

    def test_keil_irom1_matches_app_limit(self):
        start, size = common.irom1()
        a = self.slot("FLASH_SLOT_A_ADDR")
        self.assertEqual(0x08000000, start)
        self.assertEqual(a - start, size,
                         "Keil IROM1 与 FLASH_APP_LIMIT_ADDR 不一致：链接器不会报错，"
                         "但下载时会擦到白名单页")

    def test_fault_log_page_is_outside_download_range(self):
        fault = common.define_int(_fw("System", "fault_log.h"), "FAULT_LOG_ADDR")
        b = self.slot("FLASH_SLOT_B_ADDR")
        self.assertEqual(0, fault % 0x400)
        self.assertGreaterEqual(fault, b + 0x400, "故障记录页不能压在 B 槽上")
        self.assertLessEqual(fault + 0x400, 0x08010000)
        _, size = common.irom1()
        self.assertLess(0x08000000 + size, fault, "故障记录页必须在下载范围之外，否则升级就丢现场")

    def test_whitelist_fits_in_one_page(self):
        cards = common.define_int(self.storage, "MAX_WHITELIST_CARDS")
        card_len = common.define_int(self.storage, "CARD_ID_LENGTH")
        header = 24                        # magic/version/seq/len/crc/commit
        need = 1 + cards * card_len + header
        self.assertLess(need, 0x400,
                        "白名单 %d 张 × %d 字节 + 头部 %d 字节超过 1KB 页，写的时候会跨页"
                        % (cards, card_len, header))


class RamBudgetTest(unittest.TestCase):
    def test_keil_iram_is_20k(self):
        _, size = common.iram1()
        self.assertEqual(0x5000, size, "STM32F103C8T6 只有 20KB SRAM，工程配置必须一致")

    def test_pi_line_buffer_smaller_than_ring(self):
        buf = common.define_int(_fw("Hardware", "pi_link.h"), "PI_RX_BUF_SIZE")
        ring = common.define_int(_fw("Hardware", "pi_link.c"), "PI_RX_RING_SIZE")
        self.assertLess(buf, ring, "行缓冲必须小于接收环形缓冲，否则环再大也会被行截断")

    def test_serial_line_buffer_smaller_than_dma_ring(self):
        line = common.define_int(_fw("Hardware", "Serial.c"), "SERIAL_RX_LINE_SIZE")
        dma = common.define_int(_fw("Hardware", "Serial.c"), "SERIAL_RX_DMA_SIZE")
        self.assertLessEqual(line, dma)

    def test_dma_ring_covers_flash_erase_window(self):
        """擦 Flash 时 CPU 停转，DMA 必须能独立吞下这段时间的字节。"""
        dma = common.define_int(_fw("Hardware", "Serial.c"), "SERIAL_RX_DMA_SIZE")
        self.assertEqual(0, dma & (dma - 1), "环形缓冲取 2 的幂，取模才能编译成位与")

        bytes_per_sec = 115200 / 10.0          # 8N1：每字节 10 bit
        worst_ms = 2 * 40                      # 连擦 A/B 两页，F1 手册最坏 40ms/页
        worst_bytes = bytes_per_sec * worst_ms / 1000.0
        self.assertGreaterEqual(dma, 2 * worst_bytes,
                                "DMA 缓冲 %d 字节，覆盖不了两页擦除最坏情况 %.0f 字节（要求 2 倍余量）"
                                % (dma, worst_bytes))


class SpotModelTest(unittest.TestCase):
    def test_total_spots_matches_max_id(self):
        cfg = _fw("User", "project_config.h")
        self.assertEqual(common.define_int(cfg, "TOTAL_SPOTS"),
                         common.define_int(cfg, "MAX_SPOT_ID"))

    def test_dirty_mask_has_a_bit_per_spot(self):
        serial = _fw("Hardware", "Serial.c")
        spots = common.define_int(_fw("User", "project_config.h"), "TOTAL_SPOTS")
        self.assertRegex(serial, r"volatile\s+uint32_t\s+spot_dirty_mask",
                         "车位事件位图必须是 32 位且 volatile（主循环读、串口上下文写）")
        self.assertLess(spots, 32, "位图是 32 位，车位号不能再多了")

    def test_node_spot_number_fits_protocol(self):
        # 协议是 PXX:，两位十进制
        spots = common.define_int(_fw("User", "project_config.h"), "TOTAL_SPOTS")
        self.assertLessEqual(spots, 99)


class ReadmePartitionTest(unittest.TestCase):
    """README 里贴的分区表必须和头文件一致，否则新人照着文档改分区就会踩坑。"""

    def test_readme_lists_same_addresses(self):
        readme = common.read(common.README_PATH)
        storage = _fw("Hardware", "flash_storage.h")
        for name in ("FLASH_SLOT_A_ADDR", "FLASH_SLOT_B_ADDR"):
            addr = common.define_int(storage, name)
            self.assertIn("0x%08X" % addr, readme.upper().replace("0X", "0x"),
                          "README 里没有 %s 的实际地址" % name)
        fault = common.define_int(_fw("System", "fault_log.h"), "FAULT_LOG_ADDR")
        self.assertIn("0x%08X" % fault, readme.upper().replace("0X", "0x"))

    def test_readme_app_region_size_matches_project(self):
        _, size = common.irom1()
        readme = common.read(common.README_PATH)
        self.assertIn("0xF000", readme)
        self.assertIn("%dKB" % (size // 1024), readme)


if __name__ == "__main__":
    unittest.main()
