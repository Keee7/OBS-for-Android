import base64
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('sign_android_apk', Path(__file__).with_name('sign-android-apk.py'))
signer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(signer)


class SigningConfigTest(unittest.TestCase):
    def test_inline_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'settings.json'
            values = dict(zip(signer.KEYS, [base64.b64encode(b'dummy store').decode(), 'test-alias', 'fake store pass', 'fake key pass']))
            path.write_text(json.dumps(values), encoding='utf-8')
            self.assertEqual(signer.read_config(path), (b'dummy store', 'test-alias', 'fake store pass', 'fake key pass'))

    def test_labelled_external_base64_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            store = Path(tmp) / 'store with spaces.b64.txt'
            store.write_text(base64.b64encode(b'dummy store').decode(), encoding='utf-8')
            config = Path(tmp) / 'release settings.txt'
            config.write_text('\n'.join(f'{key}\t{value}' for key, value in zip(signer.KEYS, [str(store), 'test-alias', 'fake-store-pass', 'fake-key-pass'])), encoding='utf-8')
            self.assertEqual(signer.read_config(config), (b'dummy store', 'test-alias', 'fake-store-pass', 'fake-key-pass'))

    def test_missing_credentials_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = Path(tmp) / 'settings.json'
            config.write_text('{}', encoding='utf-8')
            with self.assertRaises(ValueError):
                signer.read_config(config)


if __name__ == '__main__':
    unittest.main()
