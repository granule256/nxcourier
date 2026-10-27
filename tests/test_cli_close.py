"""宿主回归：关闭应用后，必须由用户判图决定下一次输入。"""
import contextlib
import io
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pc/src"))
from nxc_host import control as ctl


class CloseTests(unittest.TestCase):
    def run_close(self, responses, pid=None):
        calls = []

        def call(device, request):
            calls.append(request)
            # 保留旧行为所读的尺寸，确保小画面也绝不触发按键。
            if request["method"] == "screen.capture":
                return {"payload": [{"bytes": "37000"}]}
            if request["method"] == "input.press":
                return {"payload": [{"result": "pressed"}]}
            return responses.pop(0)

        output = io.StringIO()
        with patch.object(ctl.nxc_mcp, "tool_call", side_effect=call), \
             patch.object(ctl.time, "sleep"), contextlib.redirect_stdout(output):
            ctl.close(pid)
        return calls, output.getvalue()

    def test_close_never_presses_for_small_screenshot(self):
        calls, output = self.run_close([{"payload": [{"result": "terminated"}]}], 12)
        self.assertEqual([c["method"] for c in calls], ["title.terminate"])
        self.assertEqual(calls[0]["params"]["confirm"], "I-KNOW-THIS-CLOSES")
        self.assertIn("用户", output)
        self.assertIn("截图", output)
        self.assertIn("确认", output)

    def test_close_current_application_only(self):
        calls, _ = self.run_close([
            {"payload": [{"running": "application", "pid": "42"}]},
            {"payload": [{"result": "terminated"}]},
        ])
        self.assertEqual([c["method"] for c in calls], ["title.current", "title.terminate"])
        self.assertEqual(calls[1]["params"]["pid"], 42)

    def test_no_application_does_not_terminate_or_press(self):
        calls, _ = self.run_close([{"payload": [{"running": "none"}]}])
        self.assertEqual([c["method"] for c in calls], ["title.current"])

    def test_current_query_error_is_reported_without_termination(self):
        calls, output = self.run_close([{"error": {"code": 503, "message": "pmdmnt-unavailable"}}])
        self.assertEqual([c["method"] for c in calls], ["title.current"])
        self.assertIn("查询当前应用失败", output)
        self.assertIn("503", output)
        self.assertIn("pmdmnt-unavailable", output)
        self.assertNotIn("没有应用", output)

    def test_missing_current_state_is_reported_without_termination(self):
        calls, output = self.run_close([{"payload": []}])
        self.assertEqual([c["method"] for c in calls], ["title.current"])
        self.assertIn("查询当前应用失败", output)
        self.assertNotIn("没有应用", output)

    def test_terminate_failure_does_not_capture_or_press(self):
        calls, _ = self.run_close([{"error": "terminate-failed"}], 12)
        self.assertEqual([c["method"] for c in calls], ["title.terminate"])


if __name__ == "__main__":
    unittest.main()
