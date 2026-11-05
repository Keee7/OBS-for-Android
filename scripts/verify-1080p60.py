#!/usr/bin/env python3
"""Verify a moving-scene recording with ffprobe and ffmpeg; reports file-level evidence only."""
import argparse
import json
import subprocess
import sys
from fractions import Fraction


def evaluate(video, duration, timestamps, hashes, min_seconds=60, expected_codec='h264'):
    """Return (errors, measurements). Timestamps must be decoded presentation order."""
    errors = []
    try:
        duration = float(duration)
        declared = float(Fraction(video['avg_frame_rate']))
        count = int(video['nb_read_frames'])
        pts = [float(x) for x in timestamps]
        if duration <= 0 or declared <= 0 or not all(__import__('math').isfinite(x) for x in pts):
            raise ValueError('invalid time values')
    except (ValueError, TypeError, KeyError, ZeroDivisionError):
        return ['missing or invalid timing data'], {}
    if video.get('codec_name') != expected_codec or (video.get('width'), video.get('height')) != (1920, 1080):
        label = {'h264': 'H.264', 'hevc': 'HEVC'}.get(expected_codec, expected_codec)
        errors.append(f'expected {label} 1920x1080')
    if duration < min_seconds:
        errors.append('recording is too short')
    if declared < 59 or count / duration < 59:
        errors.append('average or measured frame rate below 59 fps')
    if len(pts) != count or len(hashes) != count:
        errors.append('timestamp/hash count does not match decoded frame count')
    if len(pts) < 2 or any(b <= a for a, b in zip(pts, pts[1:])):
        errors.append('timestamps are missing, duplicated, or out of presentation order')
    elif max(b - a for a, b in zip(pts, pts[1:])) > 0.1:
        errors.append('presentation gap exceeds 100 ms')
    # A static test scene cannot establish motion smoothness. Consecutive identical
    # decoded frame hashes for >= 1 second are a failed/inconclusive test.
    run = longest = repeated = 0
    previous = None
    for frame_hash in hashes:
        if frame_hash == previous:
            repeated += 1
        run = run + 1 if frame_hash == previous else 1
        longest = max(longest, run)
        previous = frame_hash
    if longest >= 59 or (hashes and repeated / len(hashes) > 0.05):
        errors.append('excess identical frames: use a moving scene or investigate a freeze/duplicates')
    return errors, {'duration': duration, 'frames': count, 'declared': declared,
                    'measured': count / duration, 'longest_identical_run': longest}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', help='local recording pulled from the device')
    parser.add_argument('--min-seconds', type=float, default=60)
    parser.add_argument('--codec', choices=('h264', 'hevc'), default='h264')
    args = parser.parse_args()
    try:
        probe = subprocess.run(['ffprobe', '-v', 'error', '-select_streams', 'v:0', '-count_frames',
            '-show_entries', 'stream=codec_name,width,height,avg_frame_rate,nb_read_frames,duration:format=duration:frame=best_effort_timestamp_time',
            '-show_frames', '-of', 'json', args.recording], check=True, capture_output=True, text=True)
        data = json.loads(probe.stdout)
        video = data['streams'][0]
        duration = video.get('duration') or data['format']['duration']
        timestamps = [frame['best_effort_timestamp_time'] for frame in data['frames']]
        digest = subprocess.run(['ffmpeg', '-v', 'error', '-i', args.recording, '-map', '0:v:0',
            '-fps_mode', 'passthrough', '-f', 'framemd5', '-'], check=True, capture_output=True, text=True)
        hashes = [line.rsplit(',', 1)[-1].strip() for line in digest.stdout.splitlines()
                  if line and not line.startswith('#')]
        errors, stats = evaluate(video, duration, timestamps, hashes, args.min_seconds, args.codec)
    except (OSError, subprocess.CalledProcessError, ValueError, KeyError, IndexError) as exc:
        print(f'FAIL: unable to inspect recording: {exc}', file=sys.stderr)
        return 2
    print('Measurements:', stats)
    for error in errors:
        print('FAIL:', error)
    if not errors:
        print('PASS: file-level 1080p60 checks passed')
    print('Hardware component, sustained performance and audio sync need separate device evidence.')
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
