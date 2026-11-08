#!/usr/bin/env python3
"""Query diagnostics or submit a command and wait for its execution result. Stdlib only."""
import argparse
import json
import sys
import time
import urllib.error
import urllib.request


def request(base, path, post=False):
    req = urllib.request.Request(base.rstrip('/') + path, method='POST' if post else 'GET')
    try:
        with urllib.request.urlopen(req, timeout=5) as response:
            return json.load(response)
    except urllib.error.HTTPError as exc:
        try:
            detail = json.load(exc).get('message', str(exc))
        except (ValueError, AttributeError):
            detail = str(exc)
        raise RuntimeError(f'HTTP {exc.code}: {detail}') from exc


def show(data):
    state = data['state']
    print(json.dumps(data, ensure_ascii=False, indent=2))
    if not state.get('enabled'):
        print('提示：网球模式未启用，请检查启动参数 --tennis-config。')
    elif data.get('age_ms', 0) > 3000:
        print('提示：状态超过3秒未更新，请检查摄像头采集和推理，当前状态可能过期。')
    print('下一步：', state.get('advice', ''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--url', default='http://127.0.0.1:8080')
    parser.add_argument('--timeout', type=float, default=20)
    parser.add_argument('command', choices=['status', 'watch', 'start', 'confirm', 'pause',
                                           'calibrate_fh', 'calibrate_bh', 'save', 'load', 'reset'])
    parser.add_argument('--yes', action='store_true', help='confirm clearing samples/counts')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')
    try:
        if args.command in ('status', 'watch'):
            while True:
                show(request(args.url, '/tennis/status'))
                if args.command == 'status':
                    return 0
                time.sleep(1)
        if args.command in ('reset', 'calibrate_fh', 'calibrate_bh') and not args.yes:
            parser.error('此操作会清空计数或该类旧样本，确认后添加 --yes')
        queued = request(args.url, '/tennis/' + args.command, post=True)
        command_id = queued['id']
        print(f'命令 #{command_id} 已排队，正在等待执行结果…')
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            status = request(args.url, '/tennis/status')
            for command in status.get('commands', []):
                if command['id'] == command_id:
                    result = command['result']
                    print(('执行成功：' if result['ok'] else '执行失败：') + result['message'])
                    show(status)
                    return 0 if result['ok'] else 1
            time.sleep(.5)
        raise RuntimeError('等待超时，执行结果未知；请先查询状态和终端日志，不要立即重复发送采样命令')
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        print('排查提示：' + str(exc), file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == '__main__':
    sys.exit(main())
