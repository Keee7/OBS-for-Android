#!/usr/bin/env python3
"""Verify MediaCodec output and delivery evidence in a complete OBS log."""
import argparse
import codecs
from pathlib import Path
import re
import sys

MARKER = '[android-mediacodec]'
UINT = re.compile(r'^[0-9]+$')
MAX_U64 = (1 << 64) - 1
COUNTERS = ('submitted', 'output', 'delivered', 'bytes', 'keyframes',
            'queue', 'queue_bytes', 'peak_queue_bytes', 'elapsed_ms', 'failed')
MONOTONIC = ('submitted', 'output', 'delivered', 'bytes', 'keyframes', 'peak_queue_bytes', 'elapsed_ms')


def parse_log(text):
    """Return sessions in first-observed order. Unrelated plugin diagnostics are ignored."""
    sessions = {}
    for line_no, line in enumerate(text.splitlines(), 1):
        if MARKER not in line:
            continue
        body = line.split(MARKER, 1)[1].strip()
        # AMediaFormat_toString is free-form; never parse its payload as fields.
        if re.match(r'^session=[0-9]+\s+event=format\s+codec=(?:h264|hevc)\s+format=', body):
            continue
        # Error messages are free-form; capture only the fixed prefix.
        error = re.match(r'^session=([0-9]+)\s+event=error\s+codec=(h264|hevc)\s+message=', body)
        if error:
            sid = int(error.group(1))
            if sid > MAX_U64:
                raise ValueError(f'line {line_no}: invalid session id')
            sessions.setdefault(sid, []).append((line_no, {'event': 'error', 'codec': error.group(2)}, False))
            continue
        fields = {}
        duplicate = False
        for item in body.split():
            if '=' not in item:
                continue
            key, value = item.split('=', 1)
            if key in fields:
                duplicate = True
            fields[key] = value
        event = fields.get('event')
        if event not in ('started', 'first-packet', 'progress', 'summary', 'format', 'error'):
            continue
        sid = fields.get('session', '')
        if not UINT.fullmatch(sid) or int(sid) > MAX_U64:
            raise ValueError(f'line {line_no}: invalid or missing session id')
        sid = int(sid)
        sessions.setdefault(sid, []).append((line_no, fields, duplicate))
    return sessions


def number(fields, key, line):
    value = fields.get(key, '')
    if not UINT.fullmatch(value) or int(value) > MAX_U64:
        raise ValueError(f'line {line}: invalid or missing {key}')
    return int(value)


def evaluate(text, codec=None, session=None):
    sessions = parse_log(text)
    if session is not None:
        if session not in sessions:
            raise ValueError(f'session {session} not found')
        selected = session
    else:
        matching = [sid for sid, events in sessions.items()
                    if codec is None or any(f.get('codec') == codec for _, f, _ in events)]
        if not matching:
            raise ValueError('no matching MediaCodec session found')
        selected = matching[-1]  # first appearance determines session order, not last interleaved event
    events = sessions[selected]
    starts, first, snapshots = [], [], []
    actual_codec = None
    component = None
    for line, fields, duplicate in events:
        event = fields['event']
        if duplicate:
            raise ValueError(f'line {line}: duplicate field')
        record_codec = fields.get('codec')
        if record_codec not in ('h264', 'hevc'):
            raise ValueError(f'line {line}: invalid or missing codec')
        if actual_codec is not None and record_codec != actual_codec:
            raise ValueError(f'line {line}: codec changed within session')
        actual_codec = record_codec
        if codec and codec != actual_codec:
            raise ValueError(f'session {selected} is {actual_codec}, not {codec}')
        if event == 'error':
            raise ValueError(f'line {line}: session {selected} reports an error')
        if event == 'started':
            if starts or first or snapshots:
                raise ValueError(f'line {line}: duplicate or out-of-order started event')
            if fields.get('hardware') != 'true' or fields.get('software') != 'false' or fields.get('input') != 'Surface':
                raise ValueError(f'line {line}: hardware Surface input not established')
            component = fields.get('component', '')
            if not component or component == 'none':
                raise ValueError(f'line {line}: missing component')
            for key in ('width', 'height', 'fps_num', 'fps_den', 'bitrate'):
                if number(fields, key, line) == 0:
                    raise ValueError(f'line {line}: {key} must be positive')
            starts.append(fields)
        elif event == 'first-packet':
            if not starts or first:
                raise ValueError(f'line {line}: first-packet missing start or duplicated')
            if fields.get('headers') != 'valid' or fields.get('keyframe') != '1' or number(fields, 'bytes', line) == 0:
                raise ValueError(f'line {line}: invalid first packet evidence')
            first.append(fields)
        elif event in ('progress', 'summary'):
            if fields.get('component') != component or component is None:
                raise ValueError(f'line {line}: missing start or component mismatch')
            values = {key: number(fields, key, line) for key in COUNTERS}
            if values['failed'] != 0:
                raise ValueError(f'line {line}: session reports failed={values["failed"]}')
            if snapshots and any(values[k] < snapshots[-1][k] for k in MONOTONIC):
                raise ValueError(f'line {line}: session counters or elapsed time decreased')
            snapshots.append(values)
    if not starts or not first or len(snapshots) < 2:
        raise ValueError(f'session {selected}: insufficient started, first-packet or progress evidence')
    start = snapshots[0]
    end = snapshots[-1]
    if (end['elapsed_ms'] <= start['elapsed_ms'] or
            any(end[k] <= start[k] for k in ('submitted', 'output', 'delivered', 'bytes')) or end['keyframes'] < 1):
        raise ValueError(f'session {selected}: counters or elapsed time did not increase sufficiently')
    fps = (end['output'] - start['output']) * 1000 / (end['elapsed_ms'] - start['elapsed_ms'])
    return selected, actual_codec, component, fps


def read_log(path):
    data = path.read_bytes()
    if data.startswith(codecs.BOM_UTF16_LE):
        return data.decode('utf-16')
    return data.decode('utf-8-sig')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path, help='complete OBS log file')
    parser.add_argument('--codec', choices=('h264', 'hevc'))
    parser.add_argument('--session', type=int, help='explicit uint64 session id')
    args = parser.parse_args(argv)
    if args.session is not None and not 0 <= args.session <= MAX_U64:
        parser.error('--session must be a uint64')
    try:
        result = evaluate(read_log(args.log), args.codec, args.session)
    except (OSError, UnicodeError, ValueError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        return 1
    sid, codec, component, fps = result
    print(f'PASS: session={sid} codec={codec} component={component}; measured output rate={fps:.2f} fps')
    print('Evidence covers codec output and delivery to OBS; it does not verify a recording file or device 60 fps performance.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
