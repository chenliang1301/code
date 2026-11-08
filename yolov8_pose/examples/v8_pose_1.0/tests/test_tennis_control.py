"""Command/result protocol tests without a camera or HTTP server."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'tennis_control', Path(__file__).resolve().parents[1] / 'tools/tennis_control.py')
control = importlib.util.module_from_spec(spec)
spec.loader.exec_module(control)


class ControlTests(unittest.TestCase):
    def invoke(self, argv, responses):
        output = io.StringIO()
        with patch.object(sys, 'argv', ['tennis_control'] + argv), \
             patch.object(control, 'request', side_effect=responses), \
             contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            code = control.main()
        return code, output.getvalue()

    def status(self, ok=True):
        return {'state': {'enabled': True, 'advice': 'stand ready'}, 'age_ms': 0,
                'commands': [{'id': 7, 'result': {'ok': ok, 'message': 'executed' if ok else 'apply ROI'}}]}

    def test_success_waits_for_matching_result(self):
        code, text = self.invoke(['pause'], [{'id': 7}, self.status()])
        self.assertEqual(code, 0)
        self.assertIn('executed', text)

    def test_execution_rejection_is_failure(self):
        code, text = self.invoke(['start'], [{'id': 7}, self.status(False)])
        self.assertEqual(code, 1)
        self.assertIn('apply ROI', text)

    def test_server_rejection_is_reported(self):
        code, text = self.invoke(['pause'], [RuntimeError('HTTP 409: disabled')])
        self.assertEqual(code, 1)
        self.assertIn('409', text)

    def test_stale_status_is_visible(self):
        status = self.status(); status['age_ms'] = 4000
        code, text = self.invoke(['status'], [status])
        self.assertEqual(code, 0)
        self.assertIn('4000', text)

    def test_destructive_command_requires_explicit_flag(self):
        with patch.object(sys, 'argv', ['tennis_control', 'reset']), \
             contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as exc:
                control.main()
            self.assertEqual(exc.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
