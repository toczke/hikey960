#!/usr/bin/env python3
import glob, json, mmap, os, re, struct, subprocess, time

def get_temps():
    try:
        fd = os.open('/dev/mem', os.O_RDONLY | os.O_SYNC)
        mem = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=0xfff30000)
        temps = {}
        names = {0: 'a53', 1: 'a73', 2: 'gpu', 3: 'modem'}
        for s_id in range(4):
            offset = s_id * 0x40 + 0x1C
            raw = struct.unpack('<I', mem[offset:offset+4])[0]
            temps[names[s_id]] = (-63780 + raw * 205) / 1000.0
        mem.close()
        os.close(fd)
        return temps
    except Exception as e:
        return {'a73': 0.0, 'a53': 0.0, 'gpu': 0.0}

def monitor_run(cmd):
    start_temp = get_temps()
    peak_a73 = start_temp['a73']
    peak_a53 = start_temp['a53']

    t0 = time.time()
    proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)

    while proc.poll() is None:
        curr = get_temps()
        if curr['a73'] > peak_a73:
            peak_a73 = curr['a73']
        if curr['a53'] > peak_a53:
            peak_a53 = curr['a53']
        time.sleep(0.05)

    t1 = time.time()
    stdout, stderr = proc.communicate()
    end_temp = get_temps()
    duration = t1 - t0

    fps = 0.0
    fps_match = re.findall(r'fps=\s*([0-9.]+)', stderr)
    if fps_match:
        fps = float(fps_match[-1])

    frame_match = re.findall(r'frame=\s*([0-9]+)', stderr)
    frames = int(frame_match[-1]) if frame_match else 0
    if duration > 0 and fps == 0.0 and frames > 0:
        fps = frames / duration

    return {
        'duration_s': round(duration, 3),
        'frames': frames,
        'fps': round(fps, 1),
        'start_temp_a73': round(start_temp['a73'], 1),
        'peak_temp_a73': round(peak_a73, 1),
        'end_temp_a73': round(end_temp['a73'], 1),
        'peak_temp_a53': round(peak_a53, 1),
        'returncode': proc.returncode
    }

streams = sorted(glob.glob('/root/test_streams/*.*'))
print('=' * 75)
print('HiKey960 CPU Baseline Benchmark (Pure Software Decode/Encode)')
print('=' * 75)

results = {'decode': {}, 'encode': {}}

print('\n--- [1] SOFTWARE DECODING TESTS ---')
for s in streams:
    name = os.path.basename(s)
    cmd = 'ffmpeg -y -i ' + s + ' -f null -'
    res = monitor_run(cmd)
    results['decode'][name] = res
    print(f"{name:<32} | {res['frames']:>4} frames | {res['duration_s']:>6}s | {res['fps']:>6} fps | A73: {res['start_temp_a73']}°C -> peak {res['peak_temp_a73']}°C")
    time.sleep(1)

print('\n--- [2] SOFTWARE ENCODING TESTS ---')
enc_configs = [
    ('h264_720p30_x264', 'ffmpeg -y -f lavfi -i testsrc=duration=3:size=1280x720:rate=30 -c:v libx264 -preset veryfast -f null -'),
    ('h264_1080p30_x264', 'ffmpeg -y -f lavfi -i testsrc=duration=3:size=1920x1080:rate=30 -c:v libx264 -preset veryfast -f null -'),
    ('hevc_720p30_x265', 'ffmpeg -y -f lavfi -i testsrc=duration=3:size=1280x720:rate=30 -c:v libx265 -preset ultrafast -f null -'),
    ('hevc_1080p30_x265', 'ffmpeg -y -f lavfi -i testsrc=duration=3:size=1920x1080:rate=30 -c:v libx265 -preset ultrafast -f null -'),
]

for name, cmd in enc_configs:
    res = monitor_run(cmd)
    results['encode'][name] = res
    print(f"{name:<32} | {res['frames']:>4} frames | {res['duration_s']:>6}s | {res['fps']:>6} fps | A73: {res['start_temp_a73']}°C -> peak {res['peak_temp_a73']}°C")
    time.sleep(2)

with open('/root/cpu_baseline_results.json', 'w') as f:
    json.dump(results, f, indent=2)

print('\nResults saved to /root/cpu_baseline_results.json')
