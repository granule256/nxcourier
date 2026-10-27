"""Web 资源的包访问、离线依赖与表单关联检查，不访问设备。"""
from html.parser import HTMLParser
from importlib import resources
from pathlib import Path
import sys
import unittest
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "pc/src"))


class PageInventory(HTMLParser):
    def __init__(self):
        super().__init__()
        self.ids = []
        self.references = []
        self.assets = []
        self.language = None

    def handle_starttag(self, tag, attributes):
        values = dict(attributes)
        if "id" in values:
            self.ids.append(values["id"])
        if tag == "html":
            self.language = values.get("lang")
        if tag == "label" and "for" in values:
            self.references.append(values["for"])
        self.references.extend(values.get("aria-labelledby", "").split())
        if tag == "script" and "src" in values:
            self.assets.append(values["src"])
        if tag == "link" and values.get("rel") == "stylesheet":
            self.assets.append(values["href"])


class HostWebAssetTests(unittest.TestCase):
    def setUp(self):
        self.web = resources.files("nxc_host").joinpath("web")
        self.page = PageInventory()
        self.page.feed(self.web.joinpath("index.html").read_text(encoding="utf-8"))

    def test_page_dependencies_are_available_inside_package(self):
        self.assertTrue(self.page.assets, "页面必须声明自身所需资源")
        for asset in self.page.assets:
            with self.subTest(asset=asset):
                parsed = urlparse(asset)
                self.assertFalse(parsed.scheme or parsed.netloc,
                                 "本机控制台不应依赖 CDN 或其他服务器")
                self.assertTrue(parsed.path.startswith("/assets/"))
                content = self.web.joinpath(parsed.path.removeprefix("/assets/")).read_bytes()
                self.assertGreater(len(content), 0)

    def test_chinese_page_and_control_references_are_unambiguous(self):
        self.assertEqual(self.page.language, "zh-CN")
        self.assertEqual(len(self.page.ids), len(set(self.page.ids)), "重复 ID 会绑定到错误控件")
        for reference in self.page.references:
            with self.subTest(reference=reference):
                self.assertIn(reference, self.page.ids, "表单标签或无障碍标题引用了不存在的控件")


if __name__ == "__main__":
    unittest.main()
