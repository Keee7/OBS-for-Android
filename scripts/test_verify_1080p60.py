import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('verify1080', Path(__file__).with_name('verify-1080p60.py'))
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)

class VerifyTest(unittest.TestCase):
    def setUp(self):
        self.video = {'codec_name': 'h264', 'width': 1920, 'height': 1080,
                      'avg_frame_rate': '60000/1001', 'nb_read_frames': '3600'}
        self.pts = [i * 1001 / 60000 for i in range(3600)]
        self.hashes = [str(i) for i in range(3600)]
    def check(self, duration=60.06):
        return verify.evaluate(self.video, duration, self.pts, self.hashes)[0]
    def test_fractional_rate(self):
        self.assertEqual(self.check(), [])
    def test_hevc(self):
        self.video['codec_name'] = 'hevc'
        errors, _ = verify.evaluate(self.video, 60.06, self.pts, self.hashes, expected_codec='hevc')
        self.assertEqual(errors, [])
    def test_codec_mismatch(self):
        errors, _ = verify.evaluate(self.video, 60.06, self.pts, self.hashes, expected_codec='hevc')
        self.assertIn('expected HEVC 1920x1080', errors)
        self.video['codec_name'] = 'hevc'
        self.assertIn('expected H.264 1920x1080', self.check())
    def test_short(self):
        self.assertTrue(any('short' in e for e in self.check(10)))
    def test_few_frames(self):
        self.video['nb_read_frames'] = '1800'
        self.assertTrue(any('frame rate' in e for e in self.check()))
    def test_out_of_order(self):
        self.pts[100], self.pts[101] = self.pts[101], self.pts[100]
        self.assertTrue(any('out of presentation order' in e for e in self.check()))
    def test_freeze_not_pass(self):
        self.hashes[100:160] = ['same'] * 60
        self.assertTrue(any('identical frames' in e for e in self.check()))
    def test_duplicated_30fps_not_pass(self):
        self.hashes = [str(i // 2) for i in range(3600)]
        self.assertTrue(any('identical frames' in e for e in self.check()))
    def test_gap(self):
        self.pts[100:] = [p + .2 for p in self.pts[100:]]
        self.assertTrue(any('gap' in e for e in self.check()))
    def test_missing_hash(self):
        self.hashes.pop()
        self.assertTrue(any('count' in e for e in self.check()))

if __name__ == '__main__':
    unittest.main()
