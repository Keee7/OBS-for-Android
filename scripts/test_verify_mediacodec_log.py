import codecs
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

SCRIPT = Path(__file__).with_name('verify-mediacodec-log.py')
spec = importlib.util.spec_from_file_location('verify_mc', SCRIPT)
verify = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify)


def started(sid=1, codec='hevc', component='c2.vendor.hevc.encoder'):
    return f'12:00 [android-mediacodec] session={sid} event=started codec={codec} component={component} hardware=true software=false input=Surface width=1920 height=1080 fps_num=60 fps_den=1 bitrate=10000000'


def first(sid=1, codec='hevc'):
    return f'[android-mediacodec] session={sid} event=first-packet codec={codec} headers=valid keyframe=1 bytes=1000'


def progress(sid=1, n=1, event='progress', failed=0, codec='hevc',
             component='c2.vendor.hevc.encoder', dropped=0):
    return (f'[android-mediacodec] session={sid} event={event} codec={codec} component={component} '
            f'submitted={n*100} output={n*90} delivered={n*89} bytes={n*10000} keyframes={n} queue=1 '
            f'queue_bytes=10 peak_queue_bytes=100 dropped_load={dropped} elapsed_ms={n*5000} failed={failed}')


def shed(sid=1, dropped=10, event='shedding', codec='hevc'):
    return (f'[android-mediacodec] session={sid} event={event} codec={codec} '
            f'pending=3 degraded=3 dropped_load={dropped}')


def good(sid=1):
    return '\n'.join((started(sid), first(sid), progress(sid), progress(sid, 2)))


