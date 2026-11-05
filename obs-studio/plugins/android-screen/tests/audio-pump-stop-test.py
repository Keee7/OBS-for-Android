"""Compile the actual audio-pump section with host stubs and exercise concurrent stop.

Run: python plugins/android-screen/tests/audio-pump-stop-test.py
Requires a C11 compiler with pthreads (set CC to override gcc).
"""
import os
from pathlib import Path
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[1] / 'screen-capture.c').read_text(encoding='utf-8')
pump = source[source.index('#define AUDIO_READ_FRAMES 1024'):source.index('static void close_capture(')]
stubs = r'''
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#define UNUSED_PARAMETER(x) (void)(x)
#define LOG_INFO 0
#define LOG_WARNING 1
#define LOG_ERROR 2
#define blog(...) ((void)0)
#define DARRAY(t) struct {t *array; size_t num;}
#define DARRAY_INVALID ((size_t)-1)
#define da_push_back(a, p) do { (a).array = realloc((a).array, ((a).num + 1)*sizeof(*(a).array)); (a).array[(a).num++] = *(p); } while (0)
#define da_erase(a, i) do { memmove(&(a).array[i], &(a).array[(i)+1], ((a).num-(i)-1)*sizeof(*(a).array)); (a).num--; } while (0)
#define da_find(a, p, start) test_find((a).array, (a).num, *(p))
typedef struct source { int refs; } obs_source_t;
struct screen_source { obs_source_t *source; };
static size_t test_find(obs_source_t **a, size_t n, obs_source_t *p) { for (size_t i=0;i<n;i++) if (a[i]==p) return i; return DARRAY_INVALID; }
static atomic_int allow_read, read_done, released, started, stopped;
static obs_source_t *obs_source_get_ref(obs_source_t *p) { p->refs++; return p; }
static void obs_source_release(obs_source_t *p) {
    p->refs--; atomic_store(&released, 1);
}
typedef struct { atomic_int signaled; } os_event_t;
#define OS_EVENT_TYPE_MANUAL 0
static int os_event_init(os_event_t **e, int type) { (void)type; *e=calloc(1,sizeof(**e)); return !*e; }
static int os_event_try(os_event_t *e) { return !atomic_load(&e->signaled); }
static void os_event_signal(os_event_t *e) {
    atomic_store(&allow_read, 1);
    while (!atomic_load(&read_done)) usleep(1000);
    usleep(30000); /* the pump now attempts to take the sink lock */
    atomic_store(&e->signaled,1);
}
static void os_event_destroy(os_event_t *e) { free(e); }
static void *bmalloc(size_t n) { return malloc(n); }
#define bfree free
static uint64_t os_gettime_ns(void) { return 1; }
static void os_sleep_ms(int n) { usleep(n * 1000); }
#define AUDIO_FORMAT_16BIT 0
#define SPEAKERS_MONO 1
#define SPEAKERS_STEREO 2
struct obs_source_audio { uint8_t *data[8]; int format, speakers; uint32_t samples_per_sec, frames; uint64_t timestamp; };
static void obs_source_output_audio(obs_source_t *s, struct obs_source_audio *a) { (void)s; (void)a; }
static bool obs_android_screen_start_audio(uint32_t *r, uint32_t *c) { *r=48000; *c=2; atomic_fetch_add(&started,1); return true; }
static void obs_android_screen_stop_audio(void) { atomic_fetch_add(&stopped,1); }
static uint32_t obs_android_screen_read_audio(int16_t *b, uint32_t n) {
    (void)n;
    while (!atomic_load(&allow_read)) usleep(1000);
    b[0]=1; b[1]=1; atomic_store(&read_done,1); return 1;
}
'''
test = r'''
static void *release_thread(void *p) { audio_pump_release(p); return NULL; }
int main(void) {
    obs_source_t source = { .refs=1 };
    struct screen_source capture = { .source=&source };
    audio_pump_acquire(&capture);
    assert(atomic_load(&started)==1 && source.refs==2);
    pthread_t t; assert(!pthread_create(&t,NULL,release_thread,&capture));
    assert(!pthread_join(t,NULL));
    assert(atomic_load(&released) && atomic_load(&stopped)==1);
    assert(!audio_thread_started && !audio_buf && !audio_stop && !audio_users && source.refs==1);
    audio_pump_release(&capture); /* duplicate release is harmless */
    assert(atomic_load(&stopped)==1);
    free(audio_sinks.array);
    return 0;
}
'''
with tempfile.TemporaryDirectory() as tmp:
    c = Path(tmp) / 'test.c'
    exe = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
    c.write_text(stubs + pump + test, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-D_DEFAULT_SOURCE', '-pthread', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=5)
    # Negative control: recreate the original lock-around-stop ordering and
    # verify the same scenario actually detects the deadlock.
    old = '''\tpthread_mutex_unlock(&audio_mx);
\tif (stop)
\t\taudio_pump_stop();'''
    broken = '''\tif (stop)
\t\taudio_pump_stop();
\tpthread_mutex_unlock(&audio_mx);'''
    if old not in pump:
        raise RuntimeError('Could not construct deadlock negative control')
    c.write_text(stubs + pump.replace(old, broken, 1) + test, encoding='utf-8')
    subprocess.run([os.environ.get('CC', 'gcc'), '-std=c11', '-D_DEFAULT_SOURCE', '-pthread', str(c), '-o', str(exe)], check=True)
    try:
        subprocess.run([str(exe)], check=True, timeout=2)
    except subprocess.TimeoutExpired:
        pass
    else:
        raise AssertionError('Deadlock negative control unexpectedly completed')
print('audio pump concurrent stop: PASS (deadlock negative control detected)')
