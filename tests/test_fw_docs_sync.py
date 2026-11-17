#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文档与代码的一致性检查。

README 是外部读者读的第一份东西，也是最先过期的东西：代码改了、文档没跟上，
对外就是"这人不维护项目"。人盯不住，就让测试盯。

原则：只校验**语义**（引脚、波特率、地址、超时、容量、安全开关），
不校验排版和措辞。改一句话不该红，改一个数字必须红。
"""

import os
import re
import unittest

import common


class BaudRateTest(unittest.TestCase):
    def test_all_uarts_run_at_115200(self):
        debug = common.parse_int(
            common.defines(common.read_fw("Hardware", "USART.h"))["DEBUG_USART_BAUDRATE"])
        pi = common.parse_int(
            common.defines(common.read_fw("Hardware", "pi_link.h"))["PI_UART_BAUDRATE"])
        serial_src = common.read_fw("Hardware", "Serial.c")
        match = re.search(r"USART_BaudRate\s*=\s*(\d+)", serial_src)
        self.assertIsNotNone(match, "Serial.c 里找不到 UART3 的波特率赋值")
        cc2530 = int(match.group(1))

        for name, baud in (("USART1(调试)", debug),
                           ("USART2(树莓派)", pi),
                           ("USART3(CC2530)", cc2530)):
            self.assertEqual(115200, baud, "%s 波特率变了，README 的串口表要同步" % name)

    def test_readme_states_115200(self):
        self.assertIn("115200", common.read(common.README_PATH))


class PinoutTest(unittest.TestCase):
    def test_readme_pins_are_the_ones_wired_in_code(self):
        readme = common.read(common.README_PATH)
        for pins in ("PA9/PA10", "PA2/PA3", "PB10/PB11"):
            self.assertIn(pins, readme, "README 串口表里没有 %s" % pins)

    def test_uart_pins_match_gpio_init(self):
        # USART1/USART2 的引脚在头文件里用宏定义，UART3 直接写在 Serial.c
        debug = common.defines(common.read_fw("Hardware", "USART.h"))
        pi = common.defines(common.read_fw("Hardware", "pi_link.h"))
        self.assertEqual("GPIO_Pin_9", debug["DEBUG_USART_TX_GPIO_PIN"])
        self.assertEqual("GPIOA", debug["DEBUG_USART_TX_GPIO_PORT"])
        self.assertEqual("GPIO_Pin_2", pi["PI_UART_TX_PIN"])
        self.assertEqual("GPIOA", pi["PI_UART_TX_PORT"])
        self.assertIn("GPIO_Pin_10", common.read_fw("Hardware", "Serial.c"))


class ConstantDriftTest(unittest.TestCase):
    """README §6 列出来的常量，必须和头文件逐项相等。"""

    def assert_define_in_readme(self, readme, source_text, name):
        expected = common.define_int(source_text, name)
        pattern = re.compile(r"%s\s+\(?(\d+)" % re.escape(name))
        found = pattern.search(readme)
        self.assertIsNotNone(found, "README 里找不到 %s" % name)
        self.assertEqual(expected, int(found.group(1)),
                         "README 的 %s 与头文件不一致" % name)

    def test_timeouts(self):
        readme = common.read(common.README_PATH)
        cfg = common.read_fw("User", "project_config.h")
        for name in ("BARRIER_UP_DELAY_MS", "BARRIER_MOVE_MS",
                     "ERROR_DISPLAY_DELAY_MS", "WAIT_CARD_TIMEOUT_MS"):
            self.assert_define_in_readme(readme, cfg, name)

    def test_watchdog(self):
        readme = common.read(common.README_PATH)
        wdg = common.read_fw("Hardware", "watchdog.h")
        for name in ("WATCHDOG_TIMEOUT_MS", "WATCHDOG_FEED_INTERVAL_MS"):
            self.assert_define_in_readme(readme, wdg, name)

    def test_node_report_period(self):
        readme = common.read(common.README_PATH)
        self.assert_define_in_readme(
            readme, common.read(common.ZIGBEE_SRC, "ParkingNode.h"),
            "PARKINGNODE_SEND_PERIODIC_MSG_TIMEOUT")

    def test_whitelist_capacity(self):
        cards = common.define_int(common.read_fw("Hardware", "flash_storage.h"),
                                  "MAX_WHITELIST_CARDS")
        readme = common.read(common.README_PATH)
        self.assertIn("最多 %d 张" % cards, readme)

    def test_frame_version(self):
        ver = common.define_int(common.read_fw("Hardware", "link_frame.h"),
                                "LINK_FRAME_VER")
        readme = common.read(common.README_PATH)
        self.assertIn("VER=%d" % ver, readme,
                      "README 里没写当前协议版本，升级时两边会对不上")


class ZigbeeConfigTest(unittest.TestCase):
    def cfg(self):
        return common.read(common.ZIGBEE_CFG)

    def readme(self):
        return common.read(common.README_PATH)

    def test_security_is_on_and_documented(self):
        self.assertIn("-DSECURE=1", self.cfg())
        self.assertNotIn("-DSECURE=0", self.cfg())
        self.assertIn("SECURE=1", self.readme())

    def test_tc_device_table_matches_node_count(self):
        cfg = self.cfg()
        max_dev = common.cfg_int(cfg, "ZDSECMGR_TC_DEVICE_MAX")
        spots = common.define_int(common.read_fw("User", "project_config.h"), "TOTAL_SPOTS")
        self.assertEqual(spots + 1, max_dev,
                         "TC Link Key 表要能装下 1 个协调器 + 全部节点")
        self.assertIn("ZDSECMGR_TC_DEVICE_MAX=%d" % max_dev, self.readme())

    def test_default_key_is_not_the_public_one(self):
        """ZigBeeAlliance09 是公开密钥，谁都能入网，不能出现在提交里。"""
        cfg = self.cfg()
        public = "0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F"
        self.assertNotIn(public, cfg.replace("\"", ""))

    def test_secure_dynamic_is_pinned(self):
        self.assertIn("-DZG_SECURE_DYNAMIC=0", self.cfg())


class TestCountTest(unittest.TestCase):
    """README 里写的用例数也必须是真的。

    这里用"数 def test_"而不是跑发现器：确定、不递归、不依赖运行顺序。
    代价是它把自己也算进去——所以新增用例后 README 的数字要加一，
    这正是我们想强制的那件事：改了测试，文档跟着改。
    """

    def _count(self, rel_dir):
        base = os.path.join(common.REPO_ROOT, rel_dir)
        total = 0
        for name in sorted(os.listdir(base)):
            if not (name.startswith("test_") and name.endswith(".py")):
                continue
            with open(os.path.join(base, name), encoding="utf-8") as fh:
                total += len(re.findall(r"^\s*def test_", fh.read(), re.M))
        return total

    def test_readme_states_real_counts(self):
        rpi = self._count(os.path.join("raspberrypi", "tests"))
        fw = self._count("tests")
        readme = common.read(common.README_PATH)
        self.assertIn("| %d 项 |" % rpi, readme, "README 里树莓派用例数过期了（实际 %d）" % rpi)
        self.assertIn("| %d 项 |" % fw, readme, "README 里固件用例数过期了（实际 %d）" % fw)
        self.assertIn("合计 %d 项" % (rpi + fw), readme,
                      "README 里的合计用例数过期了（实际 %d）" % (rpi + fw))

    def test_testing_doc_states_real_counts(self):
        """TESTING.md 里的数字同样必须是真的。

        README 过期只是"介绍不准"，测试文档过期会误导人按错的基线判断结果，
        所以这里单独卡一次：两份文档都得与实际用例数一致。
        """
        path = os.path.join(common.REPO_ROOT, "TESTING.md")
        self.assertTrue(os.path.exists(path), "缺少 TESTING.md（测试文档）")
        doc = common.read(path)
        rpi = self._count(os.path.join("raspberrypi", "tests"))
        fw = self._count("tests")
        want = "> 用例总数：%d 项（树莓派 %d + 固件 %d）" % (rpi + fw, rpi, fw)
        self.assertIn(want, doc, "TESTING.md 的用例总数过期了，应为：%s" % want)


class CodeSizeTest(unittest.TestCase):
    """README 里贴的编译体积要自洽，而且必须装得进应用区。"""

    def test_reported_size_is_self_consistent_and_fits(self):
        readme = common.read(common.README_PATH)
        m = re.search(r"Code\s+(\d+)\s*\+\s*RO\s+(\d+)\s*\+\s*RW\s+(\d+)\s*=\s*(\d+)", readme)
        self.assertIsNotNone(m, "README 里找不到 'Code x + RO y + RW z = w' 这段")
        code, ro, rw, total = (int(g) for g in m.groups())
        self.assertEqual(code + ro + rw, total, "README 的加法和结果对不上")
        _, irom = common.irom1()
        self.assertLess(total, irom,
                        "文档里写的固件体积 %d 字节已经超过应用区 %d 字节" % (total, irom))


if __name__ == "__main__":
    unittest.main()