class SyntheticMediaCodecLogTests(unittest.TestCase):
    def test_valid_and_rate(self):
        sid, codec, component, fps = verify.evaluate(good())
        self.assertEqual((sid, codec, component, fps), (1, 'hevc', 'c2.vendor.hevc.encoder', 18.0))

    def test_shedding_edges_are_accepted_and_checked(self):
        log = '\n'.join((started(), first(), progress(dropped=0), shed(dropped=40),
                         progress(n=2, dropped=40), shed(dropped=40, event='recovered'),
                         progress(n=3, dropped=40)))
        self.assertEqual(verify.evaluate(log)[0], 1)

    def test_shedding_edges_must_be_consistent(self):
        cases = {
            # a shedding edge that claims no new dropped frame
            'no new drops': shed(dropped=0),
            # dropping_load going backwards between progress lines
            'counters went backwards': None,
            # a recovered edge that moves the counter
            'recovered moved counter': shed(dropped=99, event='recovered'),
            # duplicate recovered edges
            'duplicate recovered': shed(dropped=10, event='recovered'),
        }
        base = '\n'.join((started(), first(), progress(dropped=0)))
        for name, row in cases.items():
            if row is None:
                log = base + '\n' + progress(n=2, dropped=10) + '\n' + progress(n=3, dropped=5)
            elif name == 'duplicate recovered':
                log = base + '\n' + row + '\n' + row
            elif name == 'recovered moved counter':
                log = base + '\n' + shed(dropped=10) + '\n' + row
            else:
                log = base + '\n' + row
            with self.subTest(case=name), self.assertRaises(ValueError):
                verify.evaluate(log)

    def test_progress_requires_dropped_load(self):
        log = good().replace('dropped_load=0', '')
        with self.assertRaisesRegex(ValueError, 'dropped_load'):
            verify.evaluate(log)

    def test_progress_can_precede_first_packet(self):
        log = '\n'.join((started(), progress(), first(), progress(n=2)))
        self.assertEqual(verify.evaluate(log)[0], 1)

    def test_truncated(self):
        for log in (started(), started() + '\n' + first(), good().rsplit(' failed=', 1)[0]):
            with self.subTest(log=log), self.assertRaises(ValueError):
                verify.evaluate(log)

    def test_only_candidate_eligible(self):
        with self.assertRaises(ValueError):
            verify.evaluate('hardware encoder candidate eligible: c2.vendor.hevc.encoder')

    def test_started_without_packet(self):
        with self.assertRaises(ValueError):
            verify.evaluate('\n'.join((started(), progress(), progress(n=2))))

    def test_reverse_counts(self):
        with self.assertRaises(ValueError):
            verify.evaluate('\n'.join((started(), first(), progress(n=2), progress(n=1))))

    def test_failed_summary_even_after_success(self):
        with self.assertRaisesRegex(ValueError, 'failed'):
            verify.evaluate(good() + '\n' + progress(n=3, event='summary', failed=1))

    def test_two_sessions_latest_failure_and_explicit_old(self):
        log = good(1) + '\n' + started(2) + '\n' + progress(2, 1, 'summary', failed=1)
        with self.assertRaises(ValueError):
            verify.evaluate(log, codec='hevc')
        self.assertEqual(verify.evaluate(log, codec='hevc', session=1)[0], 1)

    def test_interleaved_sessions_use_first_appearance_order(self):
        log = '\n'.join((started(1), first(1), progress(1), started(2), first(2),
                         progress(2), progress(1, 2), progress(2, 2)))
        self.assertEqual(verify.evaluate(log)[0], 2)

    def test_no_growth_and_invalid_hardware(self):
        for log in (good().replace(progress(n=2), progress()), good().replace('hardware=true', 'hardware=false'),
                    good().replace('headers=valid', 'headers=invalid'), good().replace('keyframe=1 bytes=', 'keyframe=0 bytes=')):
            with self.subTest(log=log), self.assertRaises(ValueError):
                verify.evaluate(log)

    def test_error_event_without_summary(self):
        error = ('[android-mediacodec] session=1 event=error codec=hevc '
                 'message=codec failed session=999 event=progress failed=0 codec=h264')
        with self.assertRaisesRegex(ValueError, 'reports an error'):
            verify.evaluate(good() + '\n' + error)
        with self.assertRaisesRegex(ValueError, 'reports an error'):
            verify.evaluate(error)
        latest = good() + '\n' + error.replace('session=1 event=error', 'session=2 event=error', 1)
        with self.assertRaisesRegex(ValueError, 'session 2 reports an error'):
            verify.evaluate(latest)
        self.assertEqual(verify.evaluate(latest, session=1)[0], 1)

    def test_format_payload_is_ignored_and_evidence_duplicates_rejected(self):
        freeform = ('[android-mediacodec] session=1 event=format codec=hevc format={'
                    'session=999 event=summary codec=h264 codec=bogus session=1 = failed=1}')
        log = '\n'.join((started(), freeform, first(), progress(), progress(n=2)))
        self.assertEqual(verify.evaluate(log)[0], 1)
        for malformed in (started() + ' codec=hevc', progress(n=2) + ' session=1'):
            rows = [started(), first(), progress(), progress(n=2)]
            rows[0 if 'event=started' in malformed else 3] = malformed
            with self.subTest(malformed=malformed), self.assertRaisesRegex(ValueError, 'duplicate'):
                verify.evaluate('\n'.join(rows))

    def test_synthetic_cli_log_encodings(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as d:
            path = Path(d) / 'synthetic.log'
            samples = {'utf-8': good().encode('utf-8'),
                       'utf-8-bom': good().encode('utf-8-sig'),
                       'utf-16-le-bom': codecs.BOM_UTF16_LE + good().encode('utf-16-le')}
            for encoding, data in samples.items():
                with self.subTest(encoding=encoding):
                    path.write_bytes(data)
                    result = subprocess.run([sys.executable, str(SCRIPT), str(path)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertIn('session=1', result.stdout)

    def test_cli(self):
        with tempfile.TemporaryDirectory(dir=Path.cwd()) as d:
            path = Path(d) / 'synthetic.log'
            path.write_text(good(), encoding='utf-8')
            result = subprocess.run([sys.executable, str(SCRIPT), str(path), '--codec', 'hevc', '--session', '1'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn('18.00 fps', result.stdout)
            path.write_text(started(), encoding='utf-8')
            result = subprocess.run([sys.executable, str(SCRIPT), str(path)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
